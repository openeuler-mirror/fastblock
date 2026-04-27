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
	"fmt"
	"strconv"
	"strings"
	"math"
	"sync"

	"monitor/config"
	"monitor/etcdapi"
	"monitor/log"
	"monitor/msg"
	"monitor/utils"
)

const (
	// MinPoolNum defines min pool number, from 1.
	MinPoolNum = 1
	// MinPGID defines min PG ID, 0 or 1.
	MinPGID = uint64(0)
)

// PGID is the PG ID in each pool. Excluding pool number.
// TODO: now use string in etcd.
type PGID uint64

// PGsConfig 7/pools/$PGID, output.
// put pgmap in on key/value pair so we cant version it golablly
// Example: {"version":123,"pgmap":{"1_1":[1,2,3],"1_2":[2,3,4]}}
//
//	means pool 1, pg 1 osd [1,2,3], pg 2 osd [2,3,4]
//
// TODO: for each pool or any incremental update?
type PoolPGsConfig struct {
	Version int64               `json:"version,omitempty"`
	PgMap   map[string]PGConfig `json:"pgmap,omitempty"`
}

// PGConfig for each pg in /config/pgmap, output.
// Example: {"1":[1,2,3]}
// TODO: use OSDID, not string. It requires parse change.
// type PGConfig []int
type PGConfig struct {
	Version    int64   `json:"version,omitempty"`
	PgState    utils.PGSTATE `json:"pgstate,omitempty"`
	CoreIndex  uint32  `json:"coreindex,omitempty"` 
	OsdList    []int   `json:"osdlist,omitempty"`
	NewCoreIndex  uint32  `json:"newcoreindex,omitempty"` 
	NewOsdList []int   `json:"newosdlist,omitempty"`
}

// PoolID defines pool ID.
type PoolID int32

// FailureDomain defined for crush.
type FailureDomain string

func (domain FailureDomain) String() string {
	return string(domain)
}

var validFailureDomainMap = map[string]bool{"osd": true, "host": true, "rack": true, "root": true}

func (domain FailureDomain) IsValid() bool {
	if valid := validFailureDomainMap[domain.String()]; valid {
		return true
	}

	return false
}

// PoolConfig for each pool in /config/pools.
// use string for id too
type PoolConfig struct {
	RwMutex       sync.RWMutex
	Poolid        int           `json:"poolid,omitempty"`
	Name          string        `json:"name,omitempty"`
	PGSize        int           `json:"pg_size,omitempty"`
	PGCount       int           `json:"pg_count,omitempty"`
	FailureDomain string        `json:"failure_domain,omitempty"`
	Root          string        `json:"root,omitempty"`
	PoolPgMap     PoolPGsConfig `json:"poolpgmap,omitempty"`
}

type PoolsInfo struct {
	RwMutex       sync.RWMutex
	pools         map[PoolID]*PoolConfig
}

// 已经包含了pg的分配表
var AllPools PoolsInfo
var lastSeenPoolId int32
var osdmapVersion int64

type pgTask struct {
	osdid       int
	stateSwitch STATESWITCH
}

var pgsTaskQueue map[string]*CommonQueue

/*
 * 设置pg的状态, 参数state只能是PgCreating、PgActive、PgUndersize、PgDown和PgRemapped中的一种
 * pg状态可以共存的几种情况：
 *     PgCreating | PgUndersize
 *     PgCreating | PgDown
 *     PgUndersize | PgRemapped
 *     PgDown | PgRemapped
 */
func (pg *PGConfig)SetPgState(state utils.PGSTATE) {
	switch state {
	case utils.PgCreating:
		pg.PgState = pg.PgState &^ utils.PgActive
		pg.PgState = pg.PgState &^ utils.PgRemapped
		pg.PgState |= state
	case utils.PgActive:
		pg.PgState = state
	case utils.PgUndersize:
		pg.PgState = pg.PgState &^ utils.PgActive
		pg.PgState = pg.PgState &^ utils.PgDown
		pg.PgState |= state
	case utils.PgDown:
		pg.PgState = pg.PgState &^ utils.PgActive
		pg.PgState = pg.PgState &^ utils.PgUndersize
		pg.PgState |= state
	case utils.PgRemapped:
		pg.PgState = pg.PgState &^ utils.PgActive
		pg.PgState = pg.PgState &^ utils.PgCreating
		pg.PgState |= state
	}
}

