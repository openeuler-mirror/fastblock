package imagemeta

import (
	"context"
	"encoding/base64"
	"encoding/json"
	"errors"
	"sort"
	"strings"
	"time"

	"monitor/config"
	"monitor/etcdapi"
)

var (
	ErrImageNotFound     = errors.New("image metadata not found")
	ErrSnapshotNotFound  = errors.New("snapshot metadata not found")
	ErrOperationNotFound = errors.New("image operation not found")
)

type ImageStatus string

const (
	ImageStatusReady      ImageStatus = "ready"
	ImageStatusCreating   ImageStatus = "creating"
	ImageStatusFlattening ImageStatus = "flattening"
	ImageStatusDeleting   ImageStatus = "deleting"
)

type SnapshotStatus string

const (
	SnapshotStatusCreating         SnapshotStatus = "creating"
	SnapshotStatusReady            SnapshotStatus = "ready"
	SnapshotStatusDeleting         SnapshotStatus = "deleting"
	SnapshotStatusDeletedPendingGC SnapshotStatus = "deleted_pending_gc"
)

type OperationType string

const (
	OperationCreateSnapshot    OperationType = "create_snapshot"
	OperationDeleteSnapshot    OperationType = "delete_snapshot"
	OperationProtectSnapshot   OperationType = "protect_snapshot"
	OperationUnprotectSnapshot OperationType = "unprotect_snapshot"
	OperationCloneImage        OperationType = "clone_image"
	OperationFlattenImage      OperationType = "flatten_image"
)

type OperationStatus string

const (
	OperationStatusPending    OperationStatus = "pending"
	OperationStatusRunning    OperationStatus = "running"
	OperationStatusCommitting OperationStatus = "committing"
	OperationStatusDone       OperationStatus = "done"
	OperationStatusFailed     OperationStatus = "failed"
)

type ImageMetadata struct {
	ImageID          string      `json:"image_id"`
	PoolID           int32       `json:"pool_id,omitempty"`
	PoolName         string      `json:"pool_name"`
	ImageName        string      `json:"image_name"`
	Size             int64       `json:"size"`
	ObjectSize       int64       `json:"object_size"`
	Features         []string    `json:"features,omitempty"`
	Status           ImageStatus `json:"status"`
	ParentSnapshotID string      `json:"parent_snapshot_id,omitempty"`
	Depth            uint32      `json:"depth,omitempty"`
	CreatedAt        time.Time   `json:"created_at"`
	UpdatedAt        time.Time   `json:"updated_at"`
	Generation       uint64      `json:"generation,omitempty"`
}

type SnapshotMetadata struct {
	SnapshotID      string         `json:"snapshot_id"`
	SnapshotName    string         `json:"snapshot_name"`
	SourceImageID   string         `json:"source_image_id"`
	SourcePoolID    int32          `json:"source_pool_id,omitempty"`
	SourcePoolName  string         `json:"source_pool_name"`
	SourceImageName string         `json:"source_image_name"`
	SnapSeq         uint64         `json:"snap_seq,omitempty"`
	Status          SnapshotStatus `json:"status"`
	Protected       bool           `json:"protected,omitempty"`
	CreatedAt       time.Time      `json:"created_at"`
	UpdatedAt       time.Time      `json:"updated_at"`
	OperationID     string         `json:"operation_id,omitempty"`
	ChildCount      uint32         `json:"child_count,omitempty"`
}

type ImageOperationRecord struct {
	OperationID string          `json:"operation_id"`
	Type        OperationType   `json:"type"`
	TargetID    string          `json:"target_id"`
	Status      OperationStatus `json:"status"`
	Error       string          `json:"error,omitempty"`
	StartedAt   time.Time       `json:"started_at"`
	UpdatedAt   time.Time       `json:"updated_at"`
}

func PutImage(ctx context.Context, client *etcdapi.EtcdClient, metadata *ImageMetadata) error {
	if client == nil || metadata == nil {
		return errors.New("client and image metadata are required")
	}
	if err := metadata.normalizeAndValidate(); err != nil {
		return err
	}
	data, err := json.Marshal(metadata)
	if err != nil {
		return err
	}
	return client.NewTxn().
		Put(imageKey(metadata.ImageID), string(data)).
		Put(imageNameKey(metadata.PoolName, metadata.ImageName), metadata.ImageID).
		Commit(ctx)
}

