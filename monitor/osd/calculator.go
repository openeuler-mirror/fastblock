/* Copyright (c) 2023-2024 ChinaUnicom
 * fastblock is licensed under Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *          http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PSL v2 for more details.
 */
package osd

import (
	"context"
	"encoding/json"
	"errors"
	"math/rand"
	"monitor/config"
	"monitor/etcdapi"
	"monitor/log"
	"sort"
	"strconv"
	"time"
)

// OptimizeCfg to pass params.
type OptimizeCfg struct {
	OSDTree        *[]*DomainNode
	OSDInfoMap     *map[OSDID]*TreeNode
	PGCount        int
	PGSize         int
	TotalWeight    float64
	PreviousPGList map[string]PGConfig // map key "1", "2", "3", ...
}

type OptimizeResult struct {
	PoolID
	OptimizedPgMap PoolPGsConfig
}

// TreeNode TODO: support other type.
type TreeNode struct {
	OSDID         OSDID // osd id, like 1, 2, 3, ...
	Weight        float64
	UniformWeight float64
}

// DomainNode for failure domain.
type DomainNode struct {
	DomainName string // TODO, it can be other type.
	ChildNode  []*TreeNode
}

// GetID return domain name.
func (d *DomainNode) GetID() string {
	return string(d.DomainName)
}

// SortList for sort only. TODO.
type SortList struct {
	Slice *[]*TreeNode
	By    func(a, b interface{}) bool
}

func (l SortList) Len() int      { return len(*l.Slice) }
func (l SortList) Swap(i, j int) { (*l.Slice)[i], (*l.Slice)[j] = (*l.Slice)[j], (*l.Slice)[i] }

func (l SortList) Less(i, j int) bool {
	return l.By((*l.Slice)[i], (*l.Slice)[j])
}

// SortHostList for sort only. TODO.
type SortHostList []*DomainNode

func (l SortHostList) Len() int      { return len(l) }
func (l SortHostList) Swap(i, j int) { l[i], l[j] = l[j], l[i] }

type ByHostID struct{ SortHostList }

// Less for sort only. TODO.
func (l ByHostID) Less(i, j int) bool {
	return l.SortHostList[i].GetID() < l.SortHostList[j].GetID()
}