/*
 * 复位pg的状态, 参数state只能是PgCreating、PgActive、PgUndersize、PgDown和PgRemapped中的一种
 */
func (pg *PGConfig)UnsetPgState(state utils.PGSTATE) {
	if state == utils.PgCreating ||
	  state == utils.PgActive    ||
	  state == utils.PgUndersize ||
	  state == utils.PgDown      ||
	  state == utils.PgRemapped {
		pg.PgState = pg.PgState &^ state
	}
}

/*
 * 检查pg是否处于参数state所示状态
 */
func (pg *PGConfig)PgInState(state utils.PGSTATE) bool{
	if state == utils.PgCreating ||
	  state == utils.PgActive    ||
	  state == utils.PgUndersize ||
	  state == utils.PgDown      ||
	  state == utils.PgRemapped {
		return (pg.PgState & state) != 0; 
	}
	return false
}

func GetPoolPgNum() (int32, int32, *msg.PgStateInfo) {
	poolNum := 0
	pgNUm := 0
	pgstat := &msg.PgStateInfo{}

	var pool_array []PoolID
	AllPools.RwMutex.RLock()
	for poolID := range AllPools.pools {
		pool_array = append(pool_array, poolID)
	}
	AllPools.RwMutex.RUnlock()

	poolNum = 0
	for _, poolID := range pool_array {
		AllPools.RwMutex.RLock()
		pool, ok := AllPools.pools[poolID]
		if !ok {
			AllPools.RwMutex.RUnlock()
			continue
		}
		pool.RwMutex.RLock()
		AllPools.RwMutex.RUnlock()

		poolNum += 1
		pgNUm += len(pool.PoolPgMap.PgMap)
		for _, pg := range pool.PoolPgMap.PgMap {
			if pg.PgInState(utils.PgCreating) && pg.PgInState(utils.PgUndersize) {
				pgstat.CreatingUndersizeNum += 1
			} else if pg.PgInState(utils.PgCreating) && pg.PgInState(utils.PgDown) {
				pgstat.CreatingDownNum += 1
			} else if pg.PgInState(utils.PgUndersize) && pg.PgInState(utils.PgRemapped) {
				pgstat.UndersizeRemapNum += 1
			} else if pg.PgInState(utils.PgDown) && pg.PgInState(utils.PgRemapped) {
				pgstat.DownRemapNum += 1
			} else if pg.PgInState(utils.PgCreating) {
				pgstat.CreatingNum += 1
			} else if pg.PgInState(utils.PgActive) {
				pgstat.ActiveNum += 1
			} else if pg.PgInState(utils.PgUndersize) {
				pgstat.UndersizeNum += 1
			} else if pg.PgInState(utils.PgDown) {
				pgstat.DownNum += 1
			} else if pg.PgInState(utils.PgRemapped) {
				pgstat.RemapNum += 1
			}
		}
		pool.RwMutex.RUnlock()
	}
	return int32(poolNum), int32(pgNUm), pgstat
}

// findUsablePoolId finds the first available pool id
// we don't reuse pool ids, so it always increasing
// poll deletions are rare, we are safe to use int32.
func findUsablePoolId() PoolID {
	return PoolID(lastSeenPoolId + 1)
}

