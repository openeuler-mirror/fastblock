package controller

import (
	"context"
	"errors"
	"strings"

	"fastblock-csi/pkg/monitorclient"
)

type CreateSnapshotRequest struct {
	Name         string
	SourceVolume monitorclient.VolumeRef
}

type DeleteSnapshotRequest struct {
	SnapshotID string
}

type ListSnapshotsRequest struct {
	SnapshotID     string
	SourceVolumeID string
}

func (s *Service) CreateSnapshot(ctx context.Context, req CreateSnapshotRequest) (monitorclient.Snapshot, error) {
	if s.snapshotMonitor == nil {
		return monitorclient.Snapshot{}, ErrSnapshotNotSupported
	}
	if err := req.Validate(); err != nil {
		return monitorclient.Snapshot{}, err
	}
	snapshot, err := s.snapshotMonitor.CreateSnapshot(ctx, monitorclient.CreateSnapshotRequest{
		Name:         req.Name,
		SourceVolume: req.SourceVolume,
	})
	if err != nil {
		return monitorclient.Snapshot{}, err
	}
	return snapshot, snapshot.Validate()
}

func (s *Service) DeleteSnapshot(ctx context.Context, req DeleteSnapshotRequest) error {
	if s.snapshotMonitor == nil {
		return ErrSnapshotNotSupported
	}
	if err := req.Validate(); err != nil {
		return err
	}
	err := s.snapshotMonitor.DeleteSnapshot(ctx, req.SnapshotID)
	if errors.Is(err, monitorclient.ErrSnapshotNotFound) {
		return nil
	}
	return err
}

func (s *Service) ListSnapshots(ctx context.Context, req ListSnapshotsRequest) ([]monitorclient.Snapshot, error) {
	if s.snapshotMonitor == nil {
		return nil, ErrSnapshotNotSupported
	}
	if err := req.Validate(); err != nil {
		return nil, err
	}
	return s.snapshotMonitor.ListSnapshots(ctx, monitorclient.ListSnapshotsRequest{
		SnapshotID:     strings.TrimSpace(req.SnapshotID),
		SourceVolumeID: strings.TrimSpace(req.SourceVolumeID),
	})
}

func (r CreateSnapshotRequest) Validate() error {
	if strings.TrimSpace(r.Name) == "" {
		return errors.New("snapshot name is required")
	}
	if strings.TrimSpace(r.SourceVolume.ID) == "" {
		return errors.New("source volume id is required")
	}
	return r.SourceVolume.Validate()
}

func (r DeleteSnapshotRequest) Validate() error {
	if strings.TrimSpace(r.SnapshotID) == "" {
		return errors.New("snapshot id is required")
	}
	return nil
}

func (r ListSnapshotsRequest) Validate() error {
	return nil
}
