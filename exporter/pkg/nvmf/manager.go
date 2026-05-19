package nvmf

import (
	"context"
	"errors"

	"fastblock-exporter/pkg/api"
)

type Manager interface {
	CreateExport(ctx context.Context, req api.CreateExportRequest) (api.Export, error)
	DeleteExport(ctx context.Context, exportID string) error
	AllowHost(ctx context.Context, exportID, hostNQN string) error
	DenyHost(ctx context.Context, exportID, hostNQN string) error
}

type LocalManager struct {
	RPCSocketPath string
}

func NewLocalManager(rpcSocketPath string) *LocalManager {
	return &LocalManager{RPCSocketPath: rpcSocketPath}
}

func (m *LocalManager) CreateExport(context.Context, api.CreateExportRequest) (api.Export, error) {
	return api.Export{}, errors.New("create export not implemented")
}

func (m *LocalManager) DeleteExport(context.Context, string) error {
	return errors.New("delete export not implemented")
}

func (m *LocalManager) AllowHost(context.Context, string, string) error {
	return errors.New("allow host not implemented")
}

func (m *LocalManager) DenyHost(context.Context, string, string) error {
	return errors.New("deny host not implemented")
}
