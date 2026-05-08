package controller

import (
	"context"
	"errors"
	"fmt"
	"strings"

	"fastblock-csi/pkg/exporterclient"
	"fastblock-csi/pkg/monitorclient"
)

func (s *Service) reconcileState(ctx context.Context, volumeID string) error {
	metadata, metadataOK, err := s.volumes.Get(ctx, volumeID)
	if err != nil {
		return err
	}
	attachment, attachmentOK, err := s.attachments.Get(ctx, volumeID)
	if err != nil {
		return err
	}
	_, leaseOK, err := s.currentLease(ctx, volumeID)
	if err != nil {
		return err
	}

	exportID := ""
	if metadataOK {
		exportID = strings.TrimSpace(metadata.ExportID)
	}
	if exportID == "" && attachmentOK {
		exportID = strings.TrimSpace(attachment.ExportID)
	}

	exportExists := false
	if exportID != "" {
		_, err := s.exporter.GetExport(ctx, exportID)
		if err == nil {
			exportExists = true
		} else if !errors.Is(err, exporterclient.ErrNotFound) {
			return err
		}
	}

	if attachmentOK && !leaseOK && !exportExists {
		if err := s.attachments.Delete(ctx, volumeID); err != nil {
			return err
		}
		attachmentOK = false
	}
	if metadataOK && !exportExists && strings.TrimSpace(metadata.ExportID) != "" {
		metadata.ExportID = ""
		if err := s.volumes.Put(ctx, metadata); err != nil {
			return err
		}
	}
	if attachmentOK && metadataOK && strings.TrimSpace(metadata.ExportID) == "" && strings.TrimSpace(attachment.ExportID) != "" && exportExists {
		metadata.ExportID = attachment.ExportID
		if err := s.volumes.Put(ctx, metadata); err != nil {
			return err
		}
	}
	return nil
}

func (s *Service) Reconcile(ctx context.Context) error {
	client, ok := s.metadataClient()
	if !ok {
		return nil
	}
	volumes, err := client.ListVolumeMetadata(ctx)
	if err != nil {
		return err
	}
	attachments, err := client.ListAttachments(ctx)
	if err != nil {
		return err
	}
	leases, err := client.ListLeases(ctx)
	if err != nil {
		return err
	}

	ids := map[string]struct{}{}
	leaseByVolume := map[string]monitorclient.Lease{}
	for _, volume := range volumes {
		ids[volume.Volume.ID] = struct{}{}
	}
	for _, attachment := range attachments {
		ids[attachment.VolumeID] = struct{}{}
	}
	for _, lease := range leases {
		ids[lease.VolumeID] = struct{}{}
		leaseByVolume[lease.VolumeID] = lease
	}

	for volumeID := range ids {
		if err := s.reconcileState(ctx, volumeID); err != nil {
			return fmt.Errorf("reconcile volume %s: %w", volumeID, err)
		}
		attachment, attachmentOK, err := s.attachments.Get(ctx, volumeID)
		if err != nil {
			return err
		}
		lease, leaseOK, err := s.currentLease(ctx, volumeID)
		if err != nil {
			return err
		}

		exportID := ""
		volumeRef := volumeRefFromID(volumeID)
		if metadata, ok, err := s.volumes.Get(ctx, volumeID); err != nil {
			return err
		} else if ok {
			volumeRef = mergeVolumeRefs(volumeRef, metadata.Volume.Ref())
			exportID = strings.TrimSpace(metadata.ExportID)
		}
		if exportID == "" && attachmentOK {
			exportID = strings.TrimSpace(attachment.ExportID)
		}
		exportExists := false
		if exportID != "" {
			if _, err := s.exporter.GetExport(ctx, exportID); err == nil {
				exportExists = true
			} else if !errors.Is(err, exporterclient.ErrNotFound) {
				return err
			}
		}

		switch {
		case attachmentOK && exportExists && leaseOK:
			s.startLeaseRenewer(volumeRef, lease.NodeID, lease.HostNQN)
		case attachmentOK && exportExists && !leaseOK:
			if err := s.acquireLease(ctx, volumeID, attachment.NodeID, attachment.HostNQN); err != nil {
				return err
			}
			s.startLeaseRenewer(volumeRef, attachment.NodeID, attachment.HostNQN)
		case !attachmentOK && leaseOK && !exportExists:
			s.leaseRenewer.Stop(volumeID)
			if err := s.releaseLease(ctx, volumeID, lease.NodeID, lease.HostNQN); err != nil {
				return err
			}
		case !attachmentOK && exportExists:
			if err := s.exporter.DeleteExport(ctx, exportID); err != nil && !errors.Is(err, exporterclient.ErrNotFound) {
				return err
			}
			if lease, ok := leaseByVolume[volumeID]; ok {
				s.leaseRenewer.Stop(volumeID)
				if err := s.releaseLease(ctx, volumeID, lease.NodeID, lease.HostNQN); err != nil {
					return err
				}
			}
			if metadata, ok, err := s.volumes.Get(ctx, volumeID); err != nil {
				return err
			} else if ok && strings.TrimSpace(metadata.ExportID) != "" {
				metadata.ExportID = ""
				if err := s.volumes.Put(ctx, metadata); err != nil {
					return err
				}
			}
		}

	}
	return nil
}