func GetImage(ctx context.Context, client *etcdapi.EtcdClient, imageID string) (*ImageMetadata, error) {
	if client == nil {
		return nil, errors.New("client is required")
	}
	value, err := client.Get(ctx, imageKey(imageID))
	if err != nil {
		if err == etcdapi.ErrorKeyNotFound {
			return nil, ErrImageNotFound
		}
		return nil, err
	}
	metadata := &ImageMetadata{}
	if err := json.Unmarshal([]byte(value), metadata); err != nil {
		return nil, err
	}
	return metadata, nil
}

func GetImageIDByName(ctx context.Context, client *etcdapi.EtcdClient, poolName, imageName string) (string, error) {
	if client == nil {
		return "", errors.New("client is required")
	}
	value, err := client.Get(ctx, imageNameKey(poolName, imageName))
	if err != nil {
		if err == etcdapi.ErrorKeyNotFound {
			return "", ErrImageNotFound
		}
		return "", err
	}
	return strings.TrimSpace(value), nil
}

func ListImages(ctx context.Context, client *etcdapi.EtcdClient) ([]*ImageMetadata, error) {
	if client == nil {
		return nil, errors.New("client is required")
	}
	kvs, err := client.GetWithPrefix(ctx, config.ConfigImageMetaKeyPrefix)
	if err != nil {
		return nil, err
	}
	images := make([]*ImageMetadata, 0, len(kvs))
	for _, kv := range kvs {
		item := &ImageMetadata{}
		if err := json.Unmarshal([]byte(kv.Value), item); err != nil {
			return nil, err
		}
		images = append(images, item)
	}
	sort.Slice(images, func(i, j int) bool {
		return images[i].ImageID < images[j].ImageID
	})
	return images, nil
}

func DeleteImage(ctx context.Context, client *etcdapi.EtcdClient, imageID string) error {
	if client == nil {
		return errors.New("client is required")
	}
	metadata, err := GetImage(ctx, client, imageID)
	if err != nil {
		return err
	}
	return client.NewTxn().
		Delete(imageKey(imageID)).
		Delete(imageNameKey(metadata.PoolName, metadata.ImageName)).
		Commit(ctx)
}

func PutSnapshot(ctx context.Context, client *etcdapi.EtcdClient, metadata *SnapshotMetadata) error {
	if client == nil || metadata == nil {
		return errors.New("client and snapshot metadata are required")
	}
	if err := metadata.normalizeAndValidate(); err != nil {
		return err
	}
	data, err := json.Marshal(metadata)
	if err != nil {
		return err
	}
	return client.NewTxn().
		Put(snapshotKey(metadata.SourceImageID, metadata.SnapshotID), string(data)).
		Put(snapshotNameKey(metadata.SourceImageID, metadata.SnapshotName), metadata.SnapshotID).
		Commit(ctx)
}

func GetSnapshot(ctx context.Context, client *etcdapi.EtcdClient, imageID, snapshotID string) (*SnapshotMetadata, error) {
	if client == nil {
		return nil, errors.New("client is required")
	}
	value, err := client.Get(ctx, snapshotKey(imageID, snapshotID))
	if err != nil {
		if err == etcdapi.ErrorKeyNotFound {
			return nil, ErrSnapshotNotFound
		}
		return nil, err
	}
	metadata := &SnapshotMetadata{}
	if err := json.Unmarshal([]byte(value), metadata); err != nil {
		return nil, err
	}
	return metadata, nil
}

func GetSnapshotIDByName(ctx context.Context, client *etcdapi.EtcdClient, imageID, snapshotName string) (string, error) {
	if client == nil {
		return "", errors.New("client is required")
	}
	value, err := client.Get(ctx, snapshotNameKey(imageID, snapshotName))
	if err != nil {
		if err == etcdapi.ErrorKeyNotFound {
			return "", ErrSnapshotNotFound
		}
		return "", err
	}
	return strings.TrimSpace(value), nil
}

func ListSnapshots(ctx context.Context, client *etcdapi.EtcdClient, imageID string) ([]*SnapshotMetadata, error) {
	if client == nil {
		return nil, errors.New("client is required")
	}
	kvs, err := client.GetWithPrefix(ctx, snapshotPrefix(imageID))
	if err != nil {
		return nil, err
	}
	snapshots := make([]*SnapshotMetadata, 0, len(kvs))
	for _, kv := range kvs {
		item := &SnapshotMetadata{}
		if err := json.Unmarshal([]byte(kv.Value), item); err != nil {
			return nil, err
		}
		snapshots = append(snapshots, item)
	}
	sort.Slice(snapshots, func(i, j int) bool {
		return snapshots[i].SnapshotID < snapshots[j].SnapshotID
	})
	return snapshots, nil
}

