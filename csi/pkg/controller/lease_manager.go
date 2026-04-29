package controller

import (
	"context"
	"errors"
	"fmt"
	"sync"
	"time"

	"fastblock-csi/pkg/monitorclient"
)

const defaultLeaseTTLSeconds int64 = 30

type leaseRenewer struct {
	mu      sync.Mutex
	cancels map[string]context.CancelFunc
}

func newLeaseRenewer() *leaseRenewer {
	return &leaseRenewer{
		cancels: make(map[string]context.CancelFunc),
	}
}

func (r *leaseRenewer) Start(volumeID string, cancel context.CancelFunc) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if existing, ok := r.cancels[volumeID]; ok {
		existing()
	}
	r.cancels[volumeID] = cancel
}

func (r *leaseRenewer) Stop(volumeID string) {
	r.mu.Lock()
	defer r.mu.Unlock()
	if cancel, ok := r.cancels[volumeID]; ok {
		cancel()
		delete(r.cancels, volumeID)
	}
}

func (s *Service) metadataClient() (monitorclient.MetadataClient, bool) {
	client, ok := s.monitor.(monitorclient.MetadataClient)
	return client, ok
}

func (s *Service) currentLease(ctx context.Context, volumeID string) (monitorclient.Lease, bool, error) {
	client, ok := s.metadataClient()
	if !ok {
		return monitorclient.Lease{}, false, nil
	}
	lease, err := client.GetLease(ctx, volumeID)
	if err != nil {
		if errors.Is(err, monitorclient.ErrLeaseNotFound) {
			return monitorclient.Lease{}, false, nil
		}
		if errors.Is(err, monitorclient.ErrNotImplemented) {
			return monitorclient.Lease{}, false, nil
		}
		return monitorclient.Lease{}, false, err
	}
	return lease, true, nil
}

func (s *Service) acquireLease(ctx context.Context, volumeID, nodeID, hostNQN string) error {
	client, ok := s.metadataClient()
	if !ok {
		return nil
	}
	_, err := client.AcquireLease(ctx, monitorclient.Lease{
		VolumeID:   volumeID,
		NodeID:     nodeID,
		HostNQN:    hostNQN,
		TTLSeconds: s.leaseTTLSeconds,
	})
	if errors.Is(err, monitorclient.ErrLeaseConflict) {
		return fmt.Errorf("%w: volume %s lease is held by another node", ErrVolumePublishedToAnotherNode, volumeID)
	}
	if errors.Is(err, monitorclient.ErrNotImplemented) {
		return nil
	}
	return err
}

func (s *Service) releaseLease(ctx context.Context, volumeID, nodeID, hostNQN string) error {
	client, ok := s.metadataClient()
	if !ok {
		return nil
	}
	err := client.ReleaseLease(ctx, monitorclient.Lease{
		VolumeID:   volumeID,
		NodeID:     nodeID,
		HostNQN:    hostNQN,
		TTLSeconds: s.leaseTTLSeconds,
	})
	if errors.Is(err, monitorclient.ErrLeaseNotFound) || errors.Is(err, monitorclient.ErrNotImplemented) {
		return nil
	}
	if errors.Is(err, monitorclient.ErrLeaseConflict) {
		return fmt.Errorf("%w: volume %s lease belongs to another node", ErrAttachmentNodeMismatch, volumeID)
	}
	return err
}

func (s *Service) startLeaseRenewer(volumeID, nodeID, hostNQN string) {
	client, ok := s.metadataClient()
	if !ok {
		return
	}
	interval := time.Duration(s.leaseTTLSeconds/3) * time.Second
	if interval <= 0 {
		interval = 10 * time.Second
	}
	ctx, cancel := context.WithCancel(context.Background())
	s.leaseRenewer.Start(volumeID, cancel)
	go func() {
		ticker := time.NewTicker(interval)
		defer ticker.Stop()
		for {
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
				_, err := client.RenewLease(context.Background(), monitorclient.Lease{
					VolumeID:   volumeID,
					NodeID:     nodeID,
					HostNQN:    hostNQN,
					TTLSeconds: s.leaseTTLSeconds,
				})
				if errors.Is(err, monitorclient.ErrLeaseNotFound) || errors.Is(err, monitorclient.ErrLeaseConflict) {
					return
				}
			}
		}
	}()
}