// FlattenTree change a topology tree to a flatten tree needed by later optimize calculation.
// Example:
//
//	{
//	  dom1: { '1': 0.873065710067749, '4': 0.873065710067749 },
//	  dom2: { '2': 0.873065710067749, '5': 0.873065710067749 },
//	  dom3: { '3': 0.873065710067749, '6': 0.873065710067749 }
//	}
//
// 2、3参数来自 osdTreeMap, osdNodeMap, osdRevision, terr := cluster.GetOSDTreeUp(ctx)，见 crush.go:55
// 其中2参数其实是所有level的总树，即allTreeMap
func FlattenTree(ctx context.Context, treeMap *map[Level]*map[string]*BucketTreeNode, osdNodeMap *map[string]OSDTreeNode, pgCount, pgSize int, failureDomain string, poolRoot string, poolRootEnable bool) (*[]*DomainNode, *map[OSDID]*TreeNode, float64) {
	if treeMap == nil || osdNodeMap == nil {
		log.Error(ctx, "Invalid tree input:", treeMap, osdNodeMap)
		return nil, nil, 0
	}
	if failureDomain == "" {
		log.Error(ctx, "no failureDomain specified!")
		return nil, nil, 0
	}

	osdInfoMap := make(map[OSDID]*TreeNode)
	var weight float64

	// host / rack / root ... to domain.

	var tree *map[string]*BucketTreeNode
	treeBuf := make(map[string]*BucketTreeNode, 0)

	log.Info(ctx, "RootEnable:", poolRootEnable, ", pool.Root:", poolRoot, ", failureDomain:", failureDomain)
	if !poolRootEnable {
		//如果不指定 pool.root，也使用 failureDomain 对应的 level
		tree = (*treeMap)[Level(failureDomain)]
		log.Warn(ctx, "no root specified, use failureDomain level:", failureDomain)
	} else {
		//如果指定了pool.root，遍历pool.root树下所有osd。先放在treeBuf中，然后把地址给tree变量
		if hostTreeMap, ok := (*treeMap)[Level("host")]; ok {
			log.Info(ctx, "search host Tree Map:", *hostTreeMap)
			if poolRootTree, ok3 := (*hostTreeMap)[poolRoot]; ok3 {
				log.Info(ctx, "found", poolRoot, "in level host")
				poolRootTraversal(ctx, poolRootTree, &treeBuf, failureDomain)
				tree = &treeBuf
				goto treeHasGot
			} else {
				log.Info(ctx, "cannot found", poolRoot, "in level host")
			}
		}

		if rackTreeMap, ok := (*treeMap)[Level("rack")]; ok {
			log.Info(ctx, "search rack Tree Map:", *rackTreeMap)
			if poolRootTree, ok := (*rackTreeMap)[poolRoot]; ok {
				log.Info(ctx, "found", poolRoot, "in level rack")
				poolRootTraversal(ctx, poolRootTree, &treeBuf, failureDomain)
				tree = &treeBuf
				goto treeHasGot
			} else {
				log.Info(ctx, "cannot found", poolRoot, "in level rack")
			}
		}

		if rootTreeMap, ok := (*treeMap)[Level("root")]; ok {
			log.Info(ctx, "search root Tree Map:", *rootTreeMap)
			if poolRootTree, ok := (*rootTreeMap)[poolRoot]; ok {
				log.Info(ctx, "found", poolRoot, "in level root")
				poolRootTraversal(ctx, poolRootTree, &treeBuf, failureDomain)
				tree = &treeBuf
				goto treeHasGot
			} else {
				log.Info(ctx, "cannot found", poolRoot, "in level root")
			}
		}

		//如果没找到 pool.root，也使用 failureDomain 对应的 level
		tree = (*treeMap)[Level(failureDomain)]
		log.Warn(ctx, "cannot found", poolRoot, "in any level, use failureDomain level:", failureDomain)
	}

treeHasGot:

	if tree == nil || len(*tree) == 0 {
		log.Warn(ctx, "Invalid tree for failure domain:", failureDomain)
		return nil, nil, 0
	}

	log.Info(ctx, "tree", *tree)
	domainTree := make([]*DomainNode, len(*tree)) // * for sort. TODO: is sort necessary?
	// 到这里tree表示的是满足failure domain的map，其中每一个节点下可以选择一个osd
	// 如果failure domain是host，tree就类似 host1:[1 2 3], host2:[4 5 6]
	// 如果failure domain是osd， tree就类似 1:[1], 2:[2], 3:[3]
	// 下面range的目的就是把每个root下的osd，从osdNodeMap中提取并展开成一个数组，方便后面随机取一个
	i := 0
	for id, root := range *tree {
		// 来自BucketTreeNode.BucketName，是一个string
		domainTree[i] = &DomainNode{DomainName: id}
		domainTree[i].ChildNode = make([]*TreeNode, 0)

		addOSDNode(root, osdNodeMap, &domainTree[i].ChildNode, &osdInfoMap, &weight)

		sortedList := SortList{
			By: func(a, b interface{}) bool {
				n1, n2 := a.(*TreeNode), b.(*TreeNode)
				if n1.Weight != n2.Weight {
					return n1.Weight < n2.Weight
				}
				return n1.OSDID < n2.OSDID
			},
		}
		sortedList.Slice = &domainTree[i].ChildNode
		sort.Sort(sortedList)

		log.Debug(ctx, "osd list for", id, ":")
		for _, osd := range domainTree[i].ChildNode {
			if osd != nil {
				log.Debug(ctx, *osd)
			}
		}

		i++
	}

	// Uniform weight.
	// For example, for osd[1, 2, 3], pg count 16, pg size is 1, then uniform weight should be 5.3333... for each osd.
	// TODO: how to handle host1: [1,4], host2: [2], host3: [3]?
	for _, osd := range osdInfoMap {
		osd.UniformWeight = getUniformWeight(osd.Weight, weight, pgCount, pgSize)
		log.Debug(ctx, "osd", osd.OSDID, "weight:", osd.Weight, osd.UniformWeight, weight, "pg:", pgCount, pgSize)
	}

	// Order doesn't matter in fact, but keep order here for easier debug.
	sort.Sort(ByHostID{domainTree})

	log.Info(ctx, "FlattenTree done:", domainTree)

	return &domainTree, &osdInfoMap, weight
}

