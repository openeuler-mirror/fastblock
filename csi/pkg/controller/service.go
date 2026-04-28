package controller

import (
	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/exporterclient"
	"fastblock-csi/pkg/monitorclient"
)

type Service struct {
	opts     driver.Options
	monitor  monitorclient.Client
	exporter exporterclient.Client
}

func New(opts driver.Options, monitor monitorclient.Client, exporter exporterclient.Client) *Service {
	return &Service{
		opts:     opts,
		monitor:  monitor,
		exporter: exporter,
	}
}

func (s *Service) DriverName() string {
	return s.opts.DriverName
}

func (s *Service) Endpoint() string {
	return s.opts.Endpoint
}