// Example: /config/pools/pool/1 '{"poolid": 1, "name":"testpool","pg_size":3, "pg_count":256,"failure_domain":"host"}'
// Example: /config/pools/pool/2 '{"poolid": 2, "name":"testpool2","pg_size":3, "pg_count":256,"failure_domain":"host"}'
// poolid is somewhat redunant?
// call it on start
func LoadPoolConfig(ctx context.Context, client *etcdapi.EtcdClient) (err error) {
	lastSeenPoolId = 0
	AllPools.pools = make(map[PoolID]*PoolConfig)
	pgsTaskQueue = make(map[string]*CommonQueue)

	kvs, getErr := client.GetWithPrefix(ctx, config.ConfigPoolsKeyPrefix)
	if getErr != nil {
		log.Error(ctx, getErr)
		return getErr
	}

	if len(kvs) == 0 {
		log.Info(ctx, "no pool created yet")
		return nil
	}

	for _, kv := range kvs {
		log.Info(ctx, "pool key:", kv.Key, ", value:", string(kv.Value))

		k := string(kv.Key)
		poolID, err := strconv.Atoi(strings.TrimPrefix(k, config.ConfigPoolsKeyPrefix))
		if err != nil {
			log.Info(ctx, "failed to get poolId")
			continue
		}

		var poolConfig PoolConfig
		if jerr := json.Unmarshal([]byte(kv.Value), &poolConfig); jerr != nil {
			log.Error(ctx, jerr, string(kv.Key), string(kv.Value))
			return jerr
		}

		AllPools.RwMutex.Lock()
		AllPools.pools[PoolID(poolID)] = &poolConfig
		AllPools.RwMutex.Unlock()

		if lastSeenPoolId < int32(poolID) {
			lastSeenPoolId = int32(poolID)
		}

		for pgid, _ := range poolConfig.PoolPgMap.PgMap {
			pgsTaskQueue[pgid] = NewQueue()
		}

		log.Debug(ctx, "pool", poolID)
	}

	log.Info(ctx, "loadPoolConfig done")
	// for k, v := range AllPools {
		// log.Info(ctx, k, v)
	// }

	return nil
}

func ProcessOsdDown(ctx context.Context, client *etcdapi.EtcdClient, osdid int) {
	var pool_array []PoolID
	AllPools.RwMutex.RLock()	
	if AllPools.pools == nil {
		AllPools.RwMutex.RUnlock()
		log.Warn(ctx, "AllPoolsConfig nil!")
		return
	}
	for poolID := range AllPools.pools {
		pool_array = append(pool_array, poolID)
	}
	AllPools.RwMutex.RUnlock()

	var isChange bool
	for _, poolID := range pool_array {
		AllPools.RwMutex.RLock()
		pool, ok := AllPools.pools[poolID]
		if !ok {
			AllPools.RwMutex.RUnlock()
			continue
		}
		pool.RwMutex.Lock()
		AllPools.RwMutex.RUnlock()			

		isChange = false
		if pool.PoolPgMap.PgMap == nil {
			pool.RwMutex.Unlock()
			continue
		}

		log.Info(ctx, "check pool", poolID, ":", pool.Name)
		ppg := &pool.PoolPgMap

		for pgID, pg := range ppg.PgMap {
			if !contains(pg.OsdList, osdid, 0, len(pg.OsdList)) {
				continue
			}
			if !pg.PgInState(utils.PgUndersize) && !pg.PgInState(utils.PgDown) {
				pg.SetPgState(utils.PgUndersize)
				isChange = true
				log.Info(ctx, "pg ", poolID, ".", pgID, " to PgUndersize state.")
			} else if pg.PgInState(utils.PgUndersize) && CheckPgState(ctx, pg.OsdList, pool.PGSize) == utils.PgDown {
				pg.SetPgState(utils.PgDown)
				isChange = true
				log.Info(ctx, "pg ", poolID, ".", pgID, " to PgDown state.")
			} else {
				continue
			}
			pg.Version++
			ppg.PgMap[pgID] = pg
		}

		if isChange {
			ppg.Version++
			AllPools.pools[poolID].PoolPgMap = *ppg

			pc_buf, err := json.Marshal(AllPools.pools[poolID])
			pool.RwMutex.Unlock()

			if err != nil {
				log.Error(ctx, err)
				continue
			}

			key := fmt.Sprintf("%s%d", config.ConfigPoolsKeyPrefix, poolID)
			err = client.Put(ctx, key, string(pc_buf))
			if err != nil {
				log.Error(ctx, err)
				continue
			}
		} else {
			pool.RwMutex.Unlock()
		}
	}
	// printAllPool(ctx)
}

