package controller

import (
	"context"
	"errors"
	"fmt"
	"strings"

	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/exporterclient"
	"fastblock-csi/pkg/monitorclient"
)

type CreateVolumeRequest struct {
	Name          string
	Pool          string
	CapacityBytes int64
	ObjectSize    int64
	BlockSize     int64
	Transport     string
}

type PublishVolumeRequest struct {
	Volume    monitorclient.Volume
	BlockSize int64
	Transport string
	HostNQN   string
}

type PublishVolumeResult struct {
	Export         exporterclient.Export
	PublishContext map[string]string
}

func normalizeVolume(volume monitorclient.Volume, ref monitorclient.VolumeRef) monitorclient.Volume {
	if strings.TrimSpace(volume.ID) == "" {
		volume.ID = ref.ID
	}
	if strings.TrimSpace(volume.Name) == "" {
		volume.Name = ref.Name
	}
	if strings.TrimSpace(volume.Pool) == "" {
		volume.Pool = ref.Pool
	}
	return volume
}

func attachmentConflicts(existing Attachment, nodeID, hostNQN string) bool {
	existingNodeID := strings.TrimSpace(existing.NodeID)
	currentNodeID := strings.TrimSpace(nodeID)
	if existingNodeID != "" && currentNodeID != "" {
		return existingNodeID != currentNodeID
	}
	existingHostNQN := strings.TrimSpace(existing.HostNQN)
	currentHostNQN := strings.TrimSpace(hostNQN)
	return existingHostNQN != "" && currentHostNQN != "" && existingHostNQN != currentHostNQN
}

func ResolveHostNQN(nodeID, defaultHostNQN string, secrets map[string]string) string {
	if secrets != nil {
		if hostNQN := strings.TrimSpace(secrets["hostNQN"]); hostNQN != "" {
			return hostNQN
		}
	}
	if hostNQN := strings.TrimSpace(defaultHostNQN); hostNQN != "" {
		return hostNQN
	}
	return strings.TrimSpace(nodeID)
}

type DeleteVolumeRequest struct {
	Volume monitorclient.VolumeRef
}

type GetVolumeRequest struct {
	Volume monitorclient.VolumeRef
}

type ExpandVolumeRequest struct {
	Volume        monitorclient.VolumeRef
	CapacityBytes int64
}

type UnpublishVolumeRequest struct {
	ExportID string
	HostNQN  string
}

type ControllerPublishRequest struct {
	Volume    monitorclient.Volume
	BlockSize int64
	Transport string
	NodeID    string
	Secrets   map[string]string
}

type ControllerUnpublishRequest struct {
	VolumeID string
	ExportID string
	NodeID   string
	Secrets  map[string]string
}

func NewDeleteVolumeRequest(ref monitorclient.VolumeRef) DeleteVolumeRequest {
	return DeleteVolumeRequest{Volume: ref}
}

func NewGetVolumeRequest(ref monitorclient.VolumeRef) GetVolumeRequest {
	return GetVolumeRequest{Volume: ref}
}

func NewExpandVolumeRequest(ref monitorclient.VolumeRef, capacityBytes int64) ExpandVolumeRequest {
	return ExpandVolumeRequest{
		Volume:        ref,
		CapacityBytes: capacityBytes,
	}
}

func NewPublishVolumeRequest(volume monitorclient.Volume, blockSize int64, transport, hostNQN string) PublishVolumeRequest {
	return PublishVolumeRequest{
		Volume:    volume,
		BlockSize: blockSize,
		Transport: transport,
		HostNQN:   hostNQN,
	}
}

func NewUnpublishVolumeRequest(exportID, hostNQN string) UnpublishVolumeRequest {
	return UnpublishVolumeRequest{
		ExportID: exportID,
		HostNQN:  hostNQN,
	}
}

func (s *Service) recordVolumeMetadata(ctx context.Context, volume monitorclient.Volume, blockSize int64, transport, exportID string) error {
	if strings.TrimSpace(volume.ID) == "" || blockSize <= 0 || (transport != "rdma" && transport != "tcp") {
		return nil
	}
	existing, ok, err := s.volumes.Get(ctx, volume.ID)
	if err != nil {
		return err
	}
	if ok && strings.TrimSpace(exportID) == "" {
		exportID = existing.ExportID
	}
	return s.volumes.Put(ctx, VolumeMetadata{
		Volume:    volume,
		BlockSize: blockSize,
		Transport: transport,
		ExportID:  exportID,
	})
}

