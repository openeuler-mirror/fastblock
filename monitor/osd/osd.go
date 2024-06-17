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
	"monitor/config"
	"monitor/etcdapi"
	"monitor/log"
	"monitor/msg"
	"net"
	"time"
)

type OSDID int

// (fixme) make it configurable
const (
	heartbeatInterval = 3 * time.Second
	// min hearbeats before we mark it up
	minSuccessAttempts = 1
	osdDownInterval    = 10 * time.Second
	osdDownOutInterval = 30 * time.Second
)

func isValidIPv4(address string) bool {
	return net.ParseIP(address) != nil
}

func isValidPort(port uint32) bool {
	return port <= 65535
}

// when osd restarts, following infomation is changed(host may not)
type OSDInfo struct {
	Osdid           int    `json:"osdid"`
	Address         string `json:"address"`
	Uuid            string `json:"uuid"`
	Host            string `json:"host"`
	Port            uint32 `json:"port"`
	Size            int64  `json:"size"`
	IsIn            bool   `json:"isin"`
	IsUp            bool   `json:"isup"`
	IsPendingCreate bool   `json:"ispendingcreate"`
}

type OsdMap struct {
	Version int64              `json:"version"`
	Osdinfo map[OSDID]*OSDInfo `json:"osdinfo"`
}

type HeartBeatInfo struct {
	osdid          OSDID
	lastHeartBeat  time.Time
	successCounter int
}

var AllOSDInfo OsdMap
var AllHeartBeatInfo map[OSDID]*HeartBeatInfo

type STATESWITCH int

const (
	InToOut STATESWITCH = iota + 1
	DownToUp
	UpToDown
	OutToIn
)

var NoReblance = false
var NoOut = false

type OsdTask struct {
	osdid       int
	stateSwitch STATESWITCH
}

var osdTaskQueue = NewQueue()

func (task *OsdTask) Process(ctx context.Context, client *etcdapi.EtcdClient) {
	log.Info(ctx, "osd: ", task.osdid, " stateSwitch: ", task.stateSwitch)
	if task.stateSwitch == OutToIn {
		Reblance(ctx, client)
	} else {
		CheckPgs(ctx, client, task.osdid, task.stateSwitch)
	}
}

func OsdTaskrun(ctx context.Context, client *etcdapi.EtcdClient) {
	taskTick := time.NewTicker(1 * time.Second)
	for range taskTick.C {
		for !osdTaskQueue.IsEmpty() {
			item := osdTaskQueue.Dequeue()
			if task, ok := item.(OsdTask); ok {
				task.Process(ctx, client)
			}
		}
	}
}

// findUsableOsdId finds the first available OSD ID that does not exist in AllOSDInfo.
func findUsableOsdId() int {
	//let's start from 1, because gogo-protoc omit zero values, stupid...
	id := 1
	for {
		if _, exists := AllOSDInfo.Osdinfo[OSDID(id)]; !exists {
			return id
		}
		id++
	}
}

// load OSD state from etcd.
// Example: /osd/info/1 {"address": "10.0.0.1",  "host": "IPSSC004.TMP06.GUIGU-A", "port": 35311, "isup": true}
// should  also load pending create osds
func LoadOSDStateFromEtcd(ctx context.Context, client *etcdapi.EtcdClient) (err error) {
	AllOSDInfo.Osdinfo = make(map[OSDID]*OSDInfo)
	prefix := config.ConfigOSDMapKey
	osdMap, getErr := client.Get(ctx, prefix)
	if getErr != nil {
		log.Error(ctx, getErr)
		if getErr == etcdapi.ErrorKeyNotFound {
			AllOSDInfo.Version = 0
			return nil
		}
		return getErr
	}

	if err := json.Unmarshal([]byte(osdMap), &AllOSDInfo); err != nil {
		log.Error(ctx, err)
		return err
	}

	log.Info(ctx, "Load OSDs done, osd count is:", len(AllOSDInfo.Osdinfo), "version is", AllOSDInfo.Version)

	//since all osd info is loaded, we can start heart beat
	AllHeartBeatInfo = make(map[OSDID]*HeartBeatInfo)
	for _, info := range AllOSDInfo.Osdinfo {
		AllHeartBeatInfo[OSDID(info.Osdid)] = &HeartBeatInfo{osdid: OSDID(info.Osdid), successCounter: 0}
	}

	log.Info(ctx, "heartbeat info updated")

	return nil
}

