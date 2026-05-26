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

func ListVolumes(ctx context.Context, client *etcdapi.EtcdClient) (msg.CSIMetadataErrorCode, []*msg.CSIVolumeMetadata) {
	if client == nil {
		return msg.CSIMetadataErrorCode_csiMetadataInvalidArgument, nil
	}
	kvs, err := client.GetWithPrefix(ctx, config.ConfigCSIVolumesKeyPrefix)
	if err != nil {
		return msg.CSIMetadataErrorCode_csiMetadataInternalError, nil
	}
	volumes := make([]*msg.CSIVolumeMetadata, 0, len(kvs))
	for _, kv := range kvs {
		metadata := &msg.CSIVolumeMetadata{}
		if err := json.Unmarshal([]byte(kv.Value), metadata); err != nil {
			return msg.CSIMetadataErrorCode_csiMetadataInternalError, nil
		}
		volumes = append(volumes, metadata)
	}
	return msg.CSIMetadataErrorCode_csiMetadataOk, volumes
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

func ListAttachments(ctx context.Context, client *etcdapi.EtcdClient) (msg.CSIMetadataErrorCode, []*msg.CSIAttachment) {
	if client == nil {
		return msg.CSIMetadataErrorCode_csiMetadataInvalidArgument, nil
	}
	kvs, err := client.GetWithPrefix(ctx, config.ConfigCSIAttachmentsKeyPrefix)
	if err != nil {
		return msg.CSIMetadataErrorCode_csiMetadataInternalError, nil
	}
	attachments := make([]*msg.CSIAttachment, 0, len(kvs))
	for _, kv := range kvs {
		attachment := &msg.CSIAttachment{}
		if err := json.Unmarshal([]byte(kv.Value), attachment); err != nil {
			return msg.CSIMetadataErrorCode_csiMetadataInternalError, nil
		}
		attachments = append(attachments, attachment)
	}
	return msg.CSIMetadataErrorCode_csiMetadataOk, attachments
}

func AcquireLease(ctx context.Context, client *etcdapi.EtcdClient, request *msg.AcquireCSILeaseRequest) (msg.CSILeaseErrorCode, *msg.CSIVolumeLease) {
	if client == nil || request == nil {
		return msg.CSILeaseErrorCode_csiLeaseInvalidArgument, nil
	}
	if !validLeaseRequest(request.GetVolumeId(), request.GetNodeId(), request.GetHostNqn(), request.GetTtlSeconds()) {
		return msg.CSILeaseErrorCode_csiLeaseInvalidArgument, nil
	}
	key := leaseKey(request.GetVolumeId())

	entry, err := client.GetEntry(ctx, key)
	if err == nil {
		lease, err := leaseFromEntry(entry)
		if err != nil {
			return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
		}
		if sameLeaseOwner(lease, request.GetNodeId(), request.GetHostNqn()) {
			if err := client.KeepAliveOnce(ctx, entry.Lease); err != nil {
				return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
			}
			return msg.CSILeaseErrorCode_csiLeaseOk, lease
		}
		return msg.CSILeaseErrorCode_csiLeaseConflict, lease
	}
	if err != etcdapi.ErrorKeyNotFound {
		return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
	}

	leaseID, err := client.Grant(ctx, request.GetTtlSeconds())
	if err != nil {
		return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
	}
	lease := &msg.CSIVolumeLease{
		VolumeId:   request.GetVolumeId(),
		NodeId:     request.GetNodeId(),
		HostNqn:    request.GetHostNqn(),
		LeaseId:    int64(leaseID),
		TtlSeconds: request.GetTtlSeconds(),
	}
	data, err := json.Marshal(lease)
	if err != nil {
		return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
	}
	ok, err := client.PutAndLease(ctx, key, string(data), leaseID)
	if err != nil {
		return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
	}
	if ok {
		return msg.CSILeaseErrorCode_csiLeaseOk, lease
	}

	entry, err = client.GetEntry(ctx, key)
	if err != nil {
		if err == etcdapi.ErrorKeyNotFound {
			return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
		}
		return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
	}
	existing, err := leaseFromEntry(entry)
	if err != nil {
		return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
	}
	if sameLeaseOwner(existing, request.GetNodeId(), request.GetHostNqn()) {
		return msg.CSILeaseErrorCode_csiLeaseOk, existing
	}
	return msg.CSILeaseErrorCode_csiLeaseConflict, existing
}

func GetLease(ctx context.Context, client *etcdapi.EtcdClient, volumeID string) (msg.CSILeaseErrorCode, *msg.CSIVolumeLease) {
	if client == nil || strings.TrimSpace(volumeID) == "" {
		return msg.CSILeaseErrorCode_csiLeaseInvalidArgument, nil
	}
	entry, err := client.GetEntry(ctx, leaseKey(volumeID))
	if err != nil {
		if err == etcdapi.ErrorKeyNotFound {
			return msg.CSILeaseErrorCode_csiLeaseNotFound, nil
		}
		return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
	}
	lease, err := leaseFromEntry(entry)
	if err != nil {
		return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
	}
	return msg.CSILeaseErrorCode_csiLeaseOk, lease
}

func RenewLease(ctx context.Context, client *etcdapi.EtcdClient, request *msg.RenewCSILeaseRequest) (msg.CSILeaseErrorCode, *msg.CSIVolumeLease) {
	if client == nil || request == nil {
		return msg.CSILeaseErrorCode_csiLeaseInvalidArgument, nil
	}
	if !validLeaseRequest(request.GetVolumeId(), request.GetNodeId(), request.GetHostNqn(), 1) {
		return msg.CSILeaseErrorCode_csiLeaseInvalidArgument, nil
	}
	entry, err := client.GetEntry(ctx, leaseKey(request.GetVolumeId()))
	if err != nil {
		if err == etcdapi.ErrorKeyNotFound {
			return msg.CSILeaseErrorCode_csiLeaseNotFound, nil
		}
		return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
	}
	lease, err := leaseFromEntry(entry)
	if err != nil {
		return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
	}
	if !sameLeaseOwner(lease, request.GetNodeId(), request.GetHostNqn()) {
		return msg.CSILeaseErrorCode_csiLeaseConflict, lease
	}
	if err := client.KeepAliveOnce(ctx, entry.Lease); err != nil {
		return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
	}
	return msg.CSILeaseErrorCode_csiLeaseOk, lease
}

func ReleaseLease(ctx context.Context, client *etcdapi.EtcdClient, request *msg.ReleaseCSILeaseRequest) msg.CSILeaseErrorCode {
	if client == nil || request == nil {
		return msg.CSILeaseErrorCode_csiLeaseInvalidArgument
	}
	if !validLeaseRequest(request.GetVolumeId(), request.GetNodeId(), request.GetHostNqn(), 1) {
		return msg.CSILeaseErrorCode_csiLeaseInvalidArgument
	}
	entry, err := client.GetEntry(ctx, leaseKey(request.GetVolumeId()))
	if err != nil {
		if err == etcdapi.ErrorKeyNotFound {
			return msg.CSILeaseErrorCode_csiLeaseOk
		}
		return msg.CSILeaseErrorCode_csiLeaseInternalError
	}
	lease, err := leaseFromEntry(entry)
	if err != nil {
		return msg.CSILeaseErrorCode_csiLeaseInternalError
	}
	if !sameLeaseOwner(lease, request.GetNodeId(), request.GetHostNqn()) {
		return msg.CSILeaseErrorCode_csiLeaseConflict
	}
	if entry.Lease != 0 {
		if err := client.Revoke(ctx, entry.Lease); err == nil {
			return msg.CSILeaseErrorCode_csiLeaseOk
		}
	}
	if err := client.Delete(ctx, leaseKey(request.GetVolumeId())); err != nil {
		return msg.CSILeaseErrorCode_csiLeaseInternalError
	}
	return msg.CSILeaseErrorCode_csiLeaseOk
}

func ListLeases(ctx context.Context, client *etcdapi.EtcdClient) (msg.CSILeaseErrorCode, []*msg.CSIVolumeLease) {
	if client == nil {
		return msg.CSILeaseErrorCode_csiLeaseInvalidArgument, nil
	}
	kvs, err := client.GetWithPrefix(ctx, config.ConfigCSILeasesKeyPrefix)
	if err != nil {
		return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
	}
	leases := make([]*msg.CSIVolumeLease, 0, len(kvs))
	for _, kv := range kvs {
		lease := &msg.CSIVolumeLease{}
		if err := json.Unmarshal([]byte(kv.Value), lease); err != nil {
			return msg.CSILeaseErrorCode_csiLeaseInternalError, nil
		}
		leases = append(leases, lease)
	}
	return msg.CSILeaseErrorCode_csiLeaseOk, leases
}

func volumeKey(volumeID string) string {
	return config.ConfigCSIVolumesKeyPrefix + encodeVolumeID(volumeID)
}

func attachmentKey(volumeID string) string {
	return config.ConfigCSIAttachmentsKeyPrefix + encodeVolumeID(volumeID)
}

func leaseKey(volumeID string) string {
	return config.ConfigCSILeasesKeyPrefix + encodeVolumeID(volumeID)
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

func validLeaseRequest(volumeID, nodeID, hostNQN string, ttlSeconds int64) bool {
	if strings.TrimSpace(volumeID) == "" || strings.TrimSpace(nodeID) == "" || strings.TrimSpace(hostNQN) == "" {
		return false
	}
	return ttlSeconds > 0
}

func leaseFromEntry(entry etcdapi.Entry) (*msg.CSIVolumeLease, error) {
	lease := &msg.CSIVolumeLease{}
	if err := json.Unmarshal([]byte(entry.Value), lease); err != nil {
		return nil, err
	}
	lease.LeaseId = int64(entry.Lease)
	return lease, nil
}

func sameLeaseOwner(lease *msg.CSIVolumeLease, nodeID, hostNQN string) bool {
	if lease == nil {
		return false
	}
	return strings.TrimSpace(lease.GetNodeId()) == strings.TrimSpace(nodeID) &&
		strings.TrimSpace(lease.GetHostNqn()) == strings.TrimSpace(hostNQN)
}