func (s *Service) CreateVolume(ctx context.Context, req CreateVolumeRequest) (monitorclient.Volume, error) {
	if err := req.Validate(); err != nil {
		return monitorclient.Volume{}, err
	}
	volume, err := s.monitor.CreateVolume(ctx, monitorclient.CreateVolumeRequest{
		Name:          req.Name,
		Pool:          req.Pool,
		CapacityBytes: req.CapacityBytes,
		ObjectSize:    req.ObjectSize,
		BlockSize:     req.BlockSize,
	})
	if err != nil {
		return monitorclient.Volume{}, err
	}
	if err := s.recordVolumeMetadata(ctx, volume, req.BlockSize, req.Transport, ""); err != nil {
		return monitorclient.Volume{}, err
	}
	return volume, nil
}

func (s *Service) DeleteVolume(ctx context.Context, req DeleteVolumeRequest) error {
	if err := req.Validate(); err != nil {
		return err
	}
	if err := s.reconcileState(ctx, req.Volume.ID); err != nil {
		return err
	}
	if existing, ok, err := s.attachments.Get(ctx, req.Volume.ID); err != nil {
		return err
	} else if ok {
		return fmt.Errorf("%w: volume %s remains attached to node %s", ErrVolumeStillPublished, existing.VolumeID, existing.NodeID)
	}
	if lease, ok, err := s.currentLease(ctx, req.Volume.ID); err != nil {
		return err
	} else if ok {
		return fmt.Errorf("%w: volume %s lease remains held by node %s", ErrVolumeStillPublished, lease.VolumeID, lease.NodeID)
	}
	if strings.TrimSpace(req.Volume.ID) != "" {
		exportID := ""
		if metadata, ok, err := s.volumes.Get(ctx, req.Volume.ID); err != nil {
			return err
		} else if ok {
			exportID = strings.TrimSpace(metadata.ExportID)
		}
		if exportID == "" {
			var err error
			exportID, err = exporterclient.ExportIDForVolume(req.Volume.ID)
			if err != nil {
				return err
			}
		}
		if err := s.exporter.DeleteExport(ctx, exportID); err != nil {
			return err
		}
	}
	if err := s.monitor.DeleteVolume(ctx, req.Volume); err != nil {
		return err
	}
	return s.volumes.Delete(ctx, req.Volume.ID)
}

func (s *Service) GetVolume(ctx context.Context, req GetVolumeRequest) (monitorclient.Volume, error) {
	if err := req.Validate(); err != nil {
		return monitorclient.Volume{}, err
	}
	volume, err := s.monitor.GetVolume(ctx, req.Volume)
	if err != nil {
		return monitorclient.Volume{}, err
	}
	volume = normalizeVolume(volume, req.Volume)
	if existing, ok, err := s.volumes.Get(ctx, req.Volume.ID); err != nil {
		return monitorclient.Volume{}, err
	} else if ok {
		if err := s.recordVolumeMetadata(ctx, volume, existing.BlockSize, existing.Transport, existing.ExportID); err != nil {
			return monitorclient.Volume{}, err
		}
	}
	return volume, nil
}

func (s *Service) ExpandVolume(ctx context.Context, req ExpandVolumeRequest) (monitorclient.Volume, error) {
	if err := req.Validate(); err != nil {
		return monitorclient.Volume{}, err
	}
	volume, err := s.monitor.ExpandVolume(ctx, req.Volume, req.CapacityBytes)
	if err != nil {
		return monitorclient.Volume{}, err
	}
	volume = normalizeVolume(volume, req.Volume)
	if existing, ok, err := s.volumes.Get(ctx, req.Volume.ID); err != nil {
		return monitorclient.Volume{}, err
	} else if ok {
		if err := s.recordVolumeMetadata(ctx, volume, existing.BlockSize, existing.Transport, existing.ExportID); err != nil {
			return monitorclient.Volume{}, err
		}
	}
	return volume, nil
}

func (s *Service) PublishVolume(ctx context.Context, req PublishVolumeRequest) (PublishVolumeResult, error) {
	if err := req.Validate(); err != nil {
		return PublishVolumeResult{}, err
	}
	export, err := s.exporter.CreateExport(ctx, exporterclient.CreateExportRequest{
		VolumeID:      req.Volume.ID,
		PoolName:      req.Volume.Pool,
		ImageName:     req.Volume.Name,
		CapacityBytes: req.Volume.CapacityBytes,
		ObjectSize:    req.Volume.ObjectSize,
		BlockSize:     req.BlockSize,
		Transport:     req.Transport,
	})
	if err != nil {
		return PublishVolumeResult{}, err
	}
	if req.HostNQN != "" {
		if err := s.exporter.AllowHost(ctx, export.ID, req.HostNQN); err != nil {
			return PublishVolumeResult{}, err
		}
	}
	publishContext, err := driver.BuildPublishContext(export, req.Transport)
	if err != nil {
		return PublishVolumeResult{}, err
	}
	return PublishVolumeResult{
		Export:         export,
		PublishContext: publishContext,
	}, nil
}

