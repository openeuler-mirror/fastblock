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
	"monitor/config"
	"monitor/etcdapi"
	"monitor/log"
	"monitor/msg"
	"sort"
	"strconv"
	"strings"
	"time"
)

type ImageConfig struct {
	ImageID    int32  `json:"imageID,omitempty"`
	Imagename  string `json:"imagename,omitempty"`
	Poolname   string `json:"poolname,omitempty"`
	Imagesize  int64  `json:"imagesize,omitempty"`
	Objectsize int64  `json:"objectsize,omitempty"`
}

type snapshotValue struct {
	Status msg.SnapshotInfoStatus `json:"status,omitempty"`
	ID     int64                  `json:"id,omitempty"`
	Epoch  int64                  `json:"epoch,omitempty"`
}

type BySnapEpoch []*msg.SnapshotInfo

func (s BySnapEpoch) Len() int           { return len(s) }
func (s BySnapEpoch) Swap(x, y int)      { s[x], s[y] = s[y], s[x] }
func (s BySnapEpoch) Less(x, y int) bool { return s[x].Epoch < s[y].Epoch }

// 已经包含了pg的分配表
var Allimages map[int32]*ImageConfig

var MaxImageNameLength int32 = 512

var lastImageId int32 = 0
var latestSnapIDKey = "/config/snapshot/latest_id"
var snapshotPrefix = "/config/snapshot"
var snapshotEpochIndex = 6
var snapshotNameIndex = 5

func findUsableImageId() int32 {
	return int32(lastImageId + 1)
}

func isPoolExist(id uint64) bool {
	for _, pc := range AllPools {
		if id == uint64(pc.Poolid) {
			return true
		}
	}

	return false
}

func isImageExist(imagename string) bool {
	for _, image := range Allimages {
		if image.Imagename == imagename {
			return true
		}
	}

	return false
}

func findUsableSnapshotId(ctx context.Context, client *etcdapi.EtcdClient) (int64, error) {
	id_str, err := client.Get(ctx, latestSnapIDKey)
	latestID := int64(0)
	if err == etcdapi.ErrorKeyNotFound {
		err = client.Put(ctx, latestSnapIDKey, "1")
		return latestID, err
	}

	if err != nil {
		return latestID, err
	}

	latestID, err = strconv.ParseInt(id_str, 10, 64)
	if err != nil {
		return latestID, err
	}

	err = client.Put(ctx, latestSnapIDKey, strconv.FormatInt(latestID+1, 10))

	return latestID, err
}

func formatSnapshotKey(poolid uint64, image string, snapName string, epoch int64) string {
	return fmt.Sprintf("%s/%d/%s/%s/%d", snapshotPrefix, poolid, image, snapName, epoch)
}

func formatSnapshotKeyPrefix(poolid uint64, image string, snapName string) string {
	return fmt.Sprintf("%s/%d/%s/%s", snapshotPrefix, poolid, image, snapName)
}

func formatSnapshotPrefix(poolid uint64, image string) string {
	return fmt.Sprintf("%s/%d/%s", snapshotPrefix, poolid, image)
}

func snapshotFromEtcdKV(ctx context.Context, kvs []etcdapi.KeyValue) ([]*snapshotValue, error) {
	results := make([]*snapshotValue, len(kvs))
	var err error
	for i, value := range kvs {
		var snap_val snapshotValue
		err = json.Unmarshal([]byte(value.Value), &snap_val)
		results[i] = &snap_val
		if err != nil {
			return nil, err
		}
	}

	return results, nil
}

func snapshotInfoFromKV(ctx context.Context, key string, value_string string) (*msg.SnapshotInfo, error) {
	var snap_val snapshotValue
	err := json.Unmarshal([]byte(value_string), &snap_val)
	if err != nil {
		log.Error(ctx, "unmarshal snapshot value error, ", err.Error())
		return nil, err
	}

	parts := strings.Split(key, "/")
	epoch, err := strconv.ParseInt(parts[snapshotEpochIndex], 10, 64)
	if err != nil {
		log.Error(ctx, "parse snapshot info epoch string '", snap_val.ID, "', error, ", err.Error())
		return nil, err
	}
	return &msg.SnapshotInfo{
		SnapshotId:     snap_val.ID,
		SnapshotName:   parts[snapshotNameIndex],
		SnapshotStatus: snap_val.Status,
		Epoch:          epoch,
	}, nil
}