/**
 * 遍历指定pool.root（创建时配置）下的root-rack-host-osd树，
 * 当遍历到failureDomain level，会把当前的BucketTreeNode放在出参tree中
 * 如果failureDomain = host，则输出的都是host的BucketTreeNode，表示可以从这个host下选一个osd
 * 如果failureDomain = ost， 则输出的都是osd 的BucketTreeNode，表示可以从这个osd 下选一个osd
 */
func poolRootTraversal(ctx context.Context, poolRoot *BucketTreeNode, tree *map[string]*BucketTreeNode, failureDomain string) {
	if poolRoot == nil {
		return
	}

	log.Info(ctx, "poolRootTraversal:", poolRoot.BucketName, "level:", poolRoot.Level)
	if string(poolRoot.Level) == failureDomain {
		(*tree)[poolRoot.BucketName] = poolRoot
		log.Debug(ctx, "*tree:", *tree)
		return
	}

	for _, child := range poolRoot.ChildNode {
		poolRootTraversal(ctx, child, tree, failureDomain)
	}
}

/**
 * root来自poolRootTraversal输出的tree。
 * 此函数会把root展开，把下面的osd都放进osdList数组中。方便后续从中随机取一个osd。
 *
 * 如果failureDomain = osd， 则root就是一个osd  level的BucketTreeNode
 * 如果failureDomain = host，则root就是一个host level的BucketTreeNode
 */
func addOSDNode(root *BucketTreeNode, osdNodeMap *map[string]OSDTreeNode,
	osdList *[]*TreeNode, osdInfoMap *map[OSDID]*TreeNode, weight *float64) {
	if root == nil || osdList == nil || weight == nil {
		return
	}

	if root.Level.IsOSD() {
		osd, ok := (*osdNodeMap)[root.BucketName]
		if !ok {
			return
		}
		treeNode := &TreeNode{
			OSDID:  osd.OSDID,
			Weight: osd.Weight,
		}
		*osdList = append(*osdList, treeNode)
		(*osdInfoMap)[osd.OSDID] = treeNode
		*weight = *weight + osd.Weight

		return
	}

	for _, child := range root.ChildNode {
		addOSDNode(child, osdNodeMap, osdList, osdInfoMap, weight)
	}
}

func getUniformWeight(weight float64, totalWeight float64, pgCount int, pgSize int) float64 {
	return weight * float64(pgCount*pgSize) / totalWeight
}

