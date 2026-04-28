package api

import (
	"errors"
	"fmt"
	"strings"
)

type CreateExportRequest struct {
	VolumeID      string
	PoolName      string
	ImageName     string
	CapacityBytes int64
	BlockSize     int64
	Transport     string
}

type Export struct {
	ID      string
	NQN     string
	NSID    int
	Traddr  string
	Trsvcid string
}

type HostAccessRequest struct {
	HostNQN string `json:"host_nqn"`
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
	if r.CapacityBytes <= 0 {
		return fmt.Errorf("invalid capacity_bytes %d", r.CapacityBytes)
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