func DeleteSnapshot(ctx context.Context, client *etcdapi.EtcdClient, imageID, snapshotID string) error {
	if client == nil {
		return errors.New("client is required")
	}
	metadata, err := GetSnapshot(ctx, client, imageID, snapshotID)
	if err != nil {
		return err
	}
	return client.NewTxn().
		Delete(snapshotKey(imageID, snapshotID)).
		Delete(snapshotNameKey(imageID, metadata.SnapshotName)).
		Commit(ctx)
}

func PutChildLink(ctx context.Context, client *etcdapi.EtcdClient, snapshotID, childImageID string) error {
	if client == nil {
		return errors.New("client is required")
	}
	snapshotID = strings.TrimSpace(snapshotID)
	childImageID = strings.TrimSpace(childImageID)
	if snapshotID == "" || childImageID == "" {
		return errors.New("snapshot id and child image id are required")
	}
	return client.Put(ctx, childLinkKey(snapshotID, childImageID), childImageID)
}

func DeleteChildLink(ctx context.Context, client *etcdapi.EtcdClient, snapshotID, childImageID string) error {
	if client == nil {
		return errors.New("client is required")
	}
	snapshotID = strings.TrimSpace(snapshotID)
	childImageID = strings.TrimSpace(childImageID)
	if snapshotID == "" || childImageID == "" {
		return errors.New("snapshot id and child image id are required")
	}
	return client.Delete(ctx, childLinkKey(snapshotID, childImageID))
}

func ListChildImageIDs(ctx context.Context, client *etcdapi.EtcdClient, snapshotID string) ([]string, error) {
	if client == nil {
		return nil, errors.New("client is required")
	}
	kvs, err := client.GetWithPrefix(ctx, childLinkPrefix(snapshotID))
	if err != nil {
		return nil, err
	}
	children := make([]string, 0, len(kvs))
	for _, kv := range kvs {
		value := strings.TrimSpace(kv.Value)
		if value != "" {
			children = append(children, value)
		}
	}
	sort.Strings(children)
	return children, nil
}

func PutOperation(ctx context.Context, client *etcdapi.EtcdClient, record *ImageOperationRecord) error {
	if client == nil || record == nil {
		return errors.New("client and operation record are required")
	}
	if err := record.normalizeAndValidate(); err != nil {
		return err
	}
	data, err := json.Marshal(record)
	if err != nil {
		return err
	}
	return client.Put(ctx, operationKey(record.OperationID), string(data))
}

func GetOperation(ctx context.Context, client *etcdapi.EtcdClient, operationID string) (*ImageOperationRecord, error) {
	if client == nil {
		return nil, errors.New("client is required")
	}
	value, err := client.Get(ctx, operationKey(operationID))
	if err != nil {
		if err == etcdapi.ErrorKeyNotFound {
			return nil, ErrOperationNotFound
		}
		return nil, err
	}
	record := &ImageOperationRecord{}
	if err := json.Unmarshal([]byte(value), record); err != nil {
		return nil, err
	}
	return record, nil
}

func ListOperations(ctx context.Context, client *etcdapi.EtcdClient) ([]*ImageOperationRecord, error) {
	if client == nil {
		return nil, errors.New("client is required")
	}
	kvs, err := client.GetWithPrefix(ctx, config.ConfigImageOperationsKeyPrefix)
	if err != nil {
		return nil, err
	}
	records := make([]*ImageOperationRecord, 0, len(kvs))
	for _, kv := range kvs {
		item := &ImageOperationRecord{}
		if err := json.Unmarshal([]byte(kv.Value), item); err != nil {
			return nil, err
		}
		records = append(records, item)
	}
	sort.Slice(records, func(i, j int) bool {
		return records[i].OperationID < records[j].OperationID
	})
	return records, nil
}

func DeleteOperation(ctx context.Context, client *etcdapi.EtcdClient, operationID string) error {
	if client == nil {
		return errors.New("client is required")
	}
	if strings.TrimSpace(operationID) == "" {
		return errors.New("operation id is required")
	}
	return client.Delete(ctx, operationKey(operationID))
}

func (m *ImageMetadata) normalizeAndValidate() error {
	m.ImageID = strings.TrimSpace(m.ImageID)
	m.PoolName = strings.TrimSpace(m.PoolName)
	m.ImageName = strings.TrimSpace(m.ImageName)
	if m.ImageID == "" || m.PoolName == "" || m.ImageName == "" {
		return errors.New("image id, pool name and image name are required")
	}
	if m.Size <= 0 || m.ObjectSize <= 0 {
		return errors.New("image size and object size must be positive")
	}
	if m.Status == "" {
		m.Status = ImageStatusReady
	}
	if !validImageStatus(m.Status) {
		return errors.New("invalid image status")
	}
	now := time.Now().UTC()
	if m.CreatedAt.IsZero() {
		m.CreatedAt = now
	}
	if m.UpdatedAt.IsZero() {
		m.UpdatedAt = m.CreatedAt
	}
	return nil
}

