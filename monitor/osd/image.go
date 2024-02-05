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
	"strconv"
	"strings"
)

type ImageConfig struct {
	ImageID    int32  `json:"imageID,omitempty"`
	Imagename  string `json:"imagename,omitempty"`
	Poolname   string `json:"poolname,omitempty"`
	Imagesize  int64  `json:"imagesize,omitempty"`
	Objectsize int64  `json:"objectsize,omitempty"`
}

// 已经包含了pg的分配表
var Allimages map[int32]*ImageConfig

var lastImageId int32 = 0

func findUsableImageId() int32 {
	return int32(lastImageId + 1)
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

func IsImageExist(imagename string, poolname string) bool {
	if len(Allimages) == 0 {
		return false
	}

	for _, ic := range Allimages {
		if imagename == ic.Imagename && poolname == ic.Poolname {
			return true
		}
	}

	return false
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