// when a new osd is applied, we append it to AllOSDInfo.Osdinfo, and put it into etcd
func ProcessApplyIDMessage(ctx context.Context, client *etcdapi.EtcdClient, uuid string) (int, error) {
	for _, info := range AllOSDInfo.Osdinfo {
		if uuid == info.Uuid {
			return -1, EUuidAlreadyExists
		}
	}

	oid := findUsableOsdId()

	oi := &OSDInfo{
		Uuid:            uuid,
		IsPendingCreate: true,
		Osdid:           oid,
		IsIn:            false,
		IsUp:            false,
		Address:         "",
		Host:            "",
		Port:            0,
		Size:            0,
	}

	AllOSDInfo.Osdinfo[OSDID(oid)] = oi
	// AllOSDInfo.Version++

	if AllHeartBeatInfo == nil {
		AllHeartBeatInfo = make(map[OSDID]*HeartBeatInfo)
	}
	AllHeartBeatInfo[OSDID(oid)] = &HeartBeatInfo{osdid: OSDID((oid)), successCounter: 0}

	osdMap, err := json.Marshal(AllOSDInfo)
	if err != nil {
		log.Error(ctx, err)
		return -1, err
	}

	err = client.Put(ctx, config.ConfigOSDMapKey, string(osdMap))
	if err != nil {
		log.Error(ctx, err)
		return -1, err
	}

	log.Info(ctx, "successfully update osdmap after newly apply")
	return oid, nil
}

func ProcessBootMessage(ctx context.Context, client *etcdapi.EtcdClient, id int32, uuid string, size int64, port uint32, host string, address string) ERRNUM {
	// the uuid should not exist in the osd map
	found := false
	for _, info := range AllOSDInfo.Osdinfo {
		if id == int32(info.Osdid) {
			found = true
			break
		}
	}
	if !found {
		log.Warn(ctx, "osd is not found in our database.")
		return OSD_ERR_NOT_APPLY
	}
	oinfo := AllOSDInfo.Osdinfo[OSDID(id)]

	oldIsUp := oinfo.IsUp
	if oldIsUp {
		log.Warn(ctx, "osd is up.")
		return OSD_ERR_ID_CONFLICT
	}

	if !isValidIPv4(address) || !isValidPort(port) {
		log.Warn(ctx, "invalide ip or port.")
		return OSD_ERR_ADDRESS_INVALID
	}

	oldIsIn := oinfo.IsIn
	if oinfo.IsPendingCreate {
		//this is a newly create osd
		oinfo.Address = address
		oinfo.Host = host
		oinfo.Port = port
		oinfo.IsPendingCreate = false
		oinfo.IsIn = true
		oinfo.IsUp = true
		oinfo.Size = size
	} else {
		//(todo) check whether any thing changed??
		oinfo.Address = address
		oinfo.Host = host
		oinfo.Port = port
		oinfo.IsIn = true
		oinfo.IsUp = true
		oinfo.Size = size
		log.Info(ctx, "IsPendingCreate is false, this is an update osd")
	}

	AllOSDInfo.Osdinfo[OSDID(id)] = oinfo
	AllOSDInfo.Version++

	if _, ok := AllOSDInfo.Osdinfo[OSDID(id)]; ok {
		AllHeartBeatInfo[OSDID(id)].lastHeartBeat = time.Now()
	}

	osdmap, err := json.Marshal(AllOSDInfo)
	if err != nil {
		log.Error(ctx, err)
		return OSD_ERR_UPDATE_STATE_FAILED
	}

	err = client.Put(ctx, config.ConfigOSDMapKey, string(osdmap))
	if err != nil {
		log.Error(ctx, err)
		return OSD_ERR_UPDATE_STATE_FAILED
	}

	//osd由down变为up
	if !oldIsUp && oinfo.IsUp && oldIsIn && oinfo.IsIn {
		log.Info(ctx, "osd ", id, "from down to up.")
		osdTaskQueue.Enqueue(OsdTask{osdid: int(id), stateSwitch: DownToUp})
	}

	//osd由out变为in
	if !oldIsIn && oinfo.IsIn {
		log.Info(ctx, "osd ", id, "from out to in.")
		osdTaskQueue.Enqueue(OsdTask{osdid: int(id), stateSwitch: OutToIn})
	}

	log.Info(ctx, "successfully put to ectd for newly booted osd ", id)
	GetOSDTree(ctx, true, false)
	return SUCCESS
}

