package monitorclient

import (
	"context"
	"errors"
	"fmt"
	"strings"
)

type CreateVolumeRequest struct {
	Name          string
	Pool          string
	CapacityBytes int64
	ObjectSize    int64
	BlockSize     int64
}

type Volume struct {
	ID            string
	Name          string
	Pool          string
	CapacityBytes int64
	ObjectSize    int64
}

type VolumeRef struct {
	ID   string
	Name string
	Pool string
}

type Client interface {
	CreateVolume(ctx context.Context, req CreateVolumeRequest) (Volume, error)
	DeleteVolume(ctx context.Context, ref VolumeRef) error
	GetVolume(ctx context.Context, ref VolumeRef) (Volume, error)
	ExpandVolume(ctx context.Context, ref VolumeRef, capacityBytes int64) (Volume, error)
}

type NoopClient struct{}

func NewNoop() *NoopClient {
	return &NoopClient{}
}

func (c *NoopClient) CreateVolume(context.Context, CreateVolumeRequest) (Volume, error) {
	return Volume{}, ErrNotImplemented
}

func (c *NoopClient) DeleteVolume(context.Context, VolumeRef) error {
	return ErrNotImplemented
}

func (c *NoopClient) GetVolume(context.Context, VolumeRef) (Volume, error) {
	return Volume{}, ErrNotImplemented
}

func (c *NoopClient) ExpandVolume(context.Context, VolumeRef, int64) (Volume, error) {
	return Volume{}, ErrNotImplemented
}

func (r CreateVolumeRequest) Validate() error {
	if strings.TrimSpace(r.Name) == "" {
		return errors.New("name is required")
	}
	if strings.TrimSpace(r.Pool) == "" {
		return errors.New("pool is required")
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
	return nil
}

func (r VolumeRef) Validate() error {
	if strings.TrimSpace(r.Name) == "" {
		return errors.New("name is required")
	}
	if strings.TrimSpace(r.Pool) == "" {
		return errors.New("pool is required")
	}
	return nil
}
