/* Copyright (c) 2023 ChinaUnicom
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
	"strconv"

	"monitor/config"
	"monitor/etcdapi"
	"monitor/log"
)

// BucketConfig is /prefix/config/topology/XXX/YYY
type BucketConfig struct {
	Name       string `json:"name,omitempty"`
	ParentName string `json:"parent,omitempty"`
	Level      Level  `json:"level,omitempty"`
}

// Size.
const (
	KiB = 1024
	MiB = (1024 * KiB)
	GiB = (1024 * MiB)
	TiB = (1024 * GiB)
)

type Level string

// rack doesn't work but we add it for now
var allValidLevels = map[Level]bool{"osd": true, "host": true, "rack": true, "root": true}

func (level Level) IsValid() bool {
	return allValidLevels[level]
}

func (level Level) IsOSD() bool {
	return level == "osd"
}

// AllHostsMap key is host name configured in OSD, like "hostname1.XXX.YYY".
var AllHostsMap *map[string]BucketConfig
var AllRacksMap *map[string]BucketConfig
var AllRootsMap *map[string]BucketConfig

// UpdateHost fetches and updates all the /config/hosts to AllHostsMap.
func UpdateHost(ctx context.Context, client *etcdapi.EtcdClient) error {
	hosts, err := updateBucket(ctx, config.ConfigHostsPrefix, client)
	if err != nil {
		return err
	}

	AllHostsMap = hosts

	return nil
}

func UpdateRack(ctx context.Context, client *etcdapi.EtcdClient) error {
	prefix := config.ConfigRacksPrefix

	racks, err := updateBucket(ctx, prefix, client)
	if err != nil {
		return err
	}

	AllRacksMap = racks

	return nil
}

func UpdateRoot(ctx context.Context, client *etcdapi.EtcdClient) error {
	prefix := config.ConfigRootsPrefix

	roots, err := updateBucket(ctx, prefix, client)
	if err != nil {
		return err
	}

	AllRootsMap = roots

	return nil
}

// HostID is for "hostA", "hostB", "hostC", ...
type HostID string

// HostTreeNode all the OSDs info in cluster, up and down
// It contains only necessary ID to indicate the topology. The detailed info are kept somewhere else.
// TODO: add necessary info in it. Should it be generated everytime, or update when Update from state or stats?
type BucketTreeNode struct {
	BucketName string
	Level
	ChildNode map[string]*BucketTreeNode
}

const (
	MinOSDID = 0
)

func (id OSDID) String() string {
	return strconv.FormatUint(uint64(id), 10)
}

// (todo) remove this
func (id OSDID) IsValid() bool {
	return id >= MinOSDID
}

// OSDTreeNode for lp optimizer calculation later.
type OSDTreeNode struct {
	OSDID
	Level
	Weight float64
}

// GetOSDTreeUp return up osd, level and osd tree. It won't update data from etcd.
// If a bucket contains no up child, it won't be returned.
func GetOSDTreeUp(ctx context.Context) (treeMap *map[Level]*map[string]*BucketTreeNode,
	osdNodeMap *map[string]OSDTreeNode, err error) {
	if AllOSDInfo.Osdinfo == nil {
		return nil, nil, errors.New("no OSD state")
	}

	allTreeMap := make(map[Level]*map[string]*BucketTreeNode) // The whole tree with all level, root/rack/...
	hostTreeMap := make(map[string]*BucketTreeNode)           // Host ID to bucket.
	osdTreeMap := make(map[string]*BucketTreeNode)            // OSD ID to bucket.
	allOSDNodeMap := make(map[string]OSDTreeNode)             // OSD ID to osd Node map.

	// osd/state/N reported OSD up.
	for osdID, osdState := range AllOSDInfo.Osdinfo {
		if !osdState.IsUp {
			log.Info(ctx, "osd state not up!", osdID)
			continue
		}

		addOSDToHostTree(ctx, osdID, osdState, &hostTreeMap, &osdTreeMap, &allOSDNodeMap)
	}

	allTreeMap[Level("host")] = &hostTreeMap
	allTreeMap[Level("osd")] = &osdTreeMap

	// Iterate /config/topology/hosts/, Host -> Rack.
	if AllHostsMap != nil {
		racksTreeMap := make(map[string]*BucketTreeNode)
		for _, hostConfig := range *AllHostsMap {
			if hostConfig.Level != "host" {
				continue
			}
			// Only add hosts which contains OSDs.
			if hostTreeNode, ok := hostTreeMap[hostConfig.Name]; ok {
				if _, ok := racksTreeMap[hostConfig.ParentName]; !ok {
					racksTreeMap[hostConfig.ParentName] = &BucketTreeNode{
						BucketName: hostConfig.ParentName,
						Level:      "rack",
						ChildNode:  make(map[string]*BucketTreeNode),
					}
				}
				racksTreeMap[hostConfig.ParentName].ChildNode[hostConfig.Name] = hostTreeNode
			}
		}
		if len(racksTreeMap) > 0 {
			allTreeMap[Level("rack")] = &racksTreeMap
		}

		// Iterate /config/topology/racks/, Rack -> Root.
		if AllRacksMap != nil {
			rootsTreeMap := make(map[string]*BucketTreeNode)
			for _, rackConfig := range *AllRacksMap {
				if rackConfig.Level != "rack" {
					continue
				}
				// Only add roots which contains racks.
				if rackTreeNode, ok := racksTreeMap[rackConfig.Name]; ok {
					if _, ok := rootsTreeMap[rackConfig.ParentName]; !ok {
						rootsTreeMap[rackConfig.ParentName] = &BucketTreeNode{
							BucketName: rackConfig.ParentName,
							Level:      "root",
							ChildNode:  make(map[string]*BucketTreeNode),
						}
					}
					rootsTreeMap[rackConfig.ParentName].ChildNode[rackConfig.Name] = rackTreeNode
				}
			}
			if len(rootsTreeMap) > 0 {
				allTreeMap[Level("root")] = &rootsTreeMap
			}

			// Not quite necessary to parse /config/topology/root/ here. Just leave it for the future.
		}
	}

	log.Info(ctx, "GetOSDTreeUp done.")
	for _, level := range allTreeMap {
		if level != nil {
			for _, bucket := range *level {
				log.Info(ctx, bucket.BucketName, ": level:", bucket.Level, ", child num:", len(bucket.ChildNode))
			}
		}
	}

	return &allTreeMap, &allOSDNodeMap, nil
}

// osdNodeMap may contains more OSD than in hostTreeMap .
func addOSDToHostTree(ctx context.Context,
	osdID OSDID,
	osdState *OSDInfo,
	hostTreeMap *map[string]*BucketTreeNode,
	osdTreeMap *map[string]*BucketTreeNode,
	osdNodeMap *map[string]OSDTreeNode) {
	if osdState == nil || hostTreeMap == nil || osdTreeMap == nil || osdNodeMap == nil {
		return
	}
	host := string(osdState.Host)

	_, exist := (*hostTreeMap)[host]
	if !exist {
		(*hostTreeMap)[host] = &BucketTreeNode{
			BucketName: host,
			Level:      "host",
			ChildNode:  make(map[string]*BucketTreeNode),
		}
	}

	osdIDString := osdID.String()
	osdBucketNode := &BucketTreeNode{
		BucketName: osdIDString,
		Level:      "osd",
	}
	(*osdTreeMap)[osdIDString] = osdBucketNode
	(*hostTreeMap)[host].ChildNode[osdIDString] = osdBucketNode

	(*osdNodeMap)[osdIDString] = OSDTreeNode{
		OSDID:  osdID,
		Level:  "osd",
		Weight: float64(osdState.Size) / GiB,
		// Weight: float64(osdStats.Size) / GiB * osdStats.Reweight,
	}
}

func updateBucket(ctx context.Context, prefix string, client *etcdapi.EtcdClient) (*map[string]BucketConfig, error) {
	kvs, getErr := client.GetWithPrefix(ctx, prefix)
	if getErr != nil {
		log.Error(ctx, getErr)
		return nil, getErr
	}

	if len(kvs) == 0 {
		log.Info(ctx, "no", prefix, "config yet")
		return nil, nil
	}

	bucketMap := make(map[string]BucketConfig)

	for _, kv := range kvs {
		v := kv.Value

		bucketConfig := BucketConfig{}
		if err := json.Unmarshal([]byte(v), &bucketConfig); err != nil {
			log.Error(ctx, err)
			return nil, err
		}

		// key is not stored here.
		bucketMap[bucketConfig.Name] = bucketConfig
	}

	log.Warn(ctx, "updateBucket", prefix, "done:")
	for k, v := range bucketMap {
		log.Info(ctx, k, v)
	}

	return &bucketMap, nil
}

// GetFailureDomainNum returns the number of failure domains in the given tree map.
//
// It takes in a pointer to a map of Level to a map of string to BucketTreeNode, and a failureDomain string as parameters.
// The function returns an integer, which represents the number of failure domains.
func GetFailureDomainNum(treeMap *map[Level]*map[string]*BucketTreeNode, failureDomain string) int {
	if buckets := (*treeMap)[Level(failureDomain)]; buckets != nil {
		return len(*buckets)
	}
	return 0
}
