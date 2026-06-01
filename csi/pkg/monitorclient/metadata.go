package monitorclient

import (
	"context"
	"errors"
	"fmt"
	"strings"
)

var ErrMetadataNotFound = errors.New("monitor metadata not found")
var ErrLeaseNotFound = errors.New("monitor lease not found")
var ErrLeaseConflict = errors.New("monitor lease conflict")
var ErrImageNotFound = errors.New("monitor image not found")

type VolumeMetadata struct {
	Volume    Volume
	BlockSize int64
	Transport string
	ExportID  string
}

type Attachment struct {
	VolumeID string
	NodeID   string
	HostNQN  string
	ExportID string
}

type Lease struct {
	VolumeID   string
	NodeID     string
	HostNQN    string
	LeaseID    int64
	TTLSeconds int64
}

type MetadataClient interface {
	PutVolumeMetadata(ctx context.Context, metadata VolumeMetadata) error
	GetVolumeMetadata(ctx context.Context, volumeID string) (VolumeMetadata, error)
	DeleteVolumeMetadata(ctx context.Context, volumeID string) error
	ListVolumeMetadata(ctx context.Context) ([]VolumeMetadata, error)
	PutAttachment(ctx context.Context, attachment Attachment) error
	GetAttachment(ctx context.Context, volumeID string) (Attachment, error)
	DeleteAttachment(ctx context.Context, volumeID string) error
	ListAttachments(ctx context.Context) ([]Attachment, error)
	AcquireLease(ctx context.Context, lease Lease) (Lease, error)
	GetLease(ctx context.Context, volumeID string) (Lease, error)
	RenewLease(ctx context.Context, lease Lease) (Lease, error)
	ReleaseLease(ctx context.Context, lease Lease) error
	ListLeases(ctx context.Context) ([]Lease, error)
	AttachImage(ctx context.Context, ref VolumeRef, clientID, clientType string, leaseDurationSeconds int64) error
	DetachImage(ctx context.Context, ref VolumeRef, clientID string) error
	RenewImageLease(ctx context.Context, ref VolumeRef, clientID string, leaseDurationSeconds int64) error
}

func (m VolumeMetadata) Validate() error {
	if err := m.Volume.Validate(); err != nil {
		return err
	}
	if m.Volume.CapacityBytes <= 0 {
		return fmt.Errorf("invalid volume capacity %d", m.Volume.CapacityBytes)
	}
	if m.Volume.ObjectSize <= 0 {
		return fmt.Errorf("invalid volume object size %d", m.Volume.ObjectSize)
	}
	if m.BlockSize <= 0 {
		return fmt.Errorf("invalid block size %d", m.BlockSize)
	}
	if m.Transport != "rdma" && m.Transport != "tcp" {
		return fmt.Errorf("unsupported transport %q", m.Transport)
	}
	return nil
}

func (a Attachment) Validate() error {
	if strings.TrimSpace(a.VolumeID) == "" {
		return errors.New("volume id is required")
	}
	if strings.TrimSpace(a.NodeID) == "" {
		return errors.New("node id is required")
	}
	if strings.TrimSpace(a.HostNQN) == "" {
		return errors.New("host nqn is required")
	}
	if strings.TrimSpace(a.ExportID) == "" {
		return errors.New("export id is required")
	}
	return nil
}

func (l Lease) Validate() error {
	if strings.TrimSpace(l.VolumeID) == "" {
		return errors.New("volume id is required")
	}
	if strings.TrimSpace(l.NodeID) == "" {
		return errors.New("node id is required")
	}
	if strings.TrimSpace(l.HostNQN) == "" {
		return errors.New("host nqn is required")
	}
	if l.TTLSeconds <= 0 {
		return fmt.Errorf("invalid lease ttl %d", l.TTLSeconds)
	}
	return nil
}
