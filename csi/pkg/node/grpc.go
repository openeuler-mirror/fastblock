package node

import (
	"context"
	"fmt"

	"fastblock-csi/pkg/backend"
	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/mount"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
)

type GRPCService struct {
	csi.UnimplementedNodeServer

	service *Service
}

func NewGRPCService(service *Service) *GRPCService {
	return &GRPCService{service: service}
}

func (s *GRPCService) NodeGetCapabilities(context.Context, *csi.NodeGetCapabilitiesRequest) (*csi.NodeGetCapabilitiesResponse, error) {
	return &csi.NodeGetCapabilitiesResponse{
		Capabilities: driver.NodeServiceCapabilities(),
	}, nil
}

func (s *GRPCService) NodeGetInfo(context.Context, *csi.NodeGetInfoRequest) (*csi.NodeGetInfoResponse, error) {
	return &csi.NodeGetInfoResponse{
		NodeId: s.service.NodeID(),
	}, nil
}

func (s *GRPCService) NodeStageVolume(ctx context.Context, req *csi.NodeStageVolumeRequest) (*csi.NodeStageVolumeResponse, error) {
	if req.GetVolumeId() == "" {
		return nil, fmt.Errorf("volume id is required")
	}
	if req.GetStagingTargetPath() == "" {
		return nil, fmt.Errorf("staging target path is required")
	}
	if !driver.IsSupportedVolumeCapability(req.GetVolumeCapability()) {
		return nil, fmt.Errorf("unsupported volume capability")
	}
	devicePath, err := s.service.StageVolumeFromPublishContext(ctx, PublishContextStageRequest{
		VolumeID:       req.GetVolumeId(),
		PublishContext: req.GetPublishContext(),
	})
	if err != nil {
		return nil, err
	}
	volumeCtx, err := driver.ParsePublishContext(req.GetPublishContext())
	if err != nil {
		return nil, err
	}
	if err := mount.WriteStageState(req.GetStagingTargetPath(), mount.StageState{
		VolumeID:   req.GetVolumeId(),
		DevicePath: devicePath,
		Transport:  volumeCtx.Transport,
		NQN:        volumeCtx.NQN,
		Traddr:     volumeCtx.Traddr,
		Trsvcid:    volumeCtx.Trsvcid,
		NSID:       volumeCtx.NSID,
	}); err != nil {
		return nil, err
	}
	if err := mount.WriteStageDeviceLink(req.GetStagingTargetPath(), devicePath); err != nil {
		return nil, err
	}
	return &csi.NodeStageVolumeResponse{}, nil
}

func (s *GRPCService) NodeUnstageVolume(ctx context.Context, req *csi.NodeUnstageVolumeRequest) (*csi.NodeUnstageVolumeResponse, error) {
	if req.GetVolumeId() == "" {
		return nil, fmt.Errorf("volume id is required")
	}
	if req.GetStagingTargetPath() == "" {
		return nil, fmt.Errorf("staging target path is required")
	}
	state, err := mount.ReadStageState(req.GetStagingTargetPath())
	if err != nil {
		return nil, err
	}
	if state.VolumeID != req.GetVolumeId() {
		return nil, fmt.Errorf("staged volume id mismatch: got %s want %s", state.VolumeID, req.GetVolumeId())
	}
	if err := s.service.UnstageVolume(ctx, StageVolumeRequest{
		VolumeID: req.GetVolumeId(),
		VolumeContext: backend.VolumeContext{
			Transport: state.Transport,
			NQN:       state.NQN,
			Traddr:    state.Traddr,
			Trsvcid:   state.Trsvcid,
			NSID:      state.NSID,
		},
	}); err != nil {
		return nil, err
	}
	if err := mount.RemoveStageDeviceLink(req.GetStagingTargetPath()); err != nil {
		return nil, err
	}
	if err := mount.RemoveStageState(req.GetStagingTargetPath()); err != nil {
		return nil, err
	}
	return &csi.NodeUnstageVolumeResponse{}, nil
}

func (s *GRPCService) NodePublishVolume(ctx context.Context, req *csi.NodePublishVolumeRequest) (*csi.NodePublishVolumeResponse, error) {
	if req.GetVolumeId() == "" {
		return nil, fmt.Errorf("volume id is required")
	}
	if req.GetStagingTargetPath() == "" {
		return nil, fmt.Errorf("staging target path is required")
	}
	if req.GetTargetPath() == "" {
		return nil, fmt.Errorf("target path is required")
	}
	if !driver.IsSupportedVolumeCapability(req.GetVolumeCapability()) {
		return nil, fmt.Errorf("unsupported volume capability")
	}
	if err := s.service.PublishVolume(ctx, PublishVolumeRequest{
		VolumeID:          req.GetVolumeId(),
		StagingTargetPath: req.GetStagingTargetPath(),
		TargetPath:        req.GetTargetPath(),
	}); err != nil {
		return nil, err
	}
	return &csi.NodePublishVolumeResponse{}, nil
}

func (s *GRPCService) NodeUnpublishVolume(ctx context.Context, req *csi.NodeUnpublishVolumeRequest) (*csi.NodeUnpublishVolumeResponse, error) {
	if req.GetVolumeId() == "" {
		return nil, fmt.Errorf("volume id is required")
	}
	if req.GetTargetPath() == "" {
		return nil, fmt.Errorf("target path is required")
	}
	if err := s.service.UnpublishVolume(ctx, UnpublishVolumeRequest{
		VolumeID:   req.GetVolumeId(),
		TargetPath: req.GetTargetPath(),
	}); err != nil {
		return nil, err
	}
	return &csi.NodeUnpublishVolumeResponse{}, nil
}