func (s *Service) ControllerPublishVolume(ctx context.Context, req ControllerPublishRequest) (PublishVolumeResult, error) {
	if err := req.Validate(); err != nil {
		return PublishVolumeResult{}, err
	}
	if err := s.reconcileState(ctx, req.Volume.ID); err != nil {
		return PublishVolumeResult{}, err
	}
	if err := s.recordVolumeMetadata(ctx, req.Volume, req.BlockSize, req.Transport, ""); err != nil {
		return PublishVolumeResult{}, err
	}
	hostNQN := ResolveHostNQN(req.NodeID, s.defaultHostNQN, req.Secrets)
	if existing, ok, err := s.attachments.Get(ctx, req.Volume.ID); err != nil {
		return PublishVolumeResult{}, err
	} else if ok {
		if attachmentConflicts(existing, req.NodeID, hostNQN) {
			return PublishVolumeResult{}, fmt.Errorf(
				"%w: volume %s is attached to node %s with host NQN %s",
				ErrVolumePublishedToAnotherNode,
				req.Volume.ID,
				existing.NodeID,
				existing.HostNQN,
			)
		}
	}
	if err := s.acquireLease(ctx, req.Volume.ID, req.NodeID, hostNQN); err != nil {
		return PublishVolumeResult{}, err
	}
	result, err := s.PublishVolume(ctx, NewPublishVolumeRequest(
		req.Volume,
		req.BlockSize,
		req.Transport,
		hostNQN,
	))
	if err != nil {
		_ = s.releaseLease(ctx, req.Volume.ID, req.NodeID, hostNQN)
		return PublishVolumeResult{}, err
	}
	if err := s.recordVolumeMetadata(ctx, req.Volume, req.BlockSize, req.Transport, result.Export.ID); err != nil {
		_ = s.UnpublishVolume(ctx, NewUnpublishVolumeRequest(result.Export.ID, hostNQN))
		_ = s.releaseLease(ctx, req.Volume.ID, req.NodeID, hostNQN)
		return PublishVolumeResult{}, err
	}
	if err := s.attachments.Put(ctx, Attachment{
		VolumeID: req.Volume.ID,
		NodeID:   req.NodeID,
		HostNQN:  hostNQN,
		ExportID: result.Export.ID,
	}); err != nil {
		_ = s.UnpublishVolume(ctx, NewUnpublishVolumeRequest(result.Export.ID, hostNQN))
		_ = s.releaseLease(ctx, req.Volume.ID, req.NodeID, hostNQN)
		return PublishVolumeResult{}, err
	}
	s.startLeaseRenewer(req.Volume.ID, req.NodeID, hostNQN)
	return result, nil
}

func (s *Service) UnpublishVolume(ctx context.Context, req UnpublishVolumeRequest) error {
	if err := req.Validate(); err != nil {
		return err
	}
	if req.HostNQN != "" {
		if err := s.exporter.DenyHost(ctx, req.ExportID, req.HostNQN); err != nil {
			return err
		}
	}
	return s.exporter.DeleteExport(ctx, req.ExportID)
}

