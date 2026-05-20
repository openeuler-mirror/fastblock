package controller

import (
	"context"

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

func (s *Service) CreateVolume(ctx context.Context, req CreateVolumeRequest) (monitorclient.Volume, error) {
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

func (s *Service) PublishVolume(ctx context.Context, req PublishVolumeRequest) (exporterclient.Export, error) {
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
		return exporterclient.Export{}, err
	}
	if req.HostNQN != "" {
		if err := s.exporter.AllowHost(ctx, export.ID, req.HostNQN); err != nil {
			return exporterclient.Export{}, err
		}
	}
	return export, nil
}

func (s *Service) UnpublishVolume(ctx context.Context, exportID, hostNQN string) error {
	if hostNQN != "" {
		if err := s.exporter.DenyHost(ctx, exportID, hostNQN); err != nil {
			return err
		}
	}
	return s.exporter.DeleteExport(ctx, exportID)
}
