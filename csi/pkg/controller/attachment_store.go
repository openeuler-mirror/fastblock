package controller

import (
	"context"
	"strings"
	"sync"

	"fastblock-csi/pkg/monitorclient"
)

type Attachment = monitorclient.Attachment

type attachmentStore interface {
	Get(ctx context.Context, volumeID string) (Attachment, bool, error)
	Put(ctx context.Context, attachment Attachment) error
	Delete(ctx context.Context, volumeID string) error
}

type memoryAttachmentStore struct {
	mu          sync.RWMutex
	attachments map[string]Attachment
}

func newMemoryAttachmentStore() *memoryAttachmentStore {
	return &memoryAttachmentStore{
		attachments: make(map[string]Attachment),
	}
}

func (s *memoryAttachmentStore) Get(_ context.Context, volumeID string) (Attachment, bool, error) {
	return s.get(volumeID)
}

func (s *memoryAttachmentStore) get(volumeID string) (Attachment, bool, error) {
	key := strings.TrimSpace(volumeID)
	if key == "" {
		return Attachment{}, false, nil
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	attachment, ok := s.attachments[key]
	return attachment, ok, nil
}

func (s *memoryAttachmentStore) Put(_ context.Context, attachment Attachment) error {
	s.put(attachment)
	return nil
}

func (s *memoryAttachmentStore) put(attachment Attachment) {
	key := strings.TrimSpace(attachment.VolumeID)
	if key == "" {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.attachments[key] = attachment
}

func (s *memoryAttachmentStore) Delete(_ context.Context, volumeID string) error {
	s.delete(volumeID)
	return nil
}

func (s *memoryAttachmentStore) delete(volumeID string) {
	key := strings.TrimSpace(volumeID)
	if key == "" {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.attachments, key)
}
