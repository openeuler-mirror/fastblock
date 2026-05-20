package node

import (
	"context"
	"errors"
	"strings"

	"fastblock-csi/pkg/backend"
)

type StageVolumeRequest struct {
	VolumeID      string
	VolumeContext backend.VolumeContext
}

func (s *Service) StageVolume(ctx context.Context, req StageVolumeRequest) (string, error) {
	if err := req.Validate(); err != nil {
		return "", err
	}
	return s.backend.Stage(ctx, req.VolumeID, req.VolumeContext)
}

func (s *Service) UnstageVolume(ctx context.Context, req StageVolumeRequest) error {
	if err := req.Validate(); err != nil {
		return err
	}
	return s.backend.Unstage(ctx, req.VolumeID, req.VolumeContext)
}

func (s *Service) GetDevice(ctx context.Context, req StageVolumeRequest) (string, error) {
	if err := req.Validate(); err != nil {
		return "", err
	}
	return s.backend.GetDevice(ctx, req.VolumeID, req.VolumeContext)
}

func (s *Service) IsReady(ctx context.Context, req StageVolumeRequest) (bool, error) {
	if err := req.Validate(); err != nil {
		return false, err
	}
	return s.backend.IsReady(ctx, req.VolumeID, req.VolumeContext)
}

func (r StageVolumeRequest) Validate() error {
	if strings.TrimSpace(r.VolumeID) == "" {
		return errors.New("volume id is required")
	}
	return backend.ValidateVolumeContext(r.VolumeContext)
}
