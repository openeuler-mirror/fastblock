package controller

import (
	"context"
	"fmt"
	"strconv"
	"strings"

	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/exporterclient"
	"fastblock-csi/pkg/monitorclient"
	"fastblock-csi/pkg/volumeid"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
)

type GRPCService struct {
	csi.UnimplementedControllerServer

	service *Service
}

type publishVolumeContext struct {
	ref           monitorclient.VolumeRef
	transport     string
	blockSize     int64
	objectSize    int64
	capacityBytes int64
}

func NewGRPCService(service *Service) *GRPCService {
	return &GRPCService{service: service}
}

func (s *GRPCService) ControllerGetCapabilities(context.Context, *csi.ControllerGetCapabilitiesRequest) (*csi.ControllerGetCapabilitiesResponse, error) {
	return &csi.ControllerGetCapabilitiesResponse{
		Capabilities: driver.ControllerServiceCapabilities(),
	}, nil
}

func (s *GRPCService) ValidateVolumeCapabilities(_ context.Context, req *csi.ValidateVolumeCapabilitiesRequest) (*csi.ValidateVolumeCapabilitiesResponse, error) {
	if !driver.AreSupportedVolumeCapabilities(req.GetVolumeCapabilities()) {
		return &csi.ValidateVolumeCapabilitiesResponse{}, nil
	}
	return &csi.ValidateVolumeCapabilitiesResponse{
		Confirmed: &csi.ValidateVolumeCapabilitiesResponse_Confirmed{
			VolumeCapabilities: req.GetVolumeCapabilities(),
			Parameters:         req.GetParameters(),
			VolumeContext:      req.GetVolumeContext(),
		},
	}, nil
}

func (s *GRPCService) CreateVolume(ctx context.Context, req *csi.CreateVolumeRequest) (*csi.CreateVolumeResponse, error) {
	if req.GetName() == "" {
		return nil, fmt.Errorf("volume name is required")
	}
	if req.GetParameters()["pool"] == "" {
		return nil, fmt.Errorf("pool parameter is required")
	}
	required := req.GetCapacityRange().GetRequiredBytes()
	if required <= 0 {
		return nil, fmt.Errorf("required bytes must be greater than zero")
	}
	objectSize, err := strconv.ParseInt(req.GetParameters()["objectSize"], 10, 64)
	if err != nil {
		return nil, fmt.Errorf("invalid objectSize: %w", err)
	}
	blockSize, err := strconv.ParseInt(req.GetParameters()["blockSize"], 10, 64)
	if err != nil {
		return nil, fmt.Errorf("invalid blockSize: %w", err)
	}
	transport := req.GetParameters()["transport"]
	volume, err := s.service.CreateVolume(ctx, CreateVolumeRequest{
		Name:          req.GetName(),
		Pool:          req.GetParameters()["pool"],
		CapacityBytes: required,
		ObjectSize:    objectSize,
		BlockSize:     blockSize,
		Transport:     transport,
	})
	if err != nil {
		return nil, err
	}
	return &csi.CreateVolumeResponse{
		Volume: &csi.Volume{
			VolumeId:      volume.ID,
			CapacityBytes: volume.CapacityBytes,
			VolumeContext: map[string]string{
				"pool":          volume.Pool,
				"name":          volume.Name,
				"transport":     transport,
				"blockSize":     req.GetParameters()["blockSize"],
				"objectSize":    req.GetParameters()["objectSize"],
				"capacityBytes": strconv.FormatInt(volume.CapacityBytes, 10),
			},
		},
	}, nil
}

func (s *GRPCService) DeleteVolume(ctx context.Context, req *csi.DeleteVolumeRequest) (*csi.DeleteVolumeResponse, error) {
	if req.GetVolumeId() == "" {
		return nil, fmt.Errorf("volume id is required")
	}
	nameRef, err := volumeid.DecodeNameRef(req.GetVolumeId())
	if err != nil {
		return nil, err
	}
	if err := s.service.DeleteVolume(ctx, NewDeleteVolumeRequest(monitorclient.VolumeRef{
		ID:   req.GetVolumeId(),
		Pool: nameRef.Pool,
		Name: nameRef.Name,
	})); err != nil {
		return nil, err
	}
	return &csi.DeleteVolumeResponse{}, nil
}

