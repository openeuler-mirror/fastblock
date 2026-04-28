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

func (s *Service) CreateVolume(ctx context.Context, req CreateVolumeRequest) (monitorclient.Volume, error) {
	if err := req.Validate(); err != nil {
		return monitorclient.Volume{}, err
	}
	return s.monitor.CreateVolume(ctx, monitorclient.CreateVolumeRequest{
		Name:          req.Name,
		Pool:          req.Pool,
		CapacityBytes: req.CapacityBytes,
		ObjectSize:    req.ObjectSize,
		BlockSize:     req.BlockSize,
	})
}

func (s *Service) DeleteVolume(ctx context.Context, ref monitorclient.VolumeRef) error {
	return s.monitor.DeleteVolume(ctx, ref)
}

func (s *Service) GetVolume(ctx context.Context, ref monitorclient.VolumeRef) (monitorclient.Volume, error) {
	return s.monitor.GetVolume(ctx, ref)
}

func (s *Service) ExpandVolume(ctx context.Context, ref monitorclient.VolumeRef, capacityBytes int64) (monitorclient.Volume, error) {
	return s.monitor.ExpandVolume(ctx, ref, capacityBytes)
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

func (s *Service) UnpublishVolume(ctx context.Context, exportID, hostNQN string) error {
	if hostNQN != "" {
		if err := s.exporter.DenyHost(ctx, exportID, hostNQN); err != nil {
			return err
		}
	}
	return s.exporter.DeleteExport(ctx, exportID)
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