// RecheckPGs calculate
func RecheckPGs(ctx context.Context, client *etcdapi.EtcdClient) {
	recheckTick := time.NewTicker(10 * time.Second)
	for range recheckTick.C {
		log.Info(ctx, "RecheckPgs enter.")

		if AllPools == nil {
			log.Warn(ctx, "AllPoolsConfig nil!")
			return
		}
		log.Info(ctx, "All pools config:", AllPools)

		newOsdMapVersion := AllOSDInfo.Version
		// A new pool or a pool config/pgs changed.
		changed := false

		// TODO: osd revision can be in smaller granular.
		osdTreeMap, osdNodeMap, terr := GetOSDTreeUp(ctx)
		if terr != nil {
			log.Error(ctx, "GetOSDTreeUp failed!")
			return
		}

		// If a pool is deleted, then the result won't contain its PG configs.
		// TODO: hash for each root and pool, and recheck only certain pools.
		for poolID, pool := range AllPools {
			pgNum := 0
			var oldPGs map[string]PGConfig
			var pgRevision int64
			if pool.PoolPgMap.PgMap != nil {
				pgNum = len(pool.PoolPgMap.PgMap)
				oldPGs = pool.PoolPgMap.PgMap
			}

			//(fixme)
			optimizeCfg := &OptimizeCfg{
				PGCount:        pool.PGCount,
				PGSize:         getEffectivePGSize(pool.PGSize, GetFailureDomainNum(osdTreeMap, pool.FailureDomain)),
				PreviousPGList: oldPGs,
			}

			if len(*osdNodeMap)*2 < optimizeCfg.PGSize {
				continue
			}

			optimizeCfg.OSDTree, optimizeCfg.OSDInfoMap, optimizeCfg.TotalWeight = FlattenTree(ctx, osdTreeMap, osdNodeMap, pool.PGCount, pool.PGSize, pool.FailureDomain, pool.Root, pool.Root != "")

			var poolPGResult *OptimizeResult
			var oerr error
			if pgNum == 0 {
				poolPGResult, oerr = SimpleInitial(ctx, optimizeCfg)
			} else {
				//(fixme)we respect the orginal pg distribution for now
				poolPGResult, oerr = SimpleChange(ctx, optimizeCfg, newOsdMapVersion)
			}

			// Something wrong, so just keep old PG configs.
			if oerr != nil {
				log.Error(ctx, oerr, "input:", *optimizeCfg)
				continue
			}

			//seems no need to change now, we respect the original distribution
			if poolPGResult != nil {
				log.Warn(ctx, "pool", poolID, ":", pool.Name, "result:", poolPGResult)
				for pgId, pg := range poolPGResult.OptimizedPgMap.PgMap {
					log.Warn(ctx, "pgId: ", pgId, pg)
				}

				log.Warn(ctx, "------------")
				if len(oldPGs) > 0 {
					for pgId, pg := range oldPGs {
						log.Warn(ctx, "pgId: ", pgId, pg)
					}
				}

				poolPGResult.PoolID = poolID
				if !isSamePGConfig(ctx, poolPGResult, &oldPGs, pgRevision) {
					changed = true
					log.Warn(ctx, "PG map is changed!")
					AllPools[poolID].PoolPgMap.PgMap = poolPGResult.OptimizedPgMap.PgMap
					AllPools[poolID].PoolPgMap.Version++
					if err := saveNewPGsTxn(ctx, client, poolID); err != nil {
						log.Error(ctx, err)
					}
				}
			}

			//if !(changed || deleted) consider deleted pool
			if !(changed) {
				log.Warn(ctx, "PG map not changed!")
			}
		}

		osdmapVersion = newOsdMapVersion
	}

}

// saveNewPGsTxn save all to /config/pgmap/poolID, output.
func saveNewPGsTxn(ctx context.Context, client *etcdapi.EtcdClient, pid PoolID) error {
	log.Info(ctx, "saveNewPGsTxn")

	configPGBuffer, err := json.Marshal(AllPools[pid])
	if err != nil {
		log.Error(ctx, err)
		return err
	}

	poolKey := config.ConfigPoolsKeyPrefix + strconv.Itoa(int(pid))
	if rerr := client.Put(ctx, poolKey, string(configPGBuffer)); rerr != nil {
		log.Error(ctx, rerr)
		return rerr
	}

	log.Info(ctx, "key:", poolKey, ", value:", string(configPGBuffer))
	log.Warn(ctx, "pool", pid, "PG committed!")

	return nil
}

func getEffectivePGSize(pgSize, domainNum int) int {
	return config.Ternary(pgSize <= domainNum, pgSize, domainNum).(int)
}

// isSamePGConfig checks if the new and old PG configurations are the same.
// It takes in the new PG configuration, old PG configuration, and revision number as arguments.
// It returns a boolean indicating whether the configurations are the same.
func isSamePGConfig(ctx context.Context, newPGConfig *OptimizeResult, oldPGConfig *map[string]PGConfig, revision int64) bool {
	if newPGConfig == nil && oldPGConfig == nil {
		return true
	}

	if newPGConfig == nil || oldPGConfig == nil {
		return false
	}

	if len(newPGConfig.OptimizedPgMap.PgMap) != len(*oldPGConfig) {
		return false
	}

	for pgid, pgCfg := range newPGConfig.OptimizedPgMap.PgMap {
		if oldPgCfg, ok := (*oldPGConfig)[pgid]; !ok {
			return false
		} else {
			osdlist := pgCfg.OsdList

			if compare_arry(osdlist, oldPgCfg.OsdList) == false {
				return false
			}

			// if len(oldPgCfg.OsdList) < len(osdlist) {
			// return false
			// }
			// for osdIndex, osdID := range oldPgCfg.OsdList {
			//
			// if osdIndex >= len(osdlist) {
			// return false
			// }
			// if osdID != int(osdlist[osdIndex]) {
			// return false
			// }
			// }
		}
	}

	return true
}