func ProcessCreatePoolMessage(ctx context.Context, client *etcdapi.EtcdClient, name string, size int, pc int, fd string, root string) (int32, error) {
	AllPools.RwMutex.Lock()
	for _, pc := range AllPools.pools {
		if name == pc.Name {
			AllPools.RwMutex.Unlock()
			return -1, EPoolAlreadyExists
		}
	}

	pid := findUsablePoolId()

	//pgs of the pool is created by RecheckPgs
	ppgc := PoolPGsConfig{Version: 1, PgMap: make(map[string]PGConfig)}
	poolConf := &PoolConfig{
		Poolid:        int(pid),
		Name:          name,
		PGSize:        size,
		PGCount:       pc,
		FailureDomain: fd,
		Root:          root,
		PoolPgMap:     ppgc,
	}

	AllPools.pools[PoolID(pid)] = poolConf
	poolConf.RwMutex.Lock()
	AllPools.RwMutex.Unlock()

	PgMap, err := CreatePgs(ctx, client, poolConf, OSDCoreNum)
	if err != nil {
		delete(AllPools.pools, PoolID(pid))
		poolConf.RwMutex.Unlock()
		log.Error(ctx, err)
		return -1, err
	}
	poolConf.PoolPgMap.PgMap = *PgMap
	for pgid, _ := range poolConf.PoolPgMap.PgMap {
		pgsTaskQueue[pgid] = NewQueue()
	}

	pc_buf, err := json.Marshal(poolConf)
	if err != nil {
		delete(AllPools.pools, PoolID(pid))
		poolConf.RwMutex.Unlock()
		log.Error(ctx, err)
		return -1, EInternalError
	}

	key := fmt.Sprintf("%s%d", config.ConfigPoolsKeyPrefix, pid)

	err = client.Put(ctx, key, string(pc_buf))
	if err != nil {
		delete(AllPools.pools, PoolID(pid))
		poolConf.RwMutex.Unlock()
		log.Error(ctx, err)
		return -1, EInternalError
	}
	poolConf.RwMutex.Unlock()

	if lastSeenPoolId < int32(pid) {
		lastSeenPoolId = int32(pid)
	}

	log.Info(ctx, "successfully put to ectd for newly created pool", pid)
	return int32(pid), nil
}

func getPgOsdOutNum(ctx context.Context, osdList []int, pgSize int) int {
	var outNum int = 0

	AllOSDInfo.RwMutex.RLock()
	for _, id := range osdList {
		osdInfo, ok := AllOSDInfo.Osdinfo[OSDID(id)]
		if ok == true {
			osdInfo.RwMutex.RLock()		
			if !osdInfo.IsIn {
				outNum++
			}
			osdInfo.RwMutex.RUnlock()
		}
	}
	AllOSDInfo.RwMutex.RUnlock()
	return outNum
}

func ProcessLeaderBeElected(ctx context.Context, client *etcdapi.EtcdClient, leaderId int, poolId uint64, pgId uint64,
	osdList []int, newOsdList []int) error {

	pgIdStr := strconv.FormatUint(pgId, 10)
	AllPools.RwMutex.RLock()
	poolConf, ok := AllPools.pools[PoolID(poolId)]
	if ok {
		poolConf.RwMutex.Lock()
		AllPools.RwMutex.RUnlock()

		if pgConf, gok := poolConf.PoolPgMap.PgMap[pgIdStr]; gok {
			if pgConf.PgInState(utils.PgCreating) {
				if pgConf.PgInState(utils.PgUndersize) || pgConf.PgInState(utils.PgDown) {
					pgConf.UnsetPgState(utils.PgCreating)
				} else {
					pgConf.SetPgState(utils.PgActive)
				}
				pgConf.Version++
				log.Info(ctx, "pg", poolId, ".", pgId, " in PgCreating.")
			} else if pgConf.PgInState(utils.PgRemapped) {
				//pg remap过程中，leader down掉或切换，导致remap中断
				if Compare_arry(pgConf.OsdList, osdList) {
					outNum := getPgOsdOutNum(ctx, osdList, poolConf.PGSize)
					log.Info(ctx, "pg", poolId, ".", pgId, " in PgRemapped. outNum ", outNum)
					if outNum > 0 {
						pgConf.Version++
					} else {
						//pg中所有osd都是in状态，停止remap
						pgConf.NewOsdList = pgConf.NewOsdList[:0]
						pgConf.UnsetPgState(utils.PgRemapped)
						if !pgConf.PgInState(utils.PgUndersize) && !pgConf.PgInState(utils.PgDown) {
							pgConf.SetPgState(utils.PgActive)
						}
						pgConf.NewCoreIndex = math.MaxUint32

						pgConf.Version++
					}
				} else if Compare_arry(pgConf.NewOsdList, osdList) {
					pgConf.OsdList = pgConf.NewOsdList
					pgConf.NewOsdList = pgConf.NewOsdList[:0]
					pgConf.CoreIndex = pgConf.NewCoreIndex
					pgConf.NewCoreIndex = math.MaxUint32
					pgConf.UnsetPgState(utils.PgRemapped)
					state := CheckPgState(ctx, pgConf.OsdList, poolConf.PGSize)
					if state == 0 {
						pgConf.SetPgState(utils.PgActive)
					} else if state == utils.PgUndersize {
						pgConf.SetPgState(utils.PgUndersize)
					} else if state == utils.PgDown {
						pgConf.SetPgState(utils.PgDown)
					}

					pgConf.Version++
					log.Info(ctx, "pg", poolId, ".", pgId, " in PgRemapped. pg osdList == newOsdList")
				} else {
					poolConf.RwMutex.Unlock()
					return nil
				}
			} else {
				poolConf.RwMutex.Unlock()
				return nil
			}
			poolConf.PoolPgMap.Version++
			poolConf.PoolPgMap.PgMap[pgIdStr] = pgConf
			AllPools.pools[PoolID(poolId)] = poolConf
			poolConf.RwMutex.Unlock()

			ProcessPgTask(ctx, client, strconv.FormatUint(pgId, 10))
		} else {
			poolConf.RwMutex.Unlock()
			log.Info(ctx, "not find pg ", poolId, ".", pgId)
			return fmt.Errorf("not find pool %d.%d", poolId, pgId)
		}

		pc_buf, err := json.Marshal(poolConf)
		if err != nil {
			log.Error(ctx, err)
			return nil
		}

		key := fmt.Sprintf("%s%d", config.ConfigPoolsKeyPrefix, poolId)

		err = client.Put(ctx, key, string(pc_buf))
		if err != nil {
			log.Error(ctx, err)
			return nil
		}
	} else {
		AllPools.RwMutex.RUnlock()
		log.Info(ctx, "not find pool ", poolId)
		return fmt.Errorf("not find pool %d", poolId)
	}

	return nil
}

