package controller

import (
	"context"
	"strings"
	"sync"

	"fastblock-csi/pkg/monitorclient"
)

type VolumeMetadata = monitorclient.VolumeMetadata

type volumeStore interface {
	Get(ctx context.Context, volumeID string) (VolumeMetadata, bool, error)
	Put(ctx context.Context, metadata VolumeMetadata) error
	Delete(ctx context.Context, volumeID string) error
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

func (s *memoryVolumeStore) Get(_ context.Context, volumeID string) (VolumeMetadata, bool, error) {
	return s.get(volumeID)
}

func (s *memoryVolumeStore) get(volumeID string) (VolumeMetadata, bool, error) {
	key := strings.TrimSpace(volumeID)
	if key == "" {
		return VolumeMetadata{}, false, nil
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	metadata, ok := s.volumes[key]
	return metadata, ok, nil
}

func (s *memoryVolumeStore) Put(_ context.Context, metadata VolumeMetadata) error {
	s.put(metadata)
	return nil
}

func (s *memoryVolumeStore) put(metadata VolumeMetadata) {
	key := strings.TrimSpace(metadata.Volume.ID)
	if key == "" {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.volumes[key] = metadata
}

func (s *memoryVolumeStore) Delete(_ context.Context, volumeID string) error {
	s.delete(volumeID)
	return nil
}

func (s *memoryVolumeStore) delete(volumeID string) {
	key := strings.TrimSpace(volumeID)
	if key == "" {
		return
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.volumes, key)
}
