package exporterclient

import (
	"context"
	"errors"
	"fmt"
	"strings"
)

type CreateExportRequest struct {
	VolumeID      string
	PoolName      string
	ImageName     string
	CapacityBytes int64
	ObjectSize    int64
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

type Client interface {
	CreateExport(ctx context.Context, req CreateExportRequest) (Export, error)
	DeleteExport(ctx context.Context, exportID string) error
	AllowHost(ctx context.Context, exportID, hostNQN string) error
	DenyHost(ctx context.Context, exportID, hostNQN string) error
}

type NoopClient struct{}

func NewNoop() *NoopClient {
	return &NoopClient{}
}

func (c *NoopClient) CreateExport(context.Context, CreateExportRequest) (Export, error) {
	return Export{}, ErrNotImplemented
}

func (c *NoopClient) DeleteExport(context.Context, string) error {
	return ErrNotImplemented
}

func (c *NoopClient) AllowHost(context.Context, string, string) error {
	return ErrNotImplemented
}

func (c *NoopClient) DenyHost(context.Context, string, string) error {
	return ErrNotImplemented
}

func (r CreateExportRequest) Validate() error {
	if strings.TrimSpace(r.VolumeID) == "" {
		return errors.New("volume id is required")
	}
	if strings.TrimSpace(r.PoolName) == "" {
		return errors.New("pool name is required")
	}
	if strings.TrimSpace(r.ImageName) == "" {
		return errors.New("image name is required")
	}
	if r.CapacityBytes <= 0 {
		return fmt.Errorf("invalid capacity bytes %d", r.CapacityBytes)
	}
	if r.ObjectSize <= 0 {
		return fmt.Errorf("invalid object size %d", r.ObjectSize)
	}
	if r.BlockSize <= 0 {
		return fmt.Errorf("invalid block size %d", r.BlockSize)
	}
	if r.Transport != "rdma" && r.Transport != "tcp" {
		return fmt.Errorf("unsupported transport %q", r.Transport)
	}
	return nil
}

func (e Export) Validate() error {
	if strings.TrimSpace(e.ID) == "" {
		return errors.New("export id is required")
	}
	if strings.TrimSpace(e.NQN) == "" {
		return errors.New("export nqn is required")
	}
	if strings.TrimSpace(e.Traddr) == "" {
		return errors.New("export traddr is required")
	}
	if strings.TrimSpace(e.Trsvcid) == "" {
		return errors.New("export trsvcid is required")
	}
	if e.NSID <= 0 {
		return fmt.Errorf("invalid export nsid %d", e.NSID)
	}
	return nil
}