func ProcessPgMemberChangeFinish(ctx context.Context, client *etcdapi.EtcdClient, result int, poolId uint64,
		pgId uint64, osdList []int) error {
	isUpdate := false
	pgIdStr := strconv.FormatUint(pgId, 10)

	AllPools.RwMutex.RLock()
	poolConf, ok := AllPools.pools[PoolID(poolId)]
	if ok {
		poolConf.RwMutex.Lock()
		AllPools.RwMutex.RUnlock()

		locked := true
		unlockPool := func() {
			if locked {
				poolConf.RwMutex.Unlock()
				locked = false
			}
		}
		defer unlockPool()

		if pgConf, gok := poolConf.PoolPgMap.PgMap[pgIdStr]; gok {
			if result != 0 {
				if pgConf.PgInState(utils.PgRemapped) {
					log.Info(ctx, "PgMemberChangeFinishRequest, pg", poolId, ".", pgId, " in PgRemapped. result ", result)
				}
				return nil
			}

			shouldFinalize := pgConf.PgInState(utils.PgRemapped) ||
				!Compare_arry(pgConf.OsdList, osdList) ||
				len(pgConf.NewOsdList) > 0
			if !shouldFinalize {
				return nil
			}

			log.Info(ctx, "Finalize pg member change, pg", poolId, ".", pgId,
				" old osdList ", pgConf.OsdList, " new osdList ", osdList,
				" remapped ", pgConf.PgInState(utils.PgRemapped))

			pgConf.UnsetPgState(utils.PgCreating)
			pgConf.UnsetPgState(utils.PgActive)
			pgConf.UnsetPgState(utils.PgUndersize)
			pgConf.UnsetPgState(utils.PgDown)
			pgConf.UnsetPgState(utils.PgRemapped)

			state := CheckPgState(ctx, osdList, poolConf.PGSize)
			if state == 0 {
				pgConf.SetPgState(utils.PgActive)
			} else if state == utils.PgUndersize {
				pgConf.SetPgState(utils.PgUndersize)
			} else if state == utils.PgDown {
				pgConf.SetPgState(utils.PgDown)
			}

			pgConf.Version++
			pgConf.OsdList = append([]int{}, osdList...)
			pgConf.NewOsdList = pgConf.NewOsdList[:0]
			if pgConf.NewCoreIndex != math.MaxUint32 {
				pgConf.CoreIndex = pgConf.NewCoreIndex
			}
			pgConf.NewCoreIndex = math.MaxUint32
			isUpdate = true

			poolConf.PoolPgMap.PgMap[pgIdStr] = pgConf
			poolConf.PoolPgMap.Version++
			AllPools.pools[PoolID(poolId)] = poolConf
		} else {
			log.Info(ctx, "not find pg ", poolId, ".", pgId)
			return fmt.Errorf("not find pool %d.%d", poolId, pgId)
		}
		if isUpdate {
			unlockPool()
			ProcessPgTask(ctx, client, strconv.FormatUint(pgId, 10))

			pc_buf, err := json.Marshal(poolConf)
			if err != nil {
				log.Error(ctx, err)
				return nil
			}

			key := fmt.Sprintf("%s%d", config.ConfigPoolsKeyPrefix, poolId)

			err = client.Put(ctx, key, string(pc_buf))
			if err != nil {
				log.Error(ctx, err)
				return nil
			}
		}
	} else {
		AllPools.RwMutex.RUnlock()
		log.Info(ctx, "not find pool ", poolId)
		return fmt.Errorf("not find pool %d", poolId)
	}

	return nil
}

