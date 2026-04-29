package controller

import (
	"context"
	"errors"

	"fastblock-csi/pkg/monitorclient"
)

type monitorVolumeStore struct {
	client monitorclient.MetadataClient
}

func newMonitorVolumeStore(client monitorclient.MetadataClient) *monitorVolumeStore {
	return &monitorVolumeStore{client: client}
}

func (s *monitorVolumeStore) Get(ctx context.Context, volumeID string) (VolumeMetadata, bool, error) {
	metadata, err := s.client.GetVolumeMetadata(ctx, volumeID)
	if err != nil {
		if errors.Is(err, monitorclient.ErrMetadataNotFound) {
			return VolumeMetadata{}, false, nil
		}
		return VolumeMetadata{}, false, err
	}
	return metadata, true, nil
}

func (s *monitorVolumeStore) Put(ctx context.Context, metadata VolumeMetadata) error {
	return s.client.PutVolumeMetadata(ctx, metadata)
}

func (s *monitorVolumeStore) Delete(ctx context.Context, volumeID string) error {
	return s.client.DeleteVolumeMetadata(ctx, volumeID)
}

type monitorAttachmentStore struct {
	client monitorclient.MetadataClient
}

func newMonitorAttachmentStore(client monitorclient.MetadataClient) *monitorAttachmentStore {
	return &monitorAttachmentStore{client: client}
}

func (s *monitorAttachmentStore) Get(ctx context.Context, volumeID string) (Attachment, bool, error) {
	attachment, err := s.client.GetAttachment(ctx, volumeID)
	if err != nil {
		if errors.Is(err, monitorclient.ErrMetadataNotFound) {
			return Attachment{}, false, nil
		}
		return Attachment{}, false, err
	}
	return attachment, true, nil
}

func (s *monitorAttachmentStore) Put(ctx context.Context, attachment Attachment) error {
	return s.client.PutAttachment(ctx, attachment)
}

func (s *monitorAttachmentStore) Delete(ctx context.Context, volumeID string) error {
	return s.client.DeleteAttachment(ctx, volumeID)
}