func compare_arry(arr1 []int, arr2 []int) bool {
	mp1 := make(map[int]int)

	if len(arr1) != len(arr2) {
		return false
	}

	for _, val := range arr1 {
		mp1[val] = 1
	}

	for _, val := range arr2 {
		_, ok := mp1[val]
		if ok == false {
			return false
		}
	}
	return true
}

/**
 * 参数中cfg.OSDTree来自FlattenTree的domainTree，见 crush.go:99
 *    其中每个元素表示一个osd数组，其定义如下：
 *       domainTree[i] = &DomainNode{DomainName: id}
 *       domainTree[i].ChildNode = make([]*TreeNode, 0)
 *   如果failure domain是host, OSDTree就类似 host1:[1 2 3], host2:[4 5 6]
 *   如果failure domain是osd,  OSDTree就类似 1:[1], 2:[2], 3:[3]
 * 为了方便描述，下面注释把OSDTree的key都统称为host，表示failure domain为host。
 * 注释中假设pg_size为3。
 */
func SimpleInitial(ctx context.Context, cfg *OptimizeCfg) (*OptimizeResult, error) {
	if cfg == nil || cfg.OSDTree == nil || cfg.OSDInfoMap == nil || len(*cfg.OSDTree) == 0 || len(*cfg.OSDInfoMap) == 0 || len(*cfg.OSDTree)*2 < cfg.PGSize {
		return nil, errors.New("invalid input cfg")
	}

	if int(cfg.PGSize) > len(*cfg.OSDTree) {
		// 如果pg size比host数还大，没法分
		return nil, errors.New("PGSize > OSDTree, no enough hosts")
	}
	log.Info(ctx, "SimpleInitial cfg:", *cfg)

	// 把cfg中的参数拿出来
	pg_count := int(cfg.PGCount)
	pg_size := int(cfg.PGSize)
	host_count := len(*cfg.OSDTree)
	pg_per_host := pg_count * pg_size / host_count // 平均值，预计每个host上承担几个pg，向下取整

	/**
	 *  Step 1: 先均匀选取host，以后可以从host下均匀选取osd
	 */
	// 二维数组，存放每个pg下的选取的3个host的索引。
	pg_host_array := make([][]int, pg_count)
	for i := range pg_host_array {
		pg_host_array[i] = make([]int, pg_size)
	}
	// 遍历赋初值-1
	for i := 0; i < pg_count; i++ {
		for j := 0; j < pg_size; j++ {
			pg_host_array[i][j] = -1
		}
	}

	// 因为每个pg下的数组里第一个元素更可能成为leader，所以一定要保证随机
	leader_host_idx := getRandIndex(host_count, pg_count)
	log.Debug(ctx, "leader_host_idx:", leader_host_idx)
	for i := 0; i < pg_count; i++ {
		pg_host_array[i][0] = leader_host_idx[i]
	}

	// 每个pg的除第一个外，剩下的元素就完全随机生成
	rand.Seed(time.Now().UnixNano())
	for i := 0; i < pg_count; i++ {
		for j := 1; j < pg_size; j++ {
			for {
				rand_idx := rand.Intn(host_count)
				if !contains(pg_host_array[i], rand_idx, 0, j) { // 需要跟j前面的不重复
					pg_host_array[i][j] = rand_idx
					break
				}
			}
		}
	}
	log.Debug(ctx, "pg_host_array:", pg_host_array)

	// 统计每个host上目前有几个pg
	pg_num_on_host := make([]int, host_count)

	pg_on_host_map := make(map[int][]int, 0) // 每个host上到底有哪几个pg
	for i := 0; i < pg_count; i++ {
		for j := 0; j < pg_size; j++ {
			host_idx := pg_host_array[i][j]
			pg_num_on_host[host_idx]++
			pg_on_host_map[host_idx] = append(pg_on_host_map[host_idx], i)
		}
	}
	log.Info(ctx, "Step 1 done. pg_num_on_host:", pg_num_on_host, "pg_per_host", pg_per_host)

	/**
	 *  Step 2: 如果host还不够均衡，这里应该把他们手动均衡
	 *     示例：pg_num_on_host: [58 68 68 58 62 70]，则最多的是host 5(70个)，最少的是host 0(58个)。
	 *          遍历host 5上的所有pg，找到一个可以迁移走的（包含host 5但不包含host 0），例如[1,2,5]，
	 *          迁移之后变为[1,2,0]。对应的pg_num_on_host也要修改：[59 68 68 58 62 69]
	 *			但如果遍历host 5上每个pg都不满足条件，就break掉。因为会一直循环，没有意义。
	 */
	break_flag := true                       // 如果遍历完一个host都没有完成修改，那么下次循环还是这个host，那就break掉
	for loop := 0; loop < pg_count; loop++ { // loop主要是为了限制循环次数，避免陷入死循环
		// 查找承担pg最多和最少的host
		max_host := max_index(pg_num_on_host[:]) // 使用切片传递参数，避免拷贝
		min_host, second_min_host, third_min_host := top3_min_index(pg_num_on_host[:])
		log.Debug(ctx, "min:", pg_num_on_host[min_host], "max:", pg_num_on_host[max_host])

		break_flag = true
		if pg_num_on_host[min_host] < pg_per_host ||
			pg_num_on_host[max_host] > pg_per_host+1 { // host上的pg数，只能处于平均值 或 平均值+1

			// 遍历承担pg最多的host上面的每个pg
			for i := 0; i < len(pg_on_host_map[max_host]); i++ {
				pg_id := pg_on_host_map[max_host][i] // 当前遍历到的pg
				// 如果这个pg中：包含 max_host 但不包含 min_host，就可以把 max_host 修改为 min_host
				if contains(pg_host_array[pg_id], max_host, 1, -1) {
					if transfer_one(ctx,
						pg_host_array[pg_id],
						max_host,
						min_host, second_min_host, third_min_host,
						pg_num_on_host[:]) {
						break_flag = false
						// 如果在这个host上已经成功迁移，那就没必要在这个host再进行下去了。break掉进行下一个host。
						break
					}
				}

			}
		}

		if break_flag {
			break
		}
	}
	log.Info(ctx, "Step 2 done. pg_num_on_host:", pg_num_on_host)

	/**
	 *  Step 3: 从host分配osd。如果failure domain是osd，那么每个host下只有一个osd，即索引都为0
	 */
	host_queue := make([]Queue, host_count) // 每个host一个queue，用来弹出osd
	for i := 0; i < host_count; i++ {
		domain_node := (*cfg.OSDTree)[i]
		if domain_node == nil {
			return nil, errors.New("domain_node pointer invalid")
		}
		osd_size := len((*domain_node).ChildNode) // 这个host下有几个osd
		// 这个host需要几个pg，即需要从host中弹出几个osd。先算出随机的osd索引，然后放进queue里面
		pg_on_host := pg_num_on_host[i]
		rand_osd_idx := getRandIndex(osd_size, pg_on_host)
		for _, osd_idx := range rand_osd_idx {
			host_queue[i].Push(osd_idx)
		}
	}
	log.Info(ctx, "Step 3 done.")

	/**
	 *  Step 4: 构造返回值
	 */
	result := OptimizeResult{
		OptimizedPgMap: PoolPGsConfig{PgMap: make(map[string]PGConfig), Version: 1},
	}

	oldPGs := cfg.PreviousPGList

	for i := 0; i < pg_count; i++ {
		var ppc PGConfig

		for j := 0; j < pg_size; j++ {
			// 对于每个pg 每个位置，都从上面计算出的host中，pop一个osd出来
			host_idx := pg_host_array[i][j]
			osd_idx := host_queue[host_idx].Pop()

			domain_node := (*cfg.OSDTree)[host_idx]
			if domain_node == nil {
				return nil, errors.New("domain_node pointer invalid")
			}

			// 找到这个osd对应的TreeNode
			tree_node := (*domain_node).ChildNode[osd_idx]
			if tree_node == nil {
				return nil, errors.New("tree_node pointer invalid")
			}
			ppc.OsdList = append(ppc.OsdList, int((*tree_node).OSDID))
		}

		oldPgCfg, ok := oldPGs[strconv.Itoa(i)]
		if ok == false {
			ppc.Version = 1
		} else {
			old_osdlist := oldPgCfg.OsdList
			equal := compare_arry(old_osdlist, ppc.OsdList)
			if equal == true {
				ppc.Version = oldPgCfg.Version
			} else {
				ppc.Version = oldPgCfg.Version + 1
			}
		}

		result.OptimizedPgMap.PgMap[strconv.Itoa(i)] = ppc
	}
	log.Info(ctx, "Step 4 done.")

	return &result, nil
}

