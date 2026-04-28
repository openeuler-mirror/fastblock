package node

import (
	"context"

	"fastblock-csi/pkg/backend"
)

type StageVolumeRequest struct {
	VolumeID      string
	VolumeContext backend.VolumeContext
}

func (s *Service) StageVolume(ctx context.Context, req StageVolumeRequest) (string, error) {
	return s.backend.Stage(ctx, req.VolumeID, req.VolumeContext)
}

func (s *Service) UnstageVolume(ctx context.Context, req StageVolumeRequest) error {
	return s.backend.Unstage(ctx, req.VolumeID, req.VolumeContext)
}

func (s *Service) GetDevice(ctx context.Context, req StageVolumeRequest) (string, error) {
	return s.backend.GetDevice(ctx, req.VolumeID, req.VolumeContext)
}

func (s *Service) IsReady(ctx context.Context, req StageVolumeRequest) (bool, error) {
	return s.backend.IsReady(ctx, req.VolumeID, req.VolumeContext)
}
