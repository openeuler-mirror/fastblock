package controller

import (
	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/exporterclient"
	"fastblock-csi/pkg/monitorclient"
)

type Service struct {
	opts           driver.Options
	monitor        monitorclient.Client
	exporter       exporterclient.Client
	attachments    attachmentStore
	defaultHostNQN string
}

func New(opts driver.Options, monitor monitorclient.Client, exporter exporterclient.Client) *Service {
	return &Service{
		opts:        opts,
		monitor:     monitor,
		exporter:    exporter,
		attachments: newMemoryAttachmentStore(),
	}
}

func NewWithDefaultHostNQN(opts driver.Options, monitor monitorclient.Client, exporter exporterclient.Client, defaultHostNQN string) *Service {
	svc := New(opts, monitor, exporter)
	svc.defaultHostNQN = defaultHostNQN
	return svc
}

func (s *Service) DriverName() string {
	return s.opts.DriverName
}

func (s *Service) Endpoint() string {
	return s.opts.Endpoint
}

func (s *Service) DefaultHostNQN() string {
	return s.defaultHostNQN
}