// check whether we should respect the original pg distribution
func SimpleChange(ctx context.Context, cfg *OptimizeCfg, newOsdMapVersion int64) (*OptimizeResult, error) {
	if cfg == nil || cfg.OSDTree == nil || cfg.OSDInfoMap == nil || len(*cfg.OSDTree) == 0 || len(*cfg.OSDInfoMap) == 0 {
		return nil, errors.New("invalid input cfg")
	}
	if int(cfg.PGSize) > len(*cfg.OSDTree) { // 如果pg size比host数还大，没法分
		log.Error(ctx, "PGSize: ", cfg.PGSize, " > OSDTree !", len(*cfg.OSDTree))
		return nil, errors.New("PGSize > OSDTree")
	}

	if osdmapVersion >= newOsdMapVersion {
		return nil, nil
	}
	log.Warn(ctx, "osdmapVersion: ", osdmapVersion, "AllOSDInfo.Version :", newOsdMapVersion)

	log.Warn(ctx, "SimpleChange cfg:", *cfg)
	//check whether we should respect the original pg distribution
	// return nil, nil

	return SimpleInitial(ctx, cfg)
}

/**
 * 完成一次pg迁移。把pg里最忙碌的host，迁移到最轻松的host上。最轻松的host有3个候选。
 *
 * 参数：
 *    pg: 数组，是pg里面的成员。例： [1, 3, 5]
 *    max_host: 承担pg最多的host。例: 5
 *    min_host, second_min_host, third_min_host: 承担pg最少的3个host，作为候选
 *    pg_num_on_host: 转移结束之后，更新状态使用。
 *
 * 返回值：bool，表示是否进行了一次迁移。
 */
