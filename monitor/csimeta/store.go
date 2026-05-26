package csimeta

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"strings"

	"monitor/config"
	"monitor/etcdapi"
	"monitor/msg"
)

func PutVolume(ctx context.Context, client *etcdapi.EtcdClient, metadata *msg.CSIVolumeMetadata) msg.CSIMetadataErrorCode {
	if client == nil || metadata == nil {
		return msg.CSIMetadataErrorCode_csiMetadataInvalidArgument
	}
	if !validVolumeMetadata(metadata) {
		return msg.CSIMetadataErrorCode_csiMetadataInvalidArgument
	}
	data, err := json.Marshal(metadata)
	if err != nil {
		return msg.CSIMetadataErrorCode_csiMetadataInternalError
	}
	if err := client.Put(ctx, volumeKey(metadata.GetVolumeId()), string(data)); err != nil {
		return msg.CSIMetadataErrorCode_csiMetadataInternalError
	}
	return msg.CSIMetadataErrorCode_csiMetadataOk
}

func GetVolume(ctx context.Context, client *etcdapi.EtcdClient, volumeID string) (msg.CSIMetadataErrorCode, *msg.CSIVolumeMetadata) {
	if client == nil || strings.TrimSpace(volumeID) == "" {
		return msg.CSIMetadataErrorCode_csiMetadataInvalidArgument, nil
	}
	value, err := client.Get(ctx, volumeKey(volumeID))
	if err != nil {
		if err == etcdapi.ErrorKeyNotFound {
			return msg.CSIMetadataErrorCode_csiMetadataNotFound, nil
		}
		return msg.CSIMetadataErrorCode_csiMetadataInternalError, nil
	}
	metadata := &msg.CSIVolumeMetadata{}
	if err := json.Unmarshal([]byte(value), metadata); err != nil {
		return msg.CSIMetadataErrorCode_csiMetadataInternalError, nil
	}
	return msg.CSIMetadataErrorCode_csiMetadataOk, metadata
}

func DeleteVolume(ctx context.Context, client *etcdapi.EtcdClient, volumeID string) msg.CSIMetadataErrorCode {
	if client == nil || strings.TrimSpace(volumeID) == "" {
		return msg.CSIMetadataErrorCode_csiMetadataInvalidArgument
	}
	if err := client.Delete(ctx, volumeKey(volumeID)); err != nil {
		return msg.CSIMetadataErrorCode_csiMetadataInternalError
	}
	return msg.CSIMetadataErrorCode_csiMetadataOk
}

func PutAttachment(ctx context.Context, client *etcdapi.EtcdClient, attachment *msg.CSIAttachment) msg.CSIMetadataErrorCode {
	if client == nil || attachment == nil {
		return msg.CSIMetadataErrorCode_csiMetadataInvalidArgument
	}
	if !validAttachment(attachment) {
		return msg.CSIMetadataErrorCode_csiMetadataInvalidArgument
	}
	data, err := json.Marshal(attachment)
	if err != nil {
		return msg.CSIMetadataErrorCode_csiMetadataInternalError
	}
	if err := client.Put(ctx, attachmentKey(attachment.GetVolumeId()), string(data)); err != nil {
		return msg.CSIMetadataErrorCode_csiMetadataInternalError
	}
	return msg.CSIMetadataErrorCode_csiMetadataOk
}

func GetAttachment(ctx context.Context, client *etcdapi.EtcdClient, volumeID string) (msg.CSIMetadataErrorCode, *msg.CSIAttachment) {
	if client == nil || strings.TrimSpace(volumeID) == "" {
		return msg.CSIMetadataErrorCode_csiMetadataInvalidArgument, nil
	}
	value, err := client.Get(ctx, attachmentKey(volumeID))
	if err != nil {
		if err == etcdapi.ErrorKeyNotFound {
			return msg.CSIMetadataErrorCode_csiMetadataNotFound, nil
		}
		return msg.CSIMetadataErrorCode_csiMetadataInternalError, nil
	}
	attachment := &msg.CSIAttachment{}
	if err := json.Unmarshal([]byte(value), attachment); err != nil {
		return msg.CSIMetadataErrorCode_csiMetadataInternalError, nil
	}
	return msg.CSIMetadataErrorCode_csiMetadataOk, attachment
}

func DeleteAttachment(ctx context.Context, client *etcdapi.EtcdClient, volumeID string) msg.CSIMetadataErrorCode {
	if client == nil || strings.TrimSpace(volumeID) == "" {
		return msg.CSIMetadataErrorCode_csiMetadataInvalidArgument
	}
	if err := client.Delete(ctx, attachmentKey(volumeID)); err != nil {
		return msg.CSIMetadataErrorCode_csiMetadataInternalError
	}
	return msg.CSIMetadataErrorCode_csiMetadataOk
}

func volumeKey(volumeID string) string {
	return config.ConfigCSIVolumesKeyPrefix + encodeVolumeID(volumeID)
}

func attachmentKey(volumeID string) string {
	return config.ConfigCSIAttachmentsKeyPrefix + encodeVolumeID(volumeID)
}

func encodeVolumeID(volumeID string) string {
	return base64.RawURLEncoding.EncodeToString([]byte(strings.TrimSpace(volumeID)))
}

func validVolumeMetadata(metadata *msg.CSIVolumeMetadata) bool {
	if strings.TrimSpace(metadata.GetVolumeId()) == "" {
		return false
	}
	if strings.TrimSpace(metadata.GetPoolName()) == "" || strings.TrimSpace(metadata.GetImageName()) == "" {
		return false
	}
	if metadata.GetCapacityBytes() <= 0 || metadata.GetObjectSize() <= 0 || metadata.GetBlockSize() <= 0 {
		return false
	}
	transport := strings.TrimSpace(metadata.GetTransport())
	return transport == "rdma" || transport == "tcp"
}

func validAttachment(attachment *msg.CSIAttachment) bool {
	if strings.TrimSpace(attachment.GetVolumeId()) == "" {
		return false
	}
	if strings.TrimSpace(attachment.GetNodeId()) == "" {
		return false
	}
	if strings.TrimSpace(attachment.GetExportId()) == "" {
		return false
	}
	return strings.TrimSpace(attachment.GetHostNqn()) != ""
}
