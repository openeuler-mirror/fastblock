package api

import (
	"errors"
	"fmt"
	"strings"
)

type CreateExportRequest struct {
	VolumeID      string `json:"volume_id"`
	PoolName      string `json:"pool_name"`
	ImageName     string `json:"image_name"`
	CapacityBytes int64  `json:"capacity_bytes"`
	ObjectSize    int64  `json:"object_size"`
	BlockSize     int64  `json:"block_size"`
	Transport     string `json:"transport"`
	AllowAnyHost  bool   `json:"allow_any_host"`
}

type Export struct {
	ID      string `json:"id"`
	NQN     string `json:"nqn"`
	NSID    int    `json:"nsid"`
	Traddr  string `json:"traddr"`
	Trsvcid string `json:"trsvcid"`
}

type Snapshot struct {
	SnapshotID        string `json:"snapshot_id"`
	SnapshotName      string `json:"snapshot_name"`
	SourceImageID     string `json:"source_image_id"`
	SourcePoolID      int32  `json:"source_pool_id"`
	SourcePoolName    string `json:"source_pool_name"`
	SourceImageName   string `json:"source_image_name"`
	SnapSeq           uint64 `json:"snap_seq"`
	Status            string `json:"status"`
	Protected         bool   `json:"protected"`
	OperationID       string `json:"operation_id"`
	ChildCount        uint32 `json:"child_count"`
	CreatedAtUnixNano int64  `json:"created_at_unix_nano"`
	UpdatedAtUnixNano int64  `json:"updated_at_unix_nano"`
}

type HostAccessRequest struct {
	HostNQN string `json:"host_nqn"`
}

type SnapshotRequest struct {
	SnapshotName string `json:"snapshot_name"`
}

type CloneFromSnapshotRequest struct {
	SnapshotName   string `json:"snapshot_name"`
	CloneImageName string `json:"clone_image_name"`
}

func (r CreateExportRequest) Validate() error {
	if strings.TrimSpace(r.VolumeID) == "" {
		return errors.New("volume_id is required")
	}
	if strings.TrimSpace(r.PoolName) == "" {
		return errors.New("pool_name is required")
	}
	if strings.TrimSpace(r.ImageName) == "" {
		return errors.New("image_name is required")
	}
	if r.BlockSize <= 0 {
		return fmt.Errorf("invalid block_size %d", r.BlockSize)
	}
	if r.Transport != "rdma" && r.Transport != "tcp" {
		return fmt.Errorf("unsupported transport %q", r.Transport)
	}
	return nil
}

func (r HostAccessRequest) Validate() error {
	if strings.TrimSpace(r.HostNQN) == "" {
		return errors.New("host_nqn is required")
	}
	return nil
}

func (r SnapshotRequest) Validate() error {
	if strings.TrimSpace(r.SnapshotName) == "" {
		return errors.New("snapshot_name is required")
	}
	return nil
}

func (r CloneFromSnapshotRequest) Validate() error {
	if strings.TrimSpace(r.SnapshotName) == "" {
		return errors.New("snapshot_name is required")
	}
	if strings.TrimSpace(r.CloneImageName) == "" {
		return errors.New("clone_image_name is required")
	}
	return nil
}
