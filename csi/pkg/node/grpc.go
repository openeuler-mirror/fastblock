package node

import (
	"context"

	"fastblock-csi/pkg/driver"

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