func ProcessListPoolsMessage(ctx context.Context) ([]*msg.Poolinfo, error) {
	pis := make([]*msg.Poolinfo, 0)

	var pool_array []PoolID
	AllPools.RwMutex.RLock()
	if len(AllPools.pools) == 0 {
		AllPools.RwMutex.RUnlock()
		return nil, nil
	}
	for poolID := range AllPools.pools {
		pool_array = append(pool_array, poolID)
	}
	AllPools.RwMutex.RUnlock()

	for _, poolID := range pool_array {
		AllPools.RwMutex.RLock()
		pool, ok := AllPools.pools[poolID]
		if !ok {
			AllPools.RwMutex.RUnlock()
			continue
		}
		pool.RwMutex.RLock()
		AllPools.RwMutex.RUnlock()

		pi := &msg.Poolinfo{}
		pi.Failuredomain = pool.FailureDomain
		pi.Name = pool.Name
		pi.Pgcount = int32(pool.PGCount)
		pi.Root = pool.Root
		pi.Pgsize = int32(pool.PGSize)
		pi.Poolid = int32(pool.Poolid)
		pis = append(pis, pi)
		pool.RwMutex.RUnlock()	
	}

	return pis, nil
}

func ProcessGetPgMapMessage(ctx context.Context, pvs map[int32]int64) (*msg.GetPgMapResponse, error) {
	log.Info(ctx, "ProcessGetPgMapMessage", pvs)

	var pool_array []PoolID
	AllPools.RwMutex.RLock()
	if len(AllPools.pools) == 0 {
		log.Info(ctx, "no pool created yet")
		AllPools.RwMutex.RUnlock()
		return nil, fmt.Errorf("no pool created yet")
	}
	for poolID := range AllPools.pools {
		pool_array = append(pool_array, poolID)
	}
	AllPools.RwMutex.RUnlock()

	gpmr := &msg.GetPgMapResponse{
		Errorcode:          make(map[int32]msg.GetPgMapErrorCode),
		PoolidPgmapversion: make(map[int32]int64),
		Pgs:                make(map[int32]*msg.PGInfos),
		Pools:              make(map[int32]string),
	}

	addAll := false

	//if user don't specify poolid, we will give it all pools because on start, they don't know anything
	if len(pvs) == 0 {
		addAll = true
	}
	//we return whole map
	for _, pid := range pool_array {
		AllPools.RwMutex.RLock()
		ppc, ok := AllPools.pools[pid]
		if !ok {
			AllPools.RwMutex.RUnlock()
			continue
		}
		ppc.RwMutex.RLock()
		AllPools.RwMutex.RUnlock()

		log.Info(ctx, pid, *ppc)
		if addAll {
			log.Info(ctx, "add all")
			pginfos := &msg.PGInfos{
				Pi: make([]*msg.PGInfo, 0),
			}
			gpmr.Pools[int32(pid)] = ppc.Name
			for pgid, pc := range ppc.PoolPgMap.PgMap {
				pgidToi, _ := strconv.Atoi(pgid)
				var osdlist []int32
				var newOsdlist []int32
				for _, oid := range pc.OsdList {
					osdlist = append(osdlist, int32(oid))
				}
				for _, oid := range pc.NewOsdList {
					newOsdlist = append(newOsdlist, int32(oid))
				}
				pi := &msg.PGInfo{
					Pgid:     int32(pgidToi),
					Version:  pc.Version,
					State:    int32(pc.PgState),
					Coreindex:  pc.CoreIndex,
					Osdid:    osdlist,
					Newcoreindex: pc.NewCoreIndex,
					Newosdid: newOsdlist,
				}
				pginfos.Pi = append(pginfos.Pi, pi)
				gpmr.Pgs[int32(pid)] = pginfos
				gpmr.Errorcode[int32(pid)] = msg.GetPgMapErrorCode_pgMapGetOk
				gpmr.PoolidPgmapversion[int32(pid)] = ppc.PoolPgMap.Version
			}
		} else {
			log.Info(ctx, "user specified pools")
			// case the client specified poolid and versions
			for clientPid, clientVersion := range pvs {
				if clientPid == int32(pid) {
					if clientVersion > ppc.PoolPgMap.Version {
						gpmr.Errorcode[int32(pid)] = msg.GetPgMapErrorCode_pgMapclientVersionHigher
						continue
					} else if clientVersion == ppc.PoolPgMap.Version {
						// we have exactly the same version, just return it
						gpmr.Errorcode[int32(pid)] = msg.GetPgMapErrorCode_PgMapSameVersion
					}
				}
				// for onther cases:
				// 1. client didn't specify poolid, but we have ,we should return it to them
				// 2. client specified poolid, but with lower version, we return it to them
				// (TODO)3. client specified poolid, but  we don't have, we tell them the pool is deleted
				pginfos := &msg.PGInfos{
					Pi: make([]*msg.PGInfo, 0),
				}
				gpmr.Pools[int32(pid)] = ppc.Name
				for pgid, pc := range ppc.PoolPgMap.PgMap {
					pgidToi, _ := strconv.Atoi(pgid)
					var osdlist []int32
					var newOsdlist []int32
					for _, oid := range pc.OsdList {
						osdlist = append(osdlist, int32(oid))
					}
					for _, oid := range pc.NewOsdList {
						newOsdlist = append(newOsdlist, int32(oid))
					}
					pi := &msg.PGInfo{
						Pgid:     int32(pgidToi),
						Version:  pc.Version,
						State:    int32(pc.PgState),
						Coreindex:  pc.CoreIndex,
						Osdid:    osdlist,
						Newcoreindex: pc.NewCoreIndex,
						Newosdid: newOsdlist,
					}
					pginfos.Pi = append(pginfos.Pi, pi)
					gpmr.Pgs[int32(pid)] = pginfos
					gpmr.Errorcode[int32(pid)] = msg.GetPgMapErrorCode_pgMapGetOk
					gpmr.PoolidPgmapversion[int32(pid)] = ppc.PoolPgMap.Version
				}
			}
		}
		ppc.RwMutex.RUnlock()
	}
	log.Info(ctx, "ProcessGetPgMapMessage done, got pools: ", len(gpmr.PoolidPgmapversion), gpmr)

	return gpmr, nil
}