func ProcessGetOsdMapMessage(ctx context.Context, cv int64, oid int32) ([]*msg.OsdDynamicInfo, int64, msg.OsdMapErrorCode) {
	log.Info(ctx, "ProcessGetOsdMapMessage")
	if len(AllOSDInfo.Osdinfo) == 0 {
		log.Info(ctx, "no osd created yet")
		return nil, -1, msg.OsdMapErrorCode_noOsdsExist
	}

	//update heartbeat
	if _, ok := AllOSDInfo.Osdinfo[OSDID(oid)]; ok {
		log.Info(ctx, "true heartbeat message")
		AllHeartBeatInfo[OSDID(oid)].lastHeartBeat = time.Now()
	} else {
		// this is a normal getosdmap, not for heartbeat
		log.Info(ctx, "true getosdmap message")
	}

	if cv > AllOSDInfo.Version {
		log.Info(ctx, "client have higher version, maybe i'm not leader any more")
		return nil, -1, msg.OsdMapErrorCode_clientVersionHigher
	}
	if cv == AllOSDInfo.Version {
		log.Info(ctx, "client have equal version, maybe i'm not leader any more")
		return nil, cv, msg.OsdMapErrorCode_ok
	}

	var odi []*msg.OsdDynamicInfo
	// osd/state/N reported OSD up.
	for _, osdState := range AllOSDInfo.Osdinfo {
		var info msg.OsdDynamicInfo
		info.Osdid = int32(osdState.Osdid)
		info.Address = osdState.Address
		info.Port = int32(osdState.Port)
		info.Isin = osdState.IsIn
		info.Isup = osdState.IsUp
		odi = append(odi, &info)
	}

	log.Info(ctx, "ProcessGetOsdMapMessage done", odi)
	return odi, AllOSDInfo.Version, msg.OsdMapErrorCode_ok
}

func ProcessOsdStopMessage(ctx context.Context, client *etcdapi.EtcdClient, id int32) bool {
	log.Info(ctx, "ProcessOsdStopMessage")
	if len(AllOSDInfo.Osdinfo) == 0 {
		log.Info(ctx, "no osd created yet")
		return false
	}

	found := false
	for _, osdState := range AllOSDInfo.Osdinfo {
		if id == int32(osdState.Osdid) {
			found = true
			osdState.IsUp = false
			break
		}
	}
	if !found {
		log.Info(ctx, "osd is not found in our database,")
		return false
	}

	AllOSDInfo.Version++
	osdmap, err := json.Marshal(AllOSDInfo)
	if err != nil {
		log.Error(ctx, err)
		return false
	}

	err = client.Put(ctx, config.ConfigOSDMapKey, string(osdmap))
	if err != nil {
		log.Error(ctx, err)
		return false
	}

	log.Info(ctx, "successfully put to ectd for newly booted osd ", id)
	GetOSDTree(ctx, true, false)
	return true
}

func ProcessOsdOutMessage(ctx context.Context, client *etcdapi.EtcdClient, osdid int32) bool {
	log.Info(ctx, "ProcessOsdOutMessage osd ", osdid)
	if len(AllOSDInfo.Osdinfo) == 0 {
		log.Info(ctx, "no osd created yet")
		return false
	}

	found := false
	for _, osdState := range AllOSDInfo.Osdinfo {
		if osdid == int32(osdState.Osdid) {
			found = true
			if !osdState.IsIn {
				log.Info(ctx, "osd ", osdid, " is in the out state.")
				return true
			}
			osdState.IsIn = false
			break
		}
	}
	if !found {
		log.Info(ctx, "osd is not found in our database,")
		return false
	}

	osdTaskQueue.Enqueue(OsdTask{osdid: int(osdid), stateSwitch: InToOut})
	log.Info(ctx, "osd ", osdid, " from in to out.")

	AllOSDInfo.Version++
	osdmap, err := json.Marshal(AllOSDInfo)
	if err != nil {
		log.Error(ctx, err)
		return false
	}

	err = client.Put(ctx, config.ConfigOSDMapKey, string(osdmap))
	if err != nil {
		log.Error(ctx, err)
		return false
	}

	log.Info(ctx, "successfully put to ectd for out osd ", osdid)
	return true
}

