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

func (s *Service) DeleteVolume(ctx context.Context, req DeleteVolumeRequest) error {
	if err := req.Validate(); err != nil {
		return err
	}
	return s.monitor.DeleteVolume(ctx, req.Volume)
}

func (s *Service) GetVolume(ctx context.Context, req GetVolumeRequest) (monitorclient.Volume, error) {
	if err := req.Validate(); err != nil {
		return monitorclient.Volume{}, err
	}
	return s.monitor.GetVolume(ctx, req.Volume)
}

func (s *Service) ExpandVolume(ctx context.Context, req ExpandVolumeRequest) (monitorclient.Volume, error) {
	if err := req.Validate(); err != nil {
		return monitorclient.Volume{}, err
	}
	return s.monitor.ExpandVolume(ctx, req.Volume, req.CapacityBytes)
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
	return s.PublishVolume(ctx, NewPublishVolumeRequest(
		req.Volume,
		req.BlockSize,
		req.Transport,
		ResolveHostNQN(req.NodeID, s.defaultHostNQN, req.Secrets),
	))
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
	return s.UnpublishVolume(ctx, NewUnpublishVolumeRequest(
		req.ExportID,
		ResolveHostNQN(req.NodeID, s.defaultHostNQN, req.Secrets),
	))
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
	if strings.TrimSpace(r.ExportID) == "" {
		return errors.New("export id is required")
	}
	if strings.TrimSpace(r.NodeID) == "" && ResolveHostNQN(r.NodeID, "", r.Secrets) == "" {
		return errors.New("node id or hostNQN is required")
	}
	return nil
}