func ProcessDeletePoolMessage(ctx context.Context, client *etcdapi.EtcdClient, name string) error {
	found := false
	pid := -1

	AllPools.RwMutex.Lock()
	for _, pc := range AllPools.pools {
		if name == pc.Name {
			pid = pc.Poolid
			found = true
			break
		}
	}
	if !found {
		AllPools.RwMutex.Unlock()
		return EPoolNotExist
	}

	pool := AllPools.pools[PoolID(pid)]
	pool.RwMutex.Lock()
	AllPools.RwMutex.Unlock()

	key := fmt.Sprintf("%s%d", config.ConfigPoolsKeyPrefix, pid)

	err := client.Delete(ctx, key)
	if err != nil {
		pool.RwMutex.Unlock()
		log.Error(ctx, err)
		return EInternalError
	}

	// remove the pool id from the map
	delete(AllPools.pools, PoolID(pid))
	pool.RwMutex.Unlock()

	log.Info(ctx, "successfully deleted pool from etcd", pid)
	return nil
}

func PushPgTask(pgId string, osdid int, stateSwitch STATESWITCH) {
	queue := pgsTaskQueue[pgId]
	queue.Enqueue(pgTask{osdid: int(osdid), stateSwitch: stateSwitch})
}

func ProcessPgTask(ctx context.Context, client *etcdapi.EtcdClient, pgId string) {
	if queue, ok := pgsTaskQueue[pgId]; ok {
		queueSize := queue.Size()
		for i := 0; i < queueSize; i++ {
			item := queue.Dequeue()
			if task, ok := item.(pgTask); ok {
				log.Info(ctx, "process pg task for pg ", pgId, " : osd ", task.osdid, " state ", task.stateSwitch)
				CheckPgs(ctx, client, task.osdid, task.stateSwitch)
			}
		}
	}
}