func (s *GRPCService) ControllerPublishVolume(ctx context.Context, req *csi.ControllerPublishVolumeRequest) (*csi.ControllerPublishVolumeResponse, error) {
	if req.GetVolumeId() == "" {
		return nil, fmt.Errorf("volume id is required")
	}
	if req.GetNodeId() == "" {
		return nil, fmt.Errorf("node id is required")
	}
	if !driver.IsSupportedVolumeCapability(req.GetVolumeCapability()) {
		return nil, fmt.Errorf("unsupported volume capability")
	}
	volumeCtx, err := parsePublishVolumeContext(req.GetVolumeId(), req.GetVolumeContext())
	if err != nil {
		return nil, err
	}
	volume, err := s.service.GetVolume(ctx, NewGetVolumeRequest(volumeCtx.ref))
	if err != nil {
		return nil, err
	}
	volume.ID = req.GetVolumeId()
	if volume.CapacityBytes == 0 {
		volume.CapacityBytes = volumeCtx.capacityBytes
	}
	if volume.ObjectSize == 0 {
		volume.ObjectSize = volumeCtx.objectSize
	}
	result, err := s.service.ControllerPublishVolume(ctx, ControllerPublishRequest{
		Volume:    volume,
		BlockSize: volumeCtx.blockSize,
		Transport: volumeCtx.transport,
		NodeID:    req.GetNodeId(),
		Secrets:   req.GetSecrets(),
	})
	if err != nil {
		return nil, err
	}
	return &csi.ControllerPublishVolumeResponse{
		PublishContext: result.PublishContext,
	}, nil
}

func (s *GRPCService) ControllerUnpublishVolume(ctx context.Context, req *csi.ControllerUnpublishVolumeRequest) (*csi.ControllerUnpublishVolumeResponse, error) {
	if req.GetVolumeId() == "" {
		return nil, fmt.Errorf("volume id is required")
	}
	exportID, err := exporterclient.ExportIDForVolume(req.GetVolumeId())
	if err != nil {
		return nil, err
	}
	if err := s.service.ControllerUnpublishVolume(ctx, ControllerUnpublishRequest{
		ExportID: exportID,
		NodeID:   req.GetNodeId(),
		Secrets:  req.GetSecrets(),
	}); err != nil {
		return nil, err
	}
	return &csi.ControllerUnpublishVolumeResponse{}, nil
}

func parsePublishVolumeContext(volumeID string, ctx map[string]string) (publishVolumeContext, error) {
	nameRef, err := volumeid.DecodeNameRef(volumeID)
	if err != nil {
		return publishVolumeContext{}, err
	}
	pool := strings.TrimSpace(ctx["pool"])
	if pool == "" {
		return publishVolumeContext{}, fmt.Errorf("volume_context.pool is required")
	}
	name := strings.TrimSpace(ctx["name"])
	if name == "" {
		return publishVolumeContext{}, fmt.Errorf("volume_context.name is required")
	}
	if pool != nameRef.Pool || name != nameRef.Name {
		return publishVolumeContext{}, fmt.Errorf("volume_context pool/name mismatch with volume id")
	}
	transport := strings.TrimSpace(ctx["transport"])
	if transport != "rdma" && transport != "tcp" {
		return publishVolumeContext{}, fmt.Errorf("invalid volume_context.transport %q", transport)
	}
	blockSize, err := strconv.ParseInt(ctx["blockSize"], 10, 64)
	if err != nil || blockSize <= 0 {
		return publishVolumeContext{}, fmt.Errorf("invalid volume_context.blockSize")
	}
	objectSize, err := strconv.ParseInt(ctx["objectSize"], 10, 64)
	if err != nil || objectSize <= 0 {
		return publishVolumeContext{}, fmt.Errorf("invalid volume_context.objectSize")
	}
	capacityBytes, err := strconv.ParseInt(ctx["capacityBytes"], 10, 64)
	if err != nil || capacityBytes <= 0 {
		return publishVolumeContext{}, fmt.Errorf("invalid volume_context.capacityBytes")
	}
	return publishVolumeContext{
		ref: monitorclient.VolumeRef{
			ID:   volumeID,
			Pool: pool,
			Name: name,
		},
		transport:     transport,
		blockSize:     blockSize,
		objectSize:    objectSize,
		capacityBytes: capacityBytes,
	}, nil
}
