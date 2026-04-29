package monitorclient

import (
	"context"
	"errors"
	"fmt"
	"strings"
)

var ErrMetadataNotFound = errors.New("monitor metadata not found")

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

type MetadataClient interface {
	PutVolumeMetadata(ctx context.Context, metadata VolumeMetadata) error
	GetVolumeMetadata(ctx context.Context, volumeID string) (VolumeMetadata, error)
	DeleteVolumeMetadata(ctx context.Context, volumeID string) error
	PutAttachment(ctx context.Context, attachment Attachment) error
	GetAttachment(ctx context.Context, volumeID string) (Attachment, error)
	DeleteAttachment(ctx context.Context, volumeID string) error
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