func (s *Service) ControllerUnpublishVolume(ctx context.Context, req ControllerUnpublishRequest) error {
	if err := req.Validate(); err != nil {
		return err
	}
	nodeID := strings.TrimSpace(req.NodeID)
	hostNQN := ResolveHostNQN(nodeID, s.defaultHostNQN, req.Secrets)
	exportID := strings.TrimSpace(req.ExportID)
	if existing, ok, err := s.attachments.Get(ctx, req.VolumeID); err != nil {
		return err
	} else if ok {
		if attachmentConflicts(existing, nodeID, hostNQN) {
			return fmt.Errorf(
				"%w: volume %s is attached to node %s with host NQN %s",
				ErrAttachmentNodeMismatch,
				req.VolumeID,
				existing.NodeID,
				existing.HostNQN,
			)
		}
		if strings.TrimSpace(existing.ExportID) != "" {
			exportID = existing.ExportID
		}
		if nodeID == "" && strings.TrimSpace(existing.NodeID) != "" {
			nodeID = existing.NodeID
		}
		if strings.TrimSpace(existing.HostNQN) != "" {
			hostNQN = existing.HostNQN
		}
	} else if metadata, ok, err := s.volumes.Get(ctx, req.VolumeID); err != nil {
		return err
	} else if ok && strings.TrimSpace(metadata.ExportID) != "" {
		exportID = metadata.ExportID
	}
	if (nodeID == "" || hostNQN == "") && exportID != "" {
		if lease, ok, err := s.currentLease(ctx, req.VolumeID); err != nil {
			return err
		} else if ok {
			if nodeID == "" {
				nodeID = lease.NodeID
			}
			if hostNQN == "" {
				hostNQN = lease.HostNQN
			}
		}
	}
	if exportID == "" {
		derivedExportID, err := exporterclient.ExportIDForVolume(req.VolumeID)
		if err != nil {
			return err
		}
		exportID = derivedExportID
	}
	if err := s.UnpublishVolume(ctx, NewUnpublishVolumeRequest(exportID, hostNQN)); err != nil {
		return err
	}
	s.leaseRenewer.Stop(req.VolumeID)
	if err := s.attachments.Delete(ctx, req.VolumeID); err != nil {
		return err
	}
	if nodeID == "" || hostNQN == "" {
		return nil
	}
	return s.releaseLease(ctx, req.VolumeID, nodeID, hostNQN)
}

func (r CreateVolumeRequest) Validate() error {
	if strings.TrimSpace(r.Name) == "" {
		return errors.New("name is required")
	}
	if strings.TrimSpace(r.Pool) == "" {
		return errors.New("pool is required")
	}
	if r.CapacityBytes <= 0 {
		return fmt.Errorf("invalid capacity bytes %d", r.CapacityBytes)
	}
	if r.ObjectSize <= 0 {
		return fmt.Errorf("invalid object size %d", r.ObjectSize)
	}
	if r.BlockSize <= 0 {
		return fmt.Errorf("invalid block size %d", r.BlockSize)
	}
	if r.Transport != "rdma" && r.Transport != "tcp" {
		return fmt.Errorf("unsupported transport %q", r.Transport)
	}
	return nil
}

func (r PublishVolumeRequest) Validate() error {
	if strings.TrimSpace(r.Volume.ID) == "" {
		return errors.New("volume id is required")
	}
	if strings.TrimSpace(r.Volume.Name) == "" {
		return errors.New("volume name is required")
	}
	if strings.TrimSpace(r.Volume.Pool) == "" {
		return errors.New("volume pool is required")
	}
	if r.Volume.CapacityBytes <= 0 {
		return fmt.Errorf("invalid volume capacity %d", r.Volume.CapacityBytes)
	}
	if r.Volume.ObjectSize <= 0 {
		return fmt.Errorf("invalid volume object size %d", r.Volume.ObjectSize)
	}
	if r.BlockSize <= 0 {
		return fmt.Errorf("invalid block size %d", r.BlockSize)
	}
	if r.Transport != "rdma" && r.Transport != "tcp" {
		return fmt.Errorf("unsupported transport %q", r.Transport)
	}
	return nil
}

func (r DeleteVolumeRequest) Validate() error {
	return r.Volume.Validate()
}

func (r GetVolumeRequest) Validate() error {
	return r.Volume.Validate()
}

func (r ExpandVolumeRequest) Validate() error {
	if err := r.Volume.Validate(); err != nil {
		return err
	}
	if r.CapacityBytes <= 0 {
		return fmt.Errorf("invalid capacity bytes %d", r.CapacityBytes)
	}
	return nil
}

func (r UnpublishVolumeRequest) Validate() error {
	if strings.TrimSpace(r.ExportID) == "" {
		return errors.New("export id is required")
	}
	return nil
}

func (r ControllerPublishRequest) Validate() error {
	if strings.TrimSpace(r.NodeID) == "" && ResolveHostNQN(r.NodeID, "", r.Secrets) == "" {
		return errors.New("node id or hostNQN is required")
	}
	return PublishVolumeRequest{
		Volume:    r.Volume,
		BlockSize: r.BlockSize,
		Transport: r.Transport,
		HostNQN:   ResolveHostNQN(r.NodeID, "", r.Secrets),
	}.Validate()
}

func (r ControllerUnpublishRequest) Validate() error {
	if strings.TrimSpace(r.VolumeID) == "" {
		return errors.New("volume id is required")
	}
	return nil
}
