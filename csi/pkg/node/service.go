package node

import (
	"context"

	"fastblock-csi/pkg/backend"
	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/mount"
)

type Service struct {
	opts      driver.Options
	backend   backend.Interface
	publisher blockPublisher
}

func New(opts driver.Options, backend backend.Interface) *Service {
	return &Service{
		opts:      opts,
		backend:   backend,
		publisher: mount.NewBlockPublisher(),
	}
}

func (s *Service) DriverName() string {
	return s.opts.DriverName
}

func (s *Service) NodeID() string {
	return s.opts.NodeID
}

type blockPublisher interface {
	PublishBlockDevice(ctx context.Context, devicePath, stagePath, targetPath string) error
	UnpublishBlockDevice(ctx context.Context, targetPath string) error
}
