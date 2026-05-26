package controller

import (
	"strings"
	"sync"
)

type Attachment struct {
	VolumeID string
	NodeID   string
	HostNQN  string
	ExportID string
}

type attachmentStore interface {
	Get(volumeID string) (Attachment, bool)
	Put(attachment Attachment)
	Delete(volumeID string)
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

func (s *memoryAttachmentStore) Get(volumeID string) (Attachment, bool) {
	key := strings.TrimSpace(volumeID)
	if key == "" {
		return Attachment{}, false
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	attachment, ok := s.attachments[key]
	return attachment, ok
}

func (s *memoryAttachmentStore) Put(attachment Attachment) {
	key := strings.TrimSpace(attachment.VolumeID)
	if key == "" {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.attachments[key] = attachment
}

func (s *memoryAttachmentStore) Delete(volumeID string) {
	key := strings.TrimSpace(volumeID)
	if key == "" {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.attachments, key)
}
