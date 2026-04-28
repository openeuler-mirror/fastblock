package exporterclient

import "context"

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