func printAllPool(ctx context.Context) {
	var pool_array []PoolID
	AllPools.RwMutex.RLock()
	for poolID := range AllPools.pools {
		pool_array = append(pool_array, poolID)
	}
	AllPools.RwMutex.RUnlock()

	for _, poolId := range pool_array {
		AllPools.RwMutex.RLock()
		pool, ok := AllPools.pools[poolId]
		if !ok {
			AllPools.RwMutex.RUnlock()
			continue
		}
		pool.RwMutex.RLock()
		AllPools.RwMutex.RUnlock()

		log.Warn(ctx, "---- pool ", poolId, " PGSize ", pool.PGSize, " Version ", pool.PoolPgMap.Version)
		for pgId, pg := range pool.PoolPgMap.PgMap {
			log.Warn(ctx, "pg ", pgId, " Version ", pg.Version, " PgState ", pg.PgState)
		}
		pool.RwMutex.RUnlock()
	}
}

func generatePgOsdInfoRsp(ppc *PoolConfig, gpo *msg.GetPgOsdInfoResponse) {
	pgOsdInfos := &msg.PgOsdInfos{
		Pgs: make([]*msg.PgOsdInfo, 0),
	}

	for pgid, pc := range ppc.PoolPgMap.PgMap {
		pgidToi, _ := strconv.Atoi(pgid)
		var osdlist []int32

		for _, oid := range pc.OsdList {
			osdlist = append(osdlist, int32(oid))
		}
		poi := &msg.PgOsdInfo{
			Pgid:     int32(pgidToi),
			Coreindex:  pc.CoreIndex,
			Osdid:    osdlist,
		}
		pgOsdInfos.Pgs = append(pgOsdInfos.Pgs, poi)
	}
	gpo.Pools[int32(ppc.Poolid)] = pgOsdInfos
}

func ProcessGetPgOsdInfoMessage(ctx context.Context, poolIds []int32) (*msg.GetPgOsdInfoResponse, error) {
	var pool_array []PoolID
	AllPools.RwMutex.RLock()
	if len(AllPools.pools) == 0 {
		AllPools.RwMutex.RUnlock()
		log.Info(ctx, "no pool created yet")
		return nil, fmt.Errorf("no pool created yet")
	}
	for poolID := range AllPools.pools {
		pool_array = append(pool_array, poolID)
	}
	AllPools.RwMutex.RUnlock()
	
	gpo := &msg.GetPgOsdInfoResponse{
		Pools:                make(map[int32]*msg.PgOsdInfos),
	}

	if len(poolIds) == 0 {
		for _, poolId := range pool_array {
			AllPools.RwMutex.RLock()
			ppc, ok := AllPools.pools[poolId]
			if !ok {
				AllPools.RwMutex.RUnlock()
				continue
			}
			ppc.RwMutex.RLock()
			AllPools.RwMutex.RUnlock()			
			generatePgOsdInfoRsp(ppc, gpo)
			ppc.RwMutex.RUnlock()
		}
	} else {
		for _, poolid := range poolIds {
			AllPools.RwMutex.RLock()
			ppc, ok := AllPools.pools[PoolID(poolid)]
			if !ok {
				AllPools.RwMutex.RUnlock()
				continue
			}
			ppc.RwMutex.RLock()
			AllPools.RwMutex.RUnlock()			

			generatePgOsdInfoRsp(AllPools.pools[PoolID(poolid)], gpo)
			ppc.RwMutex.RUnlock()
		}
	}
	log.Info(ctx, "ProcessGetPgOsdInfoMessage done")
	return gpo, nil
}