func (m *SnapshotMetadata) normalizeAndValidate() error {
	m.SnapshotID = strings.TrimSpace(m.SnapshotID)
	m.SnapshotName = strings.TrimSpace(m.SnapshotName)
	m.SourceImageID = strings.TrimSpace(m.SourceImageID)
	m.SourcePoolName = strings.TrimSpace(m.SourcePoolName)
	m.SourceImageName = strings.TrimSpace(m.SourceImageName)
	if m.SnapshotID == "" || m.SnapshotName == "" || m.SourceImageID == "" {
		return errors.New("snapshot id, snapshot name and source image id are required")
	}
	if m.SourcePoolName == "" || m.SourceImageName == "" {
		return errors.New("source pool name and source image name are required")
	}
	if m.Status == "" {
		m.Status = SnapshotStatusReady
	}
	if !validSnapshotStatus(m.Status) {
		return errors.New("invalid snapshot status")
	}
	now := time.Now().UTC()
	if m.CreatedAt.IsZero() {
		m.CreatedAt = now
	}
	if m.UpdatedAt.IsZero() {
		m.UpdatedAt = m.CreatedAt
	}
	return nil
}

func (r *ImageOperationRecord) normalizeAndValidate() error {
	r.OperationID = strings.TrimSpace(r.OperationID)
	r.TargetID = strings.TrimSpace(r.TargetID)
	if r.OperationID == "" || r.TargetID == "" {
		return errors.New("operation id and target id are required")
	}
	if !validOperationType(r.Type) {
		return errors.New("invalid operation type")
	}
	if r.Status == "" {
		r.Status = OperationStatusPending
	}
	if !validOperationStatus(r.Status) {
		return errors.New("invalid operation status")
	}
	now := time.Now().UTC()
	if r.StartedAt.IsZero() {
		r.StartedAt = now
	}
	if r.UpdatedAt.IsZero() {
		r.UpdatedAt = r.StartedAt
	}
	return nil
}

func validImageStatus(status ImageStatus) bool {
	switch status {
	case ImageStatusReady, ImageStatusCreating, ImageStatusFlattening, ImageStatusDeleting:
		return true
	default:
		return false
	}
}

func validSnapshotStatus(status SnapshotStatus) bool {
	switch status {
	case SnapshotStatusCreating, SnapshotStatusReady, SnapshotStatusDeleting, SnapshotStatusDeletedPendingGC:
		return true
	default:
		return false
	}
}

func validOperationType(kind OperationType) bool {
	switch kind {
	case OperationCreateSnapshot, OperationDeleteSnapshot, OperationProtectSnapshot,
		OperationUnprotectSnapshot, OperationCloneImage, OperationFlattenImage:
		return true
	default:
		return false
	}
}

func validOperationStatus(status OperationStatus) bool {
	switch status {
	case OperationStatusPending, OperationStatusRunning, OperationStatusCommitting,
		OperationStatusDone, OperationStatusFailed:
		return true
	default:
		return false
	}
}

func imageKey(imageID string) string {
	return config.ConfigImageMetaKeyPrefix + encodeKeyPart(imageID)
}

func imageNameKey(poolName, imageName string) string {
	return config.ConfigImageNameKeyPrefix + encodeKeyPart(poolName) + "/" + encodeKeyPart(imageName)
}

func snapshotPrefix(imageID string) string {
	return config.ConfigImageSnapshotsKeyPrefix + encodeKeyPart(imageID) + "/"
}

func snapshotKey(imageID, snapshotID string) string {
	return snapshotPrefix(imageID) + encodeKeyPart(snapshotID)
}

func snapshotNameKey(imageID, snapshotName string) string {
	return config.ConfigImageSnapNameKeyPrefix + encodeKeyPart(imageID) + "/" + encodeKeyPart(snapshotName)
}

func childLinkPrefix(snapshotID string) string {
	return config.ConfigImageChildrenKeyPrefix + encodeKeyPart(snapshotID) + "/"
}

func childLinkKey(snapshotID, childImageID string) string {
	return childLinkPrefix(snapshotID) + encodeKeyPart(childImageID)
}

func operationKey(operationID string) string {
	return config.ConfigImageOperationsKeyPrefix + encodeKeyPart(operationID)
}

func encodeKeyPart(value string) string {
	return base64.RawURLEncoding.EncodeToString([]byte(strings.TrimSpace(value)))
}
