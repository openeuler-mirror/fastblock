package backend

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"time"
)

type VolumeContext struct {
	Transport string
	NQN       string
	Traddr    string
	Trsvcid   string
	NSID      int
}

type Interface interface {
	Stage(ctx context.Context, volumeID string, volumeCtx VolumeContext) (string, error)
	Unstage(ctx context.Context, volumeID string, volumeCtx VolumeContext) error
	GetDevice(ctx context.Context, volumeID string, volumeCtx VolumeContext) (string, error)
	IsReady(ctx context.Context, volumeID string, volumeCtx VolumeContext) (bool, error)
}

type NVMFBackend struct {
	ConnectTimeout time.Duration
}

func NewNVMF() *NVMFBackend {
	return &NVMFBackend{ConnectTimeout: 30 * time.Second}
}

func (b *NVMFBackend) Stage(context.Context, string, VolumeContext) (string, error) {
	return "", errors.New("nvmf stage not implemented")
}

func (b *NVMFBackend) Unstage(context.Context, string, VolumeContext) error {
	return errors.New("nvmf unstage not implemented")
}

func (b *NVMFBackend) GetDevice(context.Context, string, VolumeContext) (string, error) {
	return "", errors.New("nvmf get device not implemented")
}

func (b *NVMFBackend) IsReady(context.Context, string, VolumeContext) (bool, error) {
	return false, errors.New("nvmf readiness not implemented")
}

func ValidateVolumeContext(ctx VolumeContext) error {
	if strings.TrimSpace(ctx.Transport) == "" {
		return errors.New("transport is required")
	}
	if strings.TrimSpace(ctx.NQN) == "" {
		return errors.New("nqn is required")
	}
	if strings.TrimSpace(ctx.Traddr) == "" {
		return errors.New("traddr is required")
	}
	if strings.TrimSpace(ctx.Trsvcid) == "" {
		return errors.New("trsvcid is required")
	}
	if ctx.NSID <= 0 {
		return fmt.Errorf("invalid nsid %d", ctx.NSID)
	}
	return nil
}