func ProcessOsdInMessage(ctx context.Context, client *etcdapi.EtcdClient, osdid int32) bool {
	log.Info(ctx, "ProcessOsdInMessage osd ", osdid)
	if len(AllOSDInfo.Osdinfo) == 0 {
		log.Info(ctx, "no osd created yet")
		return false
	}

	found := false
	isUp := false
	for _, osdState := range AllOSDInfo.Osdinfo {
		if osdid == int32(osdState.Osdid) {
			found = true
			if osdState.IsIn {
				log.Info(ctx, "osd ", osdid, " is in the in state.")
				return true
			}
			isUp = osdState.IsUp
			if !isUp {
				log.Warn(ctx, "osd ", osdid, " is down, can not set in state.")
				return false
			}
			osdState.IsIn = true
			break
		}
	}
	if !found {
		log.Info(ctx, "osd is not found in our database,")
		return false
	}

	if isUp {
		osdTaskQueue.Enqueue(OsdTask{osdid: int(osdid), stateSwitch: OutToIn})
		log.Info(ctx, "osd ", osdid, " from out to in.")
	}
	AllOSDInfo.Version++
	osdmap, err := json.Marshal(AllOSDInfo)
	if err != nil {
		log.Error(ctx, err)
		return false
	}

	err = client.Put(ctx, config.ConfigOSDMapKey, string(osdmap))
	if err != nil {
		log.Error(ctx, err)
		return false
	}

	log.Info(ctx, "successfully put to ectd for in osd ", osdid)
	return true
}

func ProcessNoReblanceMessage(ctx context.Context, client *etcdapi.EtcdClient, set bool) bool {
	log.Info(ctx, "process  NoReblanceRequest, NoReblance: ", NoReblance, ", set: ", set)
	if NoReblance != set {
		NoReblance = set
	}
	return true
}

func ProcessNoOutMessage(ctx context.Context, client *etcdapi.EtcdClient, set bool) bool {
	log.Info(ctx, "process  NoOutRequest, NoOut: ", NoOut, ", set: ", set)
	if NoOut != set {
		NoOut = set
	}
	return true
}

func CheckOsdHeartbeat(ctx context.Context, client *etcdapi.EtcdClient) {
	// we should update the osdmap according to heartbeat info
	// if heartbeat failed up to maxFailedAttempts times, we should mark the osd down
	// we run as a time.Ticker, run every heartbeatInterval, check every osd's lastHeartBeat
	heartbeatTicker := time.NewTicker(heartbeatInterval)
	for range heartbeatTicker.C {
		isChange := false
		for _, info := range AllOSDInfo.Osdinfo {
			if !info.IsUp {
				//this osd is previously down, but now we received a heartbeat from it
				hi := AllHeartBeatInfo[OSDID(info.Osdid)]
				if hi.lastHeartBeat.Add(heartbeatInterval).After(time.Now()) {
					hi.successCounter++
					if hi.successCounter >= minSuccessAttempts {
						info.IsUp = true
						isChange = true
						hi.successCounter = 0
						osdTaskQueue.Enqueue(OsdTask{osdid: info.Osdid, stateSwitch: DownToUp})
						log.Warn(ctx, "osd ", info.Osdid, " from down to up.")
					}
				}
				if hi.lastHeartBeat.Add(osdDownInterval+osdDownOutInterval).Before(time.Now()) && info.IsIn {
					info.IsIn = false
					isChange = true
					osdTaskQueue.Enqueue(OsdTask{osdid: info.Osdid, stateSwitch: InToOut})
					log.Warn(ctx, "osd ", info.Osdid, " from in to out.")
				}
			} else {
				hi := AllHeartBeatInfo[OSDID(info.Osdid)]
				if hi.lastHeartBeat.Add(osdDownInterval).Before(time.Now()) {
					info.IsUp = false
					isChange = true
					ProcessOsdDown(ctx, client, info.Osdid)
					log.Warn(ctx, "osd ", info.Osdid, " from up to down.")
				}
			}
		}
		if isChange {
			AllOSDInfo.Version++
			osdMap, err := json.Marshal(AllOSDInfo)
			if err != nil {
				log.Error(ctx, err)
			}

			err = client.Put(ctx, config.ConfigOSDMapKey, string(osdMap))
			if err != nil {
				log.Error(ctx, err)
			}

			log.Info(ctx, "successfully update osdmap after heartbeat change")
		}
	}
}
