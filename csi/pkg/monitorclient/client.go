package monitorclient

import "context"

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