func transfer_one(ctx context.Context, pg []int, max_host int, min_host int, second_min_host int, third_min_host int, pg_num_on_host []int) bool {
	idx := find(pg, max_host, 1, -1)

	if !contains(pg, min_host, 0, -1) { // 只要pg中min_host没有出现过，比如[1,3,5]不能把5迁移到1上
		log.Debug(ctx, "pg:", pg, "max_host", max_host, "min_host", min_host)
		pg[idx] = min_host // 然后将其换成min_host
		log.Debug(ctx, "pg:", pg)

		pg_num_on_host[max_host]--
		pg_num_on_host[min_host]++
		return true
	} else if !contains(pg, second_min_host, 0, -1) { // 最小的不满足条件，就尝试第二小的
		log.Debug(ctx, "pg:", pg, "max_host", max_host, "second_min_host", second_min_host)
		pg[idx] = second_min_host
		log.Debug(ctx, "pg:", pg)

		pg_num_on_host[max_host]--
		pg_num_on_host[second_min_host]++
		return true
	} else if !contains(pg, third_min_host, 0, -1) { // 第二小的不满足条件，就尝试第三小的
		log.Debug(ctx, "pg:", pg, "max_host", max_host, "third_min_host", third_min_host)
		pg[idx] = third_min_host
		log.Debug(ctx, "pg:", pg)

		pg_num_on_host[max_host]--
		pg_num_on_host[third_min_host]++
		return true
	}

	return false
}

