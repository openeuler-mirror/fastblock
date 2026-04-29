package controller

import (
	"strings"
	"sync"

	"fastblock-csi/pkg/monitorclient"
)

type VolumeMetadata struct {
	Volume    monitorclient.Volume
	BlockSize int64
	Transport string
	ExportID  string
}

type volumeStore interface {
	Get(volumeID string) (VolumeMetadata, bool)
	Put(metadata VolumeMetadata)
	Delete(volumeID string)
}

type memoryVolumeStore struct {
	mu      sync.RWMutex
	volumes map[string]VolumeMetadata
}

func newMemoryVolumeStore() *memoryVolumeStore {
	return &memoryVolumeStore{
		volumes: make(map[string]VolumeMetadata),
	}
}

func (s *memoryVolumeStore) Get(volumeID string) (VolumeMetadata, bool) {
	key := strings.TrimSpace(volumeID)
	if key == "" {
		return VolumeMetadata{}, false
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	metadata, ok := s.volumes[key]
	return metadata, ok
}

func (s *memoryVolumeStore) Put(metadata VolumeMetadata) {
	key := strings.TrimSpace(metadata.Volume.ID)
	if key == "" {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.volumes[key] = metadata
}

func (s *memoryVolumeStore) Delete(volumeID string) {
	key := strings.TrimSpace(volumeID)
	if key == "" {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.volumes, key)
}
