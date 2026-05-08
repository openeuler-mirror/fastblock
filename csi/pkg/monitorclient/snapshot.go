package monitorclient

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"time"
)

var ErrSnapshotNotFound = errors.New("monitor snapshot not found")

type Snapshot struct {
	ID           string
	Name         string
	SourceVolume VolumeRef
	CreationTime time.Time
	SizeBytes    int64
	ReadyToUse   bool
}

type CreateSnapshotRequest struct {
	Name         string
	SourceVolume VolumeRef
}

type ListSnapshotsRequest struct {
	SnapshotID     string
	SourceVolumeID string
}

type CreateVolumeFromSnapshotRequest struct {
	Name          string
	Pool          string
	CapacityBytes int64
	ObjectSize    int64
	BlockSize     int64
	SnapshotID    string
}

type SnapshotClient interface {
	CreateSnapshot(ctx context.Context, req CreateSnapshotRequest) (Snapshot, error)
	DeleteSnapshot(ctx context.Context, snapshotID string) error
	GetSnapshot(ctx context.Context, snapshotID string) (Snapshot, error)
	ListSnapshots(ctx context.Context, req ListSnapshotsRequest) ([]Snapshot, error)
	CreateVolumeFromSnapshot(ctx context.Context, req CreateVolumeFromSnapshotRequest) (Volume, error)
}

func (s Snapshot) Validate() error {
	if strings.TrimSpace(s.ID) == "" {
		return errors.New("snapshot id is required")
	}
	if strings.TrimSpace(s.Name) == "" {
		return errors.New("snapshot name is required")
	}
	if strings.TrimSpace(s.SourceVolume.ID) == "" {
		return errors.New("source volume id is required")
	}
	if err := s.SourceVolume.Validate(); err != nil {
		return fmt.Errorf("invalid source volume: %w", err)
	}
	if s.SizeBytes < 0 {
		return fmt.Errorf("invalid snapshot size %d", s.SizeBytes)
	}
	return nil
}

func (r CreateSnapshotRequest) Validate() error {
	if strings.TrimSpace(r.Name) == "" {
		return errors.New("snapshot name is required")
	}
	if strings.TrimSpace(r.SourceVolume.ID) == "" {
		return errors.New("source volume id is required")
	}
	if err := r.SourceVolume.Validate(); err != nil {
		return fmt.Errorf("invalid source volume: %w", err)
	}
	return nil
}

func (r CreateVolumeFromSnapshotRequest) Validate() error {
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
	if strings.TrimSpace(r.SnapshotID) == "" {
		return errors.New("snapshot id is required")
	}
	return nil
}