/**
 * 获取平均的随机索引。
 * 参数：candidate 一共有多少个待选元素。
 *      need 需要选出多少个待选元素。
 * 返回值： 一个索引数组。每个数字都不超过candidate数。
 * 示例：candidate=3, need=14, 从3个候选者中返回11个索引，尽量平均
 *         14 / 3 = 4 ··· 2
 *      先把candidata循环4次 [0 1 2,  0 1 2,  0 1 2,  0 1 2]
 *      然后剩下2个不能循环的，从3里随机选取2个，如随机选中[0 2]
 */
func getRandIndex(candidate int, need int) []int {
	ans := make([]int, need)
	div := need / candidate        // 除法
	remain := need - div*candidate // 余数

	// 循环div次
	for i := 0; i < div; i++ {
		for j := 0; j < candidate; j++ {
			ans[i*candidate+j] = j
		}
	}

	// 随机选取剩下remain个
	rand.Seed(time.Now().UnixNano())
	rand_idx := rand.Perm(candidate)
	for j := 0; j < remain; j++ {
		ans[div*candidate+j] = rand_idx[j]
	}
	// 可以打乱顺序。
	/* rand.Shuffle(len(ans), func(i, j int) {
		ans[i], ans[j] = ans[j], ans[i]
	}) */
	return ans
}

// array： 在begin和end位置之间，是否有和val相同的元素
// end:    结束位置。负数表示默认end = array的原始长度
func contains(array []int, val int, begin int, end int) bool {
	if end < 0 {
		end = len(array)
	}

	for i := begin; i < len(array) && i < end; i++ {
		if array[i] == val {
			return true
		}
	}
	return false
}

// array： 在begin和end位置之间，和val相同的元素的位置
// end:    结束位置。负数表示默认end = array的原始长度
// except: 除了这个位置之外。负数表示没有except
func find(array []int, val int, begin int, end int) int {
	if end < 0 {
		end = len(array)
	}

	for i := begin; i < len(array) && i < end; i++ {
		if array[i] == val {
			return i
		}
	}
	return -1
}

// 与最大值不同，求最小值时要求最小的3个，作为3个候选
func top3_min_index(array []int) (int, int, int) {
	min_num := 2147483647        // 最小的数字
	second_min_num := 2147483647 // 第二小的数字
	third_min_num := 2147483647  // 第三小的数字
	min_index := -1              // 最小数字的索引
	second_min_index := -1       // 第二小数字的索引
	third_min_index := -1        // 第三小数字的索引
	for i := 0; i < len(array); i++ {
		if array[i] < min_num {
			third_min_num, third_min_index = second_min_num, second_min_index
			second_min_num, second_min_index = min_num, min_index
			min_num, min_index = array[i], i
		} else if array[i] >= min_num && array[i] < second_min_num {
			third_min_num, third_min_index = second_min_num, second_min_index
			second_min_num, second_min_index = array[i], i
		} else if array[i] >= second_min_num && array[i] < third_min_num {
			third_min_num, third_min_index = array[i], i
		}
	}
	return min_index, second_min_index, third_min_index
}

func max_index(array []int) int {
	max_num := -2147483648
	index := -1
	for i := 0; i < len(array); i++ {
		if array[i] > max_num {
			max_num = array[i]
			index = i
		}
	}
	return index
}

/**
 * go语言没有queue，实现一个
 */
type Queue []int

// Pushes the element into the queue.
//
//	e.g. q.Push(123)
func (q *Queue) Push(v int) {
	*q = append(*q, v)
}

// Pops element from head.
func (q *Queue) Pop() int {
	head := (*q)[0]
	*q = (*q)[1:]
	return head
}

// Returns if the queue is empty or not.
func (q *Queue) IsEmpty() bool {
	return len(*q) == 0
}
