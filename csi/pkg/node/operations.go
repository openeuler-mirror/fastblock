package node

import (
	"context"
	"errors"
	"strings"

	"fastblock-csi/pkg/backend"
	"fastblock-csi/pkg/driver"
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

func (s *Service) StageVolumeFromPublishContext(ctx context.Context, volumeID string, publishContext map[string]string) (string, error) {
	volumeCtx, err := driver.ParsePublishContext(publishContext)
	if err != nil {
		return "", err
	}
	return s.StageVolume(ctx, StageVolumeRequest{VolumeID: volumeID, VolumeContext: volumeCtx})
}

func (s *Service) UnstageVolumeFromPublishContext(ctx context.Context, volumeID string, publishContext map[string]string) error {
	volumeCtx, err := driver.ParsePublishContext(publishContext)
	if err != nil {
		return err
	}
	return s.UnstageVolume(ctx, StageVolumeRequest{VolumeID: volumeID, VolumeContext: volumeCtx})
}

func (s *Service) GetDeviceFromPublishContext(ctx context.Context, volumeID string, publishContext map[string]string) (string, error) {
	volumeCtx, err := driver.ParsePublishContext(publishContext)
	if err != nil {
		return "", err
	}
	return s.GetDevice(ctx, StageVolumeRequest{VolumeID: volumeID, VolumeContext: volumeCtx})
}

func (s *Service) IsReadyFromPublishContext(ctx context.Context, volumeID string, publishContext map[string]string) (bool, error) {
	volumeCtx, err := driver.ParsePublishContext(publishContext)
	if err != nil {
		return false, err
	}
	return s.IsReady(ctx, StageVolumeRequest{VolumeID: volumeID, VolumeContext: volumeCtx})
}

func (r StageVolumeRequest) Validate() error {
	if strings.TrimSpace(r.VolumeID) == "" {
		return errors.New("volume id is required")
	}
	return backend.ValidateVolumeContext(r.VolumeContext)
}
