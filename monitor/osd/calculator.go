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
	"math"
	"math/rand"
	"monitor/config"
	"monitor/etcdapi"
	"monitor/log"
	"monitor/utils"
	"sort"
	"strconv"
	"time"
	"fmt"
)

// OptimizeCfg to pass params.
type OptimizeCfg struct {
	OSDTree        *[]*DomainNode
	OSDInfoMap     *map[OSDID]*TreeNode
	PGCount        int
	PGSize         int
	TotalWeight    float64
	FailureDomain  string
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

func CreatePgs(ctx context.Context, client *etcdapi.EtcdClient, pool *PoolConfig) (*map[string]PGConfig, error) {
    if pool == nil {
        log.Warn(ctx, "pool nil!")
        return nil, errors.New("pool is nil.")
    }
    
    osdTreeMap, osdNodeMap, terr := GetOSDTree(ctx, false, true)
    if terr != nil {
        log.Error(ctx, "GetOSDTree failed!")
        return nil, errors.New("GetOSDTree failed.")
    }

    optimizeCfg := &OptimizeCfg{
        PGCount:        pool.PGCount,
        PGSize:         getEffectivePGSize(pool.PGSize, GetFailureDomainNum(osdTreeMap, pool.FailureDomain)),
        PreviousPGList: make(map[string]PGConfig),
    }

    log.Info(ctx, "osdNodeMap size:", len(*osdNodeMap), ", PGSize:", optimizeCfg.PGSize)
    if len(*osdNodeMap) < optimizeCfg.PGSize || optimizeCfg.PGSize == 0{
        log.Error(ctx, "Too few osd nodes.")
        return nil, errors.New("Too few osd nodes.")
    }

    optimizeCfg.OSDTree, optimizeCfg.OSDInfoMap, optimizeCfg.TotalWeight = FlattenTree(ctx, osdTreeMap, osdNodeMap, 
            pool.PGCount, pool.PGSize, pool.FailureDomain, pool.Root, pool.Root != "")
    
    poolPGResult, oerr := SimpleInitial(ctx, optimizeCfg, pool.PGSize)
    if oerr != nil {
        log.Error(ctx, oerr, "create pg failed.")
        return nil, errors.New("create pg failed.")
    }

	// log.Warn(ctx, "pool", pool.Poolid, ":", pool.Name, "result:", poolPGResult)
    return &poolPGResult.OptimizedPgMap.PgMap, nil
}

//pg的成员列表osdList，osdid表示的osd由down变为up，是否可以触发pg remap
func shouldChange(osdList []int, pgSize int, osdid int) bool {
    var downNum int
    var outNum int
    var downOutNum int

    osdIsDownOut := false
    for _, id := range osdList {
        osdInfo, ok := AllOSDInfo.Osdinfo[OSDID(id)]
        if ok == true {
            isDown := false
            if !osdInfo.IsUp {
                downNum++
                isDown = true
            }
            if !osdInfo.IsIn {
                outNum++
                if isDown {
                    downOutNum++
                    if osdid == id {
                        osdIsDownOut = true
                    }
                }
            }
        }
    }	

    if downNum >= 2  &&  (downNum - 1) * 2 < pgSize && downNum * 2 >= pgSize{
        //down  to  Undersize
        if outNum >= 1 {
            if outNum == 1 && osdIsDownOut {
				//唯一的处于out状态的osd变为in
                return false
            }
            return true
        }
    }
    return false
}

func getDomainPg(cfg *OptimizeCfg, poolid PoolID) (*map[string]int32, *map[string][]int, *map[int]int32, *map[int][]string, *map[int]string) {
	domainPgNum := make(map[string]int32)
	pgOnDomainMap := make(map[string][]int)
	pgNumPerOsd := make(map[int]int32)
	pgDomainMap := make(map[int][]string)
	osdDomainMap := make(map[int]string)

	for _, domain := range *(cfg.OSDTree) {
		domainPgNum[domain.DomainName] = 0
		pgOnDomainMap[domain.DomainName] = make([]int, 0)
		for _, osd := range domain.ChildNode {
			osdDomainMap[int(osd.OSDID)] = domain.DomainName
			pgNumPerOsd[int(osd.OSDID)] = 0
		}
	}

	pgCount := AllPools[poolid].PGCount
	for pgid := 0; pgid < pgCount; pgid++ {
		pg := AllPools[poolid].PoolPgMap.PgMap[strconv.Itoa(pgid)]
		for _, osdId := range pg.OsdList {
			domain := osdDomainMap[osdId]
			domainPgNum[domain]++
			pgOnDomainMap[domain] = append(pgOnDomainMap[domain], pgid)
			pgDomainMap[pgid] = append(pgDomainMap[pgid], domain)
			pgNumPerOsd[osdId]++
		}
	}

    return &domainPgNum, &pgOnDomainMap, &pgNumPerOsd, &pgDomainMap, &osdDomainMap
}

//检测domain是否在pg中
func containsDomain(pgDomainMap *map[int][]string, pgId int, domain string) bool{
	for _, value := range (*pgDomainMap)[pgId] {
		if value == domain {
			return true
		}
	}
	return false
}

//检测pg是否可以把domain从成员列表中移除
func transferable(poolid PoolID, pgId int) bool {
	pg := AllPools[poolid].PoolPgMap.PgMap[strconv.Itoa(pgId)]

	if pg.PgInState(utils.PgRemapped) ||
	  pg.PgInState(utils.PgDown) ||
	  pg.PgInState(utils.PgCreating) ||
	  pg.PgInState(utils.PgUndersize) {
		return false
	}

	return true
}

func removeArrInt(arr []int, val int) []int {
	left, right := 0, len(arr)-1
	for left <= right {
		if arr[left] == val {
			arr[left], arr[right] = arr[right], arr[left]
			right--
		} else {
			left++
		}
	}
	return arr[:left]
}

func changePgDomain(pgDomainMap *map[int][]string, pgId int, srcDomain string, dstDomain string) {
	num := len((*pgDomainMap)[pgId])
	for i := 0; i < num; i++ {
		if srcDomain == (*pgDomainMap)[pgId][i] {
			(*pgDomainMap)[pgId][i] = dstDomain
			break
		}
	}
}

func getMinPgOsd(cfg *OptimizeCfg, domain string, pgNumPerOsd *map[int]int32) int {
	var minOsdPNum int32
	minOsdPNum = math.MaxInt32
	var minOsd int 
	for _, domainNode := range *(cfg.OSDTree) {
		if domainNode.DomainName != domain {
			continue
		}
		for _, osd := range domainNode.ChildNode {
			if (*pgNumPerOsd)[int(osd.OSDID)] < minOsdPNum {
				minOsdPNum = (*pgNumPerOsd)[int(osd.OSDID)]
				minOsd = int(osd.OSDID)
			}
		}
	}
	return minOsd
}

func addPgOsd(ctx context.Context, cfg *OptimizeCfg, poolid PoolID, pgId int, pgDomainMap *map[int][]string,
  minPgDomain string, secondMinPgDomain string, thirdMinPgDomain string, domainPgNum *map[string]int32, 
  pgOnDomainMap *map[string][]int, pgNumPerOsd *map[int]int32) bool {

	pg := AllPools[poolid].PoolPgMap.PgMap[strconv.Itoa(pgId)]
	var addOsdNum = cfg.PGSize - len(pg.OsdList)
	newOsdList := pg.OsdList
	var addOsd int
	isRemap := false
	pgSize := AllPools[poolid].PGSize

	for i := 0; i < addOsdNum; i++ {
		if !containsDomain(pgDomainMap, pgId, minPgDomain) {
			log.Debug(ctx, "pg:", pgId, " minPgDomain ", minPgDomain)
			(*domainPgNum)[minPgDomain]++
			(*pgOnDomainMap)[minPgDomain] = append((*pgOnDomainMap)[minPgDomain], pgId)
			(*pgDomainMap)[pgId] = append((*pgDomainMap)[pgId], minPgDomain)
			addOsd = getMinPgOsd(cfg, minPgDomain, pgNumPerOsd)
		} else if !containsDomain(pgDomainMap, pgId, secondMinPgDomain) {
			log.Debug(ctx, "pg:", pgId, " secondMinPgDomain ", secondMinPgDomain)
			(*domainPgNum)[secondMinPgDomain]++
			(*pgOnDomainMap)[secondMinPgDomain] = append((*pgOnDomainMap)[secondMinPgDomain], pgId)
			(*pgDomainMap)[pgId] = append((*pgDomainMap)[pgId], secondMinPgDomain)
			addOsd = getMinPgOsd(cfg, secondMinPgDomain, pgNumPerOsd)
		} else if !containsDomain(pgDomainMap, pgId, thirdMinPgDomain) {
			log.Debug(ctx, "pg:", pgId, " thirdMinPgDomain ", thirdMinPgDomain)
			(*domainPgNum)[thirdMinPgDomain]++
			(*pgOnDomainMap)[thirdMinPgDomain] = append((*pgOnDomainMap)[thirdMinPgDomain], pgId)
			(*pgDomainMap)[pgId] = append((*pgDomainMap)[pgId], thirdMinPgDomain)
			addOsd = getMinPgOsd(cfg, thirdMinPgDomain, pgNumPerOsd)
		} else {
			continue
		}

		isRemap = true
		(*pgNumPerOsd)[addOsd]++
		newOsdList = append(newOsdList, addOsd)
	}
	if isRemap {
		pg.Version++
		pg.SetPgState(utils.PgRemapped)
		if len(newOsdList) == pgSize {
			pg.PgState = pg.PgState &^ utils.PgUndersize
		}
		pg.NewOsdList = newOsdList
		AllPools[poolid].PoolPgMap.PgMap[strconv.Itoa(pgId)] = pg
		log.Info(ctx, "pg: ", pgId, " PgState ", pg.PgState, " osdList: ", pg.OsdList, " NewOsdList: ", pg.NewOsdList)	
	}
	return 	isRemap
}

func transferPG(ctx context.Context, cfg *OptimizeCfg, poolid PoolID, pgId int, pgDomainMap *map[int][]string, 
  osdDomainMap *map[int]string, maxPgDomain string, minPgDomain string, secondMinPgDomain string, 
  thirdMinPgDomain string, domainPgNum *map[string]int32, pgOnDomainMap *map[string][]int, 
  pgNumPerOsd *map[int]int32) bool {
	

	pg := AllPools[poolid].PoolPgMap.PgMap[strconv.Itoa(pgId)]
	var newOsdList []int
	var srcOsd int
	var dstOsd int
	for _, osd := range pg.OsdList {
		domain, ok := (*osdDomainMap)[osd]
		if ok && domain == maxPgDomain {
			srcOsd = osd
		} else {
			newOsdList = append(newOsdList, osd)
		}
	} 

	if !containsDomain(pgDomainMap, pgId, minPgDomain) {
		log.Debug(ctx, "pg:", pgId, " maxPgDomain ", maxPgDomain, " minPgDomain ", minPgDomain)
		(*domainPgNum)[maxPgDomain]--
		(*domainPgNum)[minPgDomain]++
		(*pgOnDomainMap)[maxPgDomain] = removeArrInt((*pgOnDomainMap)[maxPgDomain], pgId)
		(*pgOnDomainMap)[minPgDomain] = append((*pgOnDomainMap)[minPgDomain], pgId)
		changePgDomain(pgDomainMap, pgId, maxPgDomain, minPgDomain)
		dstOsd = getMinPgOsd(cfg, minPgDomain, pgNumPerOsd)
	} else if !containsDomain(pgDomainMap, pgId, secondMinPgDomain) {
		log.Debug(ctx, "pg:", pgId, " maxPgDomain ", maxPgDomain, " secondMinPgDomain ", secondMinPgDomain)
		(*domainPgNum)[maxPgDomain]--
		(*domainPgNum)[secondMinPgDomain]++		
		(*pgOnDomainMap)[maxPgDomain] = removeArrInt((*pgOnDomainMap)[maxPgDomain], pgId)
		(*pgOnDomainMap)[secondMinPgDomain] = append((*pgOnDomainMap)[secondMinPgDomain], pgId)
		changePgDomain(pgDomainMap, pgId, maxPgDomain, secondMinPgDomain)
		dstOsd = getMinPgOsd(cfg, secondMinPgDomain, pgNumPerOsd)
	} else if !containsDomain(pgDomainMap, pgId, thirdMinPgDomain) {
		log.Debug(ctx, "pg:", pgId, " maxPgDomain ", maxPgDomain, " thirdMinPgDomain ", thirdMinPgDomain)
		(*domainPgNum)[maxPgDomain]--
		(*domainPgNum)[thirdMinPgDomain]++
		(*pgOnDomainMap)[maxPgDomain] = removeArrInt((*pgOnDomainMap)[maxPgDomain], pgId)
		(*pgOnDomainMap)[thirdMinPgDomain] = append((*pgOnDomainMap)[thirdMinPgDomain], pgId)
		changePgDomain(pgDomainMap, pgId, maxPgDomain, thirdMinPgDomain)
		dstOsd = getMinPgOsd(cfg, thirdMinPgDomain, pgNumPerOsd)
	} else {
		return false
	}
	log.Debug(ctx, "+++ pool: ", poolid, " pg: ", pgId, ",  srcOsd: ", srcOsd, ", dstOsd: ", dstOsd)
	(*pgNumPerOsd)[srcOsd]--
	(*pgNumPerOsd)[dstOsd]++
	newOsdList = append(newOsdList, dstOsd)
	pg.Version++
	pg.SetPgState(utils.PgRemapped)
	pg.NewOsdList = newOsdList
	AllPools[poolid].PoolPgMap.PgMap[strconv.Itoa(pgId)] = pg
	log.Debug(ctx, "pg: ", pgId, " PgState ", pg.PgState, " osdList: ", pg.OsdList, " NewOsdList: ", pg.NewOsdList)

	return true
}

func reblancePool(ctx context.Context,
  osdTreeMap *map[Level]*map[string]*BucketTreeNode,
  osdNodeMap *map[string]OSDTreeNode,
  pool *PoolConfig) bool {
	log.Info(ctx, "reblance pool ", pool.Poolid, ":", pool.Name)
    optimizeCfg := &OptimizeCfg{
        PGCount:        pool.PGCount,
        PGSize:         getEffectivePGSize(pool.PGSize, GetFailureDomainNum(osdTreeMap, pool.FailureDomain)),
        FailureDomain:  pool.FailureDomain,
        PreviousPGList: pool.PoolPgMap.PgMap,
    }

    optimizeCfg.OSDTree, optimizeCfg.OSDInfoMap, optimizeCfg.TotalWeight = FlattenTree(ctx, osdTreeMap, osdNodeMap, 
	  pool.PGCount, pool.PGSize, pool.FailureDomain, pool.Root, pool.Root != "")

	/* 
	 * domain既failure domain表示pool的故障域。
	 * domainPgNum记录每个domain上的pg数量
	 * pgOnDomainMap记录每个domain上有哪些pg
	 * pgNumPerOsd记录每个osd上的pg数量
	 * pgDomainMap记录每个pg上面有哪些domain
	 * osdDomainMap记录osd属于哪个domain
	 */
	domainPgNum, pgOnDomainMap, pgNumPerOsd, pgDomainMap, osdDomainMap := getDomainPg(optimizeCfg, PoolID(pool.Poolid))
	for domainName, pgList := range *pgOnDomainMap {
		log.Debug(ctx, "+++ pool: ", pool.Poolid, " domain: ", domainName, " pg num: ", (*domainPgNum)[domainName], ", pg list: ", pgList)
	}
	for osdId, pgnum := range *pgNumPerOsd {
		log.Debug(ctx, "+++ pool: ", pool.Poolid, " osdId: ", osdId, " pg num: ", pgnum)
	}
	for pgid, domainList := range *pgDomainMap {
		log.Debug(ctx, "+++ pool: ", pool.Poolid, " pgid: ", pgid, " domain list: ", domainList)
	}


    if len(*domainPgNum) == 0{
        return false
    }

	domainCount := len(*domainPgNum)
	pgPerDomain := int32(optimizeCfg.PGCount * optimizeCfg.PGSize / domainCount) // 平均值，计算每个domain上承担的pg数量，向下取整

	isRemap := false
	breakFlag := true

    /*
	 * 检查pg种osd数量是否少于pool的pgsize,如果是就需要向pg里增加osd
	 */
	for pgIdStr, pgConfig := range pool.PoolPgMap.PgMap {
		if pgConfig.PgInState(utils.PgRemapped) || pgConfig.PgInState(utils.PgDown) {
            continue
		}
		if len(pgConfig.OsdList) == optimizeCfg.PGSize {
			continue
		}

        minPgDomain, secondMinPgDomain, thirdMinPgDomain := top3MinValue(domainPgNum)
		pgId, _ := strconv.Atoi(pgIdStr)

		if addPgOsd(ctx, optimizeCfg, PoolID(pool.Poolid), pgId, pgDomainMap,
		  minPgDomain, secondMinPgDomain, thirdMinPgDomain, 
		  domainPgNum, pgOnDomainMap, pgNumPerOsd) {
			isRemap = true
		}  
	}

	/*   
	 *  domainPgNum是一个map， key是pool的domain名字，value为此domain上面的pg数量
	 *  均衡pg，从pg数量最多的那个domain上迁移pg到pg数量最少的domain上
	 *  
	 */
	for loop := 0; loop < pool.PGCount; loop++ {
		maxPgDomain := getMaxValue(domainPgNum)
		minPgDomain, secondMinPgDomain, thirdMinPgDomain := top3MinValue(domainPgNum)
		log.Debug(ctx, "+++ pool: ", pool.Poolid, ", maxPgDomain: ", maxPgDomain, ", minPgDomain: ", minPgDomain, 
				", secondMinPgDomain: ", secondMinPgDomain, ", thirdMinPgDomain: ", thirdMinPgDomain)

		breakFlag = true
		//domain的pg数，只能处于平均值 或 平均值+1
		if (*domainPgNum)[minPgDomain] < pgPerDomain ||
		  (*domainPgNum)[maxPgDomain] > pgPerDomain + 1 {

			//遍历承担pg最多的那个domain上的pg
			for i := 0; i < len((*pgOnDomainMap)[maxPgDomain]); i++ {
				pgId := (*pgOnDomainMap)[maxPgDomain][i]
				log.Debug(ctx, "+++ pool: ", pool.Poolid, ", maxPgDomain: ", maxPgDomain, ", pgId: ", pgId)
				//检测maxPgDomain是否在pg中
				if containsDomain(pgDomainMap, pgId, maxPgDomain) {
					if !transferable(PoolID(pool.Poolid), pgId) {
						log.Debug(ctx, "+++ pool: ", pool.Poolid, ", pg: ",  pgId, " can not transfre.")
						continue
					}

					if transferPG(ctx, optimizeCfg, PoolID(pool.Poolid), pgId, pgDomainMap, osdDomainMap, 
					  maxPgDomain, minPgDomain, secondMinPgDomain, thirdMinPgDomain, 
					  domainPgNum, pgOnDomainMap, pgNumPerOsd) {
						isRemap = true
						breakFlag = false
						break
					}
				}
			}
		}
		if breakFlag {
			break
		}
	}
	if isRemap {
		AllPools[PoolID(pool.Poolid)].PoolPgMap.Version++
	}
	return isRemap
}

func Reblance(ctx context.Context, client *etcdapi.EtcdClient) {
    if AllPools == nil {
        log.Info(ctx, "AllPoolsConfig nil!")
        return
    }

    osdTreeMap, osdNodeMap, terr := GetOSDTree(ctx, true, true)
    if terr != nil {
        log.Error(ctx, "GetOSDTree failed!")
        return
    }		

	if len(*osdNodeMap) == 0 {
		return
	}


    for poolID, pool := range AllPools {
        if pool.PoolPgMap.PgMap == nil {
            continue
        }	
		
		isRemap := reblancePool(ctx, osdTreeMap, osdNodeMap, pool)
		if isRemap {
			pc_buf, err := json.Marshal(AllPools[poolID])
            if err != nil {
                log.Error(ctx, err)
                return
            }
            key := fmt.Sprintf("%s%d", config.ConfigPoolsKeyPrefix, poolID)
            	
            err = client.Put(ctx, key, string(pc_buf))
            if err != nil {
                log.Error(ctx, err)
                return
            }
		}
	}
}


func CheckPgs(ctx context.Context, client *etcdapi.EtcdClient, osdid int, stateSwitch  STATESWITCH) {
    if AllPools == nil {
        log.Info(ctx, "AllPoolsConfig nil!")
        return
    }

    osdTreeMap, osdNodeMap, terr := GetOSDTree(ctx, true, true)
    if terr != nil {
        log.Error(ctx, "GetOSDTree failed!")
        return
    }	

    for poolID, pool := range AllPools {
        if pool.PoolPgMap.PgMap == nil {
            continue
        }

        log.Info(ctx, "check pool", poolID, ":", pool.Name)

        pgSize := pool.PGSize
        oldPGs := pool.PoolPgMap.PgMap
        isRemap := false
		isPgStateChange := false
        
        for pgID, pg := range oldPGs {
            if pg.PgInState(utils.PgRemapped) {
                if !listContain(pg.OsdList, osdid) && !listContain(pg.NewOsdList, osdid) {
                	//状态变更的osd不在pg的osd列表中
                	continue
                }
                log.Info(ctx, "pg ", poolID, ".", pgID, " is in PgRemapped state.")
                if !pg.PgInState(utils.PgDown) {
                    if stateSwitch == InToOut {
                        PushPgTask(pgID, osdid, stateSwitch)
                    }
                } 
                // else {  //pg处于PgDown状态
                	// pg选出leader后会检查是否继续变更，这里不需要检查是否变更
                    // if stateSwitch == DownToUp  && shouldChange(pg.OsdList, pgSize, osdid) {
                        // PushPgTask(pgID, osdid, stateSwitch)
                    // }
                // }
            } else {
                if !listContain(pg.OsdList, osdid) {
                	//状态变更的osd不在pg的osd列表中
                	continue
                }
	
                if pg.PgInState(utils.PgCreating) {
                    //pg is creating
                    if pg.PgInState(utils.PgDown) {
                    	if stateSwitch == InToOut {
                    	    //osd from in to out
                    	    log.Info(ctx, "pg ", poolID, ".", pgID, "in PgCreating | PgDown, osd from in to out")
                    	    pgConfig, ok := redistributionPg(ctx, osdTreeMap, osdNodeMap, poolID, pgID)
                    	    if ok {
                    	        //pg处于redistributionPg && PgDown，此时有osd状态有in变为out，pg状态不会变为PgRemapped
                    	        pgConfig.UnsetPgState(utils.PgRemapped)
                    	        pgConfig.SetPgState(utils.PgCreating)
                    	        isRemap = true
                    	        AllPools[poolID].PoolPgMap.PgMap[pgID] = *pgConfig
                    	    }
                    	}
                    } else {
                        if stateSwitch == InToOut {
                            //osd from in to out
                            log.Info(ctx, "pg ", poolID, ".", pgID, "in PgCreating, osd from in to out")
                            PushPgTask(pgID, osdid, stateSwitch)
                        }
                    }
                }else if !pg.PgInState(utils.PgDown) {
                    //pg is not down
                    if stateSwitch == InToOut ||  stateSwitch == DownToUp{
                        //osd from in to out
                        if stateSwitch == InToOut {
                            log.Info(ctx, "pg ", poolID, ".", pgID, "not in PgDown, osd from in to out")
                        } else if stateSwitch == DownToUp {
                            log.Info(ctx, "pg ", poolID, ".", pgID, "not in PgDown, osd from down to up")
                        }
                        
                        pgConfig, ok := redistributionPg(ctx, osdTreeMap, osdNodeMap, poolID, pgID)
                        if ok {
                            isRemap = true
                            AllPools[poolID].PoolPgMap.PgMap[pgID] = *pgConfig
                        }
                    }
                }else if pg.PgInState(utils.PgDown) {
                    //pg is down
                    if stateSwitch == DownToUp && shouldChange(pg.OsdList, pgSize, osdid) {
                        log.Info(ctx, "pg ", poolID, ".", pgID, "in PgDown, osd from down to up")
                        pgConfig, ok := redistributionPg(ctx, osdTreeMap, osdNodeMap, poolID, pgID)
                        if ok {
                            isRemap = true
                            AllPools[poolID].PoolPgMap.PgMap[pgID] = *pgConfig
                        }
                    }
                }
            }
            if stateSwitch == DownToUp {
                pgConfig := AllPools[poolID].PoolPgMap.PgMap[pgID]
                //检查pg状态是否需要变更
                state := CheckPgState(pgConfig.OsdList, pool.PGSize)
                if state == 0 {
                    isPgStateChange = true
                    pgConfig.UnsetPgState(utils.PgUndersize)
                    pgConfig.UnsetPgState(utils.PgDown)
                    if !pgConfig.PgInState(utils.PgCreating) && !pgConfig.PgInState(utils.PgRemapped) {
                        pgConfig.SetPgState(utils.PgActive)
                    }
                    AllPools[poolID].PoolPgMap.PgMap[pgID] = pgConfig
                } else if !pgConfig.PgInState(state) {
                    isPgStateChange = true
                    pgConfig.SetPgState(state)
                    AllPools[poolID].PoolPgMap.PgMap[pgID] = pgConfig
                }
            }
        }
        if isRemap || isPgStateChange {
            AllPools[poolID].PoolPgMap.Version++
            pc_buf, err := json.Marshal(AllPools[poolID])
            if err != nil {
                log.Error(ctx, err)
                return
            }
            key := fmt.Sprintf("%s%d", config.ConfigPoolsKeyPrefix, poolID)
            	
            err = client.Put(ctx, key, string(pc_buf))
            if err != nil {
                log.Error(ctx, err)
                return
            }
        }
	}
}

func redistributionPg(ctx context.Context, 
  osdTreeMap *map[Level]*map[string]*BucketTreeNode, 
  osdNodeMap *map[string]OSDTreeNode,
  poolId PoolID, pgId string) (*PGConfig, bool){
    oldOsdList := AllPools[poolId].PoolPgMap.PgMap[pgId].OsdList
    var newOsdList []int
    for _, id := range oldOsdList {
        osdInfo, ok := AllOSDInfo.Osdinfo[OSDID(id)]
        if ok == true {
            if osdInfo.IsIn {
                newOsdList = append(newOsdList, id)
            }
        }
    }

    pool := AllPools[poolId]
    optimizeCfg := &OptimizeCfg{
        PGCount:        pool.PGCount,
        PGSize:         getEffectivePGSize(pool.PGSize, GetFailureDomainNum(osdTreeMap, pool.FailureDomain)),
        FailureDomain:  pool.FailureDomain,
        PreviousPGList: AllPools[poolId].PoolPgMap.PgMap,
    }
    addNum := optimizeCfg.PGSize - len(newOsdList)
    log.Info(ctx, "pg: ", poolId, ".", pgId, " PGSize: ", optimizeCfg.PGSize, " osdList: ", oldOsdList, " newOsdList: ", newOsdList,
            " addNum: ", addNum)
    if addNum <= 0 {
        return nil, false
    }

    optimizeCfg.OSDTree, optimizeCfg.OSDInfoMap, optimizeCfg.TotalWeight = FlattenTree(ctx, osdTreeMap, osdNodeMap, 
            pool.PGCount, pool.PGSize, pool.FailureDomain, pool.Root, pool.Root != "")
    addOsdList := calculatePgOsds(ctx, optimizeCfg, poolId, pgId, addNum, oldOsdList)
    log.Info(ctx, "pg: ", poolId, ".", pgId, " addOsdList: ", addOsdList)
    if len(addOsdList) == 0{
        return nil, false
    }
    newOsdList = append(newOsdList, addOsdList...)
    pgConfig := AllPools[poolId].PoolPgMap.PgMap[pgId]
    pgConfig.Version++
	pgConfig.SetPgState(utils.PgRemapped)
    pgConfig.NewOsdList = newOsdList    
    log.Info(ctx, "pg: ", poolId, ".", pgId, " Version: ", pgConfig.Version, " PgState: ", pgConfig.PgState, 
	        " OsdList: ", pgConfig.OsdList, " NewOsdList: ", pgConfig.NewOsdList)
    return &pgConfig, true
}

func getPgNumPerOsd() map[int]int32 {
    pgNumPerOsd := make(map[int]int32)
    for _, pool := range AllPools {
        for _, pg := range pool.PoolPgMap.PgMap {
            for _, osdId := range pg.OsdList {
                pgNumPerOsd[osdId]++
            }
        }
    }
    return pgNumPerOsd
}

func listContain(list []int, value int) bool{
    for _, val := range list {
        if val == value {
            return true
        }
    }
    return false
}

func getMinValue(mp *map[string]int32) string {
    var minVal int32
    var minKey string
    for key, value := range *mp {
        minVal = value
        minKey = key
        break
    }    
    for key, value := range *mp {
        if value < minVal {
            minKey = key
        }
    }
    return minKey
}

func top3MinValue(mp *map[string]int32) (string, string, string) {
	var minVal int32 = math.MaxInt32
	var secondMinVal int32 = math.MaxInt32
	var thirdMinVal int32 = math.MaxInt32
	var minKey string
	var secondMinKey string
	var thirdMinKey string

	for key, value := range *mp {
		if value < minVal {
			thirdMinVal, thirdMinKey = secondMinVal, secondMinKey
			secondMinVal, secondMinKey = minVal, minKey
			minVal, minKey = value, key
		} else if value >= minVal && value < secondMinVal {
			thirdMinVal, thirdMinKey = secondMinVal, secondMinKey
			secondMinVal, secondMinKey = value, key
		} else if value >= secondMinVal && value < thirdMinVal {
			thirdMinVal, thirdMinKey = value, key
		}
	}
	return minKey, secondMinKey, thirdMinKey
}

func getMaxValue(mp *map[string]int32) string {
    var maxVal int32 = 0
    var maxKey string
    for key, value := range *mp {
        if value > maxVal {
            maxKey = key
            maxVal = value
        }
    }
    return maxKey
}

func calculatePgOsds(ctx context.Context, cfg *OptimizeCfg, poolId PoolID, pgId string, addNum int, oldOsdList []int) ([]int) {
    pgNumPerOsd := getPgNumPerOsd()
    domainPgNum := make(map[string]int32)
    var addOsdList []int
    var minPgDomain string

    log.Warn(ctx, "pg: ", poolId, ".", pgId, " pgNumPerOsd: ", pgNumPerOsd)
    for _, domain := range *cfg.OSDTree {
        var pgNum int32
        noContain := false

        for _, osd := range domain.ChildNode {
            if listContain(oldOsdList, int(osd.OSDID)) {
                if len(domain.ChildNode) == 1 {
                    noContain = true
                    break
                } else {
                    continue
                }
            }
            pgNum += pgNumPerOsd[int(osd.OSDID)]
        }
        if !noContain {
        	domainPgNum[domain.DomainName] = pgNum
        }
	}

    log.Info(ctx, "pg: ", poolId, ".", pgId, " domainPgNum: ", domainPgNum)
    if len(domainPgNum) == 0{
        return addOsdList
    }

    for i := 0; i < addNum; i++ {
getMin:
        minPgDomain = getMinValue(&domainPgNum)
        log.Info(ctx, "pg: ", poolId, ".", pgId, " minPgDomain: ", minPgDomain)
        if cfg.FailureDomain == "osd" {
            id, _ := strconv.Atoi(minPgDomain)
            log.Info(ctx, "pg: ", poolId, ".", pgId, " id: ", id)
            addOsdList = append(addOsdList, id)
            pgNumPerOsd[id]++
        } else {
            var pgNumPerDomainOsd map[string]int32
            for _, domain := range *cfg.OSDTree {
                if domain.DomainName == minPgDomain {
                    for _, osd := range domain.ChildNode {
                        if listContain(oldOsdList, int(osd.OSDID)) ||  listContain(addOsdList, int(osd.OSDID)) {
                            continue
                        }
                        pgNumPerDomainOsd[osd.OSDID.String()] = pgNumPerOsd[int(osd.OSDID)]
                    }
                    log.Info(ctx, "pg: ", poolId, ".", pgId, " pgNumPerDomainOsd: ", pgNumPerDomainOsd)
                    if len(pgNumPerDomainOsd) == 0 {
                        delete(domainPgNum, minPgDomain)
                        goto getMin
                    }
                    minPgOsd := getMinValue(&pgNumPerDomainOsd)
                    id, _ := strconv.Atoi(minPgOsd)
                    log.Info(ctx, "pg: ", poolId, ".", pgId, " minPgOsd: ", minPgOsd, " id: ", id)
                    addOsdList = append(addOsdList, id)
                    pgNumPerOsd[id]++
                }
            }
		}
        log.Info(ctx, "pg: ", poolId, ".", pgId, " pgNumPerOsd: ", pgNumPerOsd)
        domainPgNum[minPgDomain]++
        log.Info(ctx, "pg: ", poolId, ".", pgId, " domainPgNum: ", domainPgNum)
    }
    
    return addOsdList
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
    if domainNum * 2 < pgSize {
        return 0
    }
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

			if Compare_arry(osdlist, oldPgCfg.OsdList) == false {
				return false
			}
		}
	}

	return true
}

func Compare_arry(arr1 []int, arr2 []int) bool {
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

func CheckPgState(osdList []int, pgSize int) utils.PGSTATE {
    var upNum int
    for _, id := range osdList {
        osdInfo, ok := AllOSDInfo.Osdinfo[OSDID(id)]
        if ok == true {
            if osdInfo.IsUp {
                upNum++
            }
        }
    }
	if upNum > pgSize / 2  &&  upNum < pgSize {
        return utils.PgUndersize
    } else if upNum <= pgSize / 2 {
        return utils.PgDown
    }
    return 0
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
func SimpleInitial(ctx context.Context, cfg *OptimizeCfg, poolPgSize int) (*OptimizeResult, error) {
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
			ppc.SetPgState(utils.PgCreating)
			state := CheckPgState(ppc.OsdList, poolPgSize)
			if state == utils.PgUndersize ||  state == utils.PgDown {
				ppc.SetPgState(state)
			}
		} else {
			old_osdlist := oldPgCfg.OsdList
			equal := Compare_arry(old_osdlist, ppc.OsdList)
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