func snapshotsFromEtcdKV(ctx context.Context, kvs []etcdapi.KeyValue, startEpoch int64, limit int64) ([]*msg.SnapshotInfo, error) {
	if limit == -1 {
		limit = int64(len(kvs))
	}
	log.Debug(ctx, "limit is ", limit)
	var result []*msg.SnapshotInfo
	i := int64(0)
	for _, value := range kvs {
		if i == limit {
			break
		}
		item, err := snapshotInfoFromKV(ctx, value.Key, value.Value)
		if err != nil {
			return nil, err
		}

		if item.Epoch < startEpoch {
			continue
		}

		result = append(result, item)
		i += 1
	}

	sort.Sort(BySnapEpoch(result))
	return result, nil
}

func LoadImageConfig(ctx context.Context, client *etcdapi.EtcdClient) (err error) {
	lastImageId = 0
	Allimages = make(map[int32]*ImageConfig)

	kvs, getErr := client.GetWithPrefix(ctx, config.ConfigImagesKeyPrefix)

	if getErr != nil {
		log.Error(ctx, getErr)
		return getErr
	}

	if len(kvs) == 0 {
		log.Info(ctx, "no image created yet")
		return nil
	}

	for _, kv := range kvs {
		log.Info(ctx, "pool key:", kv.Key, ", value:", string(kv.Value))

		k := string(kv.Key)
		imageID, err := strconv.Atoi(strings.TrimPrefix(k, config.ConfigImagesKeyPrefix))
		if err != nil {
			log.Info(ctx, "failed to get poolid")
			continue
		}

		var imageConfig ImageConfig
		if jerr := json.Unmarshal([]byte(kv.Value), &imageConfig); jerr != nil {
			log.Error(ctx, jerr, string(kv.Key), string(kv.Value))
			return jerr
		}

		Allimages[int32(imageID)] = &imageConfig
		if lastSeenPoolId < int32(imageID) {
			lastSeenPoolId = int32(imageID)
		}

		log.Debug(ctx, "imageID", imageID)
	}

	log.Info(ctx, "loadPoolConfig done")
	for k, v := range AllPools {
		log.Info(ctx, k, v)
	}

	return nil
}
func ProcessCreateImageMessage(ctx context.Context, client *etcdapi.EtcdClient, imagename string, poolname string, imagesize int64, objectsize int64) msg.CreateImageErrorCode {

	if len(imagename) > int(MaxImageNameLength) {
		return msg.CreateImageErrorCode_imageNameTooLong
	}

	exist := false
	for _, pc := range AllPools {
		if poolname == pc.Name {
			exist = true
		}
	}
	if !exist {
		return msg.CreateImageErrorCode_unknownPoolName
	}

	for _, ic := range Allimages {
		if imagename == ic.Imagename && poolname == ic.Poolname {
			return msg.CreateImageErrorCode_imageExists
		}
	}

	imageID := findUsableImageId()

	imageConf := &ImageConfig{
		ImageID:    int32(imageID),
		Imagename:  imagename,
		Poolname:   poolname,
		Imagesize:  imagesize,
		Objectsize: objectsize,
	}

	ic_buf, err := json.Marshal(imageConf)
	if err != nil {
		log.Error(ctx, err)
		return msg.CreateImageErrorCode_marshalImageContextError
	}

	key := fmt.Sprintf("%s%d", config.ConfigImagesKeyPrefix, imageID)

	err = client.Put(ctx, key, string(ic_buf))
	if err != nil {
		log.Error(ctx, err)
		return msg.CreateImageErrorCode_putEtcdError
	}

	if lastImageId < int32(imageID) {
		lastImageId = int32(imageID)
	}

	Allimages[imageID] = imageConf
	log.Info(ctx, "successfully put to ectd for newly image,name :%s ", imagename)
	return msg.CreateImageErrorCode_createImageOk
}

func ProcessCreateSnapshotMessage(ctx context.Context, client *etcdapi.EtcdClient, imagename string, poolid uint64, snapname string) (msg.CreateSnapshotErrorCode, int64) {
	if !isPoolExist(poolid) {
		return msg.CreateSnapshotErrorCode_createSnapshotUnknownPoolName, -1
	}

	if !isImageExist(imagename) {
		return msg.CreateSnapshotErrorCode_createSnapshotUnknownImageName, -1
	}

	snapID, err := findUsableSnapshotId(ctx, client)
	if err != nil {
		log.Error(ctx, "find usable snapshot id failed, ", err.Error())
		return msg.CreateSnapshotErrorCode_createSnapshotIDError, snapID
	}

	val := snapshotValue{
		Status: msg.SnapshotInfoStatus_snapshotCreated,
		ID:     snapID,
		Epoch:  time.Now().UnixNano(),
	}
	val_str, err := json.Marshal(val)
	if err != nil {
		log.Error(ctx, "marshal snapshot value error, ", err.Error())
		return msg.CreateSnapshotErrorCode_createSnapshotServerError, snapID
	}
	snapKey := formatSnapshotKey(poolid, imagename, snapname, val.Epoch)
	err = client.Put(ctx, snapKey, string(val_str))
	if err != nil {
		log.Error(ctx, "put snapshot ", snapKey, " error, ", err.Error())
		return msg.CreateSnapshotErrorCode_createSnapshotPutEtcdError, snapID
	}
	log.Info(ctx, "created snapshot '", snapname, "', with etcd key ", snapKey)

	return msg.CreateSnapshotErrorCode_createSnapshotOk, snapID
}

