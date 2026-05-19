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
}

type Client interface {
	CreateVolume(ctx context.Context, req CreateVolumeRequest) (Volume, error)
	DeleteVolume(ctx context.Context, volumeID string) error
	GetVolume(ctx context.Context, volumeID string) (Volume, error)
	ExpandVolume(ctx context.Context, volumeID string, capacityBytes int64) (Volume, error)
}

type NoopClient struct{}

func NewNoop() *NoopClient {
	return &NoopClient{}
}

func (c *NoopClient) CreateVolume(context.Context, CreateVolumeRequest) (Volume, error) {
	return Volume{}, ErrNotImplemented
}

func (c *NoopClient) DeleteVolume(context.Context, string) error {
	return ErrNotImplemented
}

func (c *NoopClient) GetVolume(context.Context, string) (Volume, error) {
	return Volume{}, ErrNotImplemented
}

func (c *NoopClient) ExpandVolume(context.Context, string, int64) (Volume, error) {
	return Volume{}, ErrNotImplemented
}
