package controller

import (
	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/exporterclient"
	"fastblock-csi/pkg/monitorclient"
)

type Service struct {
	opts            driver.Options
	monitor         monitorclient.Client
	snapshotMonitor monitorclient.SnapshotClient
	exporter        exporterclient.Client
	volumes         volumeStore
	attachments     attachmentStore
	leaseTTLSeconds int64
	leaseRenewer    *leaseRenewer
	defaultHostNQN  string
}

func New(opts driver.Options, monitor monitorclient.Client, exporter exporterclient.Client) *Service {
	svc := &Service{
		opts:            opts,
		monitor:         monitor,
		exporter:        exporter,
		volumes:         newMemoryVolumeStore(),
		attachments:     newMemoryAttachmentStore(),
		leaseTTLSeconds: defaultLeaseTTLSeconds,
		leaseRenewer:    newLeaseRenewer(),
	}
	if metadataClient, ok := monitor.(monitorclient.MetadataClient); ok {
		svc.volumes = newMonitorVolumeStore(metadataClient)
		svc.attachments = newMonitorAttachmentStore(metadataClient)
	}
	if snapshotClient, ok := monitor.(monitorclient.SnapshotClient); ok {
		svc.snapshotMonitor = snapshotClient
	}
	return svc
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

func (s *Service) SupportsSnapshots() bool {
	return s.snapshotMonitor != nil
}