func ProcessListSnapshotMessage(ctx context.Context, client *etcdapi.EtcdClient, imagename string, poolid uint64, startEpoch int64, limit int64) (msg.ListSnapshotErrorCode, []*msg.SnapshotInfo) {
	if !isPoolExist(poolid) {
		return msg.ListSnapshotErrorCode_listSnapshotUnknownPoolName, []*msg.SnapshotInfo{}
	}

	if !isImageExist(imagename) {
		return msg.ListSnapshotErrorCode_listSnapshotUnknownImageName, []*msg.SnapshotInfo{}
	}

	snapPrefix := formatSnapshotPrefix(poolid, imagename)
	values, err := client.GetWithSortAscend(ctx, snapPrefix)
	if err != nil {
		log.Error(ctx, "list snapshot failed, image is ", imagename, ", pool is ", poolid, ", error ", err.Error())
		return msg.ListSnapshotErrorCode_listSnapshotServerError, []*msg.SnapshotInfo{}
	}

	if len(values) == 0 {
		log.Warn(ctx, "list nothing of prefix ", snapPrefix)
		return msg.ListSnapshotErrorCode_listSnapshotOk, []*msg.SnapshotInfo{}
	}

	log.Debug(ctx, "startEpoch is ", startEpoch, ", limit is ", limit)
	infos, err := snapshotsFromEtcdKV(ctx, values, startEpoch, limit)
	if err != nil {
		log.Error(ctx, "read snapshot info from etcd kv error")
		return msg.ListSnapshotErrorCode_listSnapshotServerError, nil
	}

	return msg.ListSnapshotErrorCode_listSnapshotOk, infos
}

func ProcessDeleteSnapshotMessage(ctx context.Context, client *etcdapi.EtcdClient, imagename string, poolid uint64, snapName string) msg.DeleteSnapshotErrorCode {
	if !isPoolExist(poolid) {
		return msg.DeleteSnapshotErrorCode_deleteSnapshotUnknownPoolName
	}

	if !isImageExist(imagename) {
		return msg.DeleteSnapshotErrorCode_deleteSnapshotUnknownImageName
	}

	snapKeyPrefix := formatSnapshotKeyPrefix(poolid, imagename, snapName)
	values, err := client.GetWithSortAscend(ctx, snapKeyPrefix)
	if err == etcdapi.ErrorKeyNotFound {
		log.Info(ctx, "empty result of getting ", snapKeyPrefix)
		return msg.DeleteSnapshotErrorCode_deleteSnapshotNotFound
	}
	if len(values) == 0 {
		log.Warn(ctx, "delete nothing for snapshot ", snapName, ", with prefix ", snapKeyPrefix)
		return msg.DeleteSnapshotErrorCode_deleteSnapshotOk
	}

	if err != nil {
		log.Error(ctx, "get ectd with snapshot of ", snapName, " error, ", err.Error())
		return msg.DeleteSnapshotErrorCode_deleteSnapshotError
	}
	infos, err := snapshotFromEtcdKV(ctx, values)
	if err != nil {
		log.Error(ctx, "read snapshot info from etcd kv error, ", err.Error())
		return msg.DeleteSnapshotErrorCode_deleteSnapshotError
	}

	lastSnapVal := infos[len(infos)-1]
	lastSnapKey := formatSnapshotKey(poolid, imagename, snapName, lastSnapVal.Epoch)
	if lastSnapVal.Status == msg.SnapshotInfoStatus_snapshotDeleted {
		return msg.DeleteSnapshotErrorCode_deleteSnapshotOk
	}
	lastSnapVal.Status = msg.SnapshotInfoStatus_snapshotDeleted
	lastSnapValByte, err := json.Marshal(lastSnapVal)
	if err != nil {
		log.Error(ctx, "marshal snapshot etcd value error, ", err.Error())
		return msg.DeleteSnapshotErrorCode_deleteSnapshotError
	}
	lastSnapVal.Epoch = time.Now().UnixNano()
	newLastSnapKey := formatSnapshotKey(poolid, imagename, snapName, lastSnapVal.Epoch)

	deleteTxn := client.NewTxn()
	deleteTxn.Delete(lastSnapKey)
	deleteTxn.Put(newLastSnapKey, string(lastSnapValByte))
	deleteTxnResp, err := deleteTxn.Commit(ctx)
	if err != nil {
		log.Error(ctx, "transaction to delete", lastSnapKey, ", put ", newLastSnapKey, " error, ", err.Error())
		return msg.DeleteSnapshotErrorCode_deleteSnapshotError
	}

	if deleteTxnResp.Succeeded {
		log.Info(ctx, "transaction to delete", lastSnapKey, ", put ", newLastSnapKey, " success")
		return msg.DeleteSnapshotErrorCode_deleteSnapshotOk
	}

	log.Error(ctx, "transaction to delete '", snapName, "' failed")
	return msg.DeleteSnapshotErrorCode_deleteSnapshotError
}

