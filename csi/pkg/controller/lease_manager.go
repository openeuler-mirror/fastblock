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
const imageAttachmentClientType = "csi-controller"

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

func (s *Service) imageAttachmentClient() (monitorclient.MetadataClient, bool) {
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

func imageAttachmentClientID(nodeID, hostNQN string) string {
	if nodeID != "" {
		return nodeID
	}
	return hostNQN
}

func (s *Service) attachPublishedImage(ctx context.Context, ref monitorclient.VolumeRef, nodeID, hostNQN string) error {
	client, ok := s.imageAttachmentClient()
	if !ok {
		return nil
	}
	if err := ref.Validate(); err != nil {
		return nil
	}
	err := client.AttachImage(ctx, ref, imageAttachmentClientID(nodeID, hostNQN), imageAttachmentClientType, s.leaseTTLSeconds)
	if errors.Is(err, monitorclient.ErrNotImplemented) {
		return nil
	}
	return err
}

func (s *Service) detachPublishedImage(ctx context.Context, ref monitorclient.VolumeRef, nodeID, hostNQN string) error {
	client, ok := s.imageAttachmentClient()
	if !ok {
		return nil
	}
	if err := ref.Validate(); err != nil {
		return nil
	}
	err := client.DetachImage(ctx, ref, imageAttachmentClientID(nodeID, hostNQN))
	if errors.Is(err, monitorclient.ErrNotImplemented) || errors.Is(err, monitorclient.ErrImageNotFound) {
		return nil
	}
	return err
}

func (s *Service) renewPublishedImageAttachment(ctx context.Context, ref monitorclient.VolumeRef, nodeID, hostNQN string) error {
	client, ok := s.imageAttachmentClient()
	if !ok {
		return nil
	}
	if err := ref.Validate(); err != nil {
		return nil
	}
	err := client.RenewImageLease(ctx, ref, imageAttachmentClientID(nodeID, hostNQN), s.leaseTTLSeconds)
	switch {
	case err == nil:
		return nil
	case errors.Is(err, monitorclient.ErrNotImplemented):
		return nil
	case errors.Is(err, monitorclient.ErrImageNotFound):
		return client.AttachImage(ctx, ref, imageAttachmentClientID(nodeID, hostNQN), imageAttachmentClientType, s.leaseTTLSeconds)
	default:
		return err
	}
}

func (s *Service) startLeaseRenewer(volumeRef monitorclient.VolumeRef, nodeID, hostNQN string) {
	client, ok := s.metadataClient()
	if !ok {
		return
	}
	interval := time.Duration(s.leaseTTLSeconds/3) * time.Second
	if interval <= 0 {
		interval = 10 * time.Second
	}
	volumeID := volumeRef.ID
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
				if err := s.renewPublishedImageAttachment(context.Background(), volumeRef, nodeID, hostNQN); err != nil &&
					!errors.Is(err, monitorclient.ErrNotImplemented) {
					continue
				}
			}
		}
	}()
}
