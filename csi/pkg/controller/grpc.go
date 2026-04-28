package controller

import (
	"context"
	"fmt"
	"strconv"

	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/monitorclient"
	"fastblock-csi/pkg/volumeid"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
)

type GRPCService struct {
	csi.UnimplementedControllerServer

	service *Service
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
				"pool":      volume.Pool,
				"name":      volume.Name,
				"transport": transport,
			},
		},
	}, nil
}

func (s *GRPCService) DeleteVolume(ctx context.Context, req *csi.DeleteVolumeRequest) (*csi.DeleteVolumeResponse, error) {
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