func ProcessRemoveImageMessage(ctx context.Context, client *etcdapi.EtcdClient, imagename string, poolname string) (msg.RemoveImageErrorCode, *ImageConfig) {
	found := false
	var imageid int32 = -1
	for _, ic := range Allimages {
		if imagename == ic.Imagename && poolname == ic.Poolname {
			imageid = ic.ImageID
			found = true
			break
		}
	}

	if !found {
		return msg.RemoveImageErrorCode_imageNotFound, nil
	}

	key := fmt.Sprintf("%s%d", config.ConfigImagesKeyPrefix, imageid)

	err := client.Delete(ctx, key)

	if err != nil {
		log.Error(ctx, err)
		return msg.RemoveImageErrorCode_removeImageFail, nil
	}
	//remove the pool id from the map
	imageinfo := Allimages[imageid]
	delete(Allimages, imageid)

	log.Info(ctx, "successfully deleted image '", imagename, "' from etcd")
	return msg.RemoveImageErrorCode_removeImageOk, imageinfo
}

func ProcessGetImageMessage(ctx context.Context, imagename string, poolname string) (msg.GetImageErrorCode, *ImageConfig) {

	log.Info(ctx, "  ProcessGetImageMessage   ")
	if len(Allimages) == 0 {
		log.Info(ctx, "no image created yet")
		return msg.GetImageErrorCode_getImageNotFound, nil
	}

	for _, ic := range Allimages {
		if imagename == ic.Imagename && poolname == ic.Poolname {
			return msg.GetImageErrorCode_getImageOk, Allimages[ic.ImageID]
		}
	}
	return msg.GetImageErrorCode_getImageNotFound, nil
}

func ProcessResizeImageMessage(ctx context.Context, client *etcdapi.EtcdClient, imagename string, poolname string, imagesize int64) (msg.ResizeImageErrorCode, *ImageConfig) {
	var imageID int32 = -1
	for _, ic := range Allimages {
		if imagename == ic.Imagename && poolname == ic.Poolname {
			imageID = ic.ImageID
			break
		}
	}
	if imageID == -1 {
		log.Info(ctx, "no image created yet")
		return msg.ResizeImageErrorCode_resizeImageNotFound, nil
	}

	imageConf := &ImageConfig{
		ImageID:    int32(imageID),
		Imagename:  imagename,
		Poolname:   poolname,
		Imagesize:  imagesize,
		Objectsize: Allimages[imageID].Objectsize,
	}
	log.Info(ctx, "resize image with new size ", imagesize)

	ic_buf, err := json.Marshal(imageConf)
	if err != nil {
		log.Error(ctx, err)
		return msg.ResizeImageErrorCode_marshalResizeImageContextError, nil
	}

	key := fmt.Sprintf("%s%d", config.ConfigImagesKeyPrefix, imageID)

	err = client.Put(ctx, key, string(ic_buf))
	if err != nil {
		log.Error(ctx, err)
		return msg.ResizeImageErrorCode_putResizeImageEtcdError, nil
	}

	if lastImageId < int32(imageID) {
		lastImageId = int32(imageID)
	}

	Allimages[imageID] = imageConf
	log.Info(ctx, "successfully put to ectd for newly image: ", imagename)
	return msg.ResizeImageErrorCode_resizeImageOk, imageConf
}
