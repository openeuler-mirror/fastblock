package node

import (
	"fastblock-csi/pkg/backend"
	"fastblock-csi/pkg/driver"
)

type Service struct {
	opts    driver.Options
	backend backend.Interface
}

func New(opts driver.Options, backend backend.Interface) *Service {
	return &Service{
		opts:    opts,
		backend: backend,
	}
}

func (s *Service) DriverName() string {
	return s.opts.DriverName
}

func (s *Service) NodeID() string {
	return s.opts.NodeID
}
