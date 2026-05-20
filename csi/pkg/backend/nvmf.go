package backend

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
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
	PollInterval   time.Duration
	DeviceRoot     string
	SysClassNVMe   string
	SysClassBlock  string
	runner         commandRunner
}

func NewNVMF() *NVMFBackend {
	return &NVMFBackend{
		ConnectTimeout: 30 * time.Second,
		PollInterval:   200 * time.Millisecond,
		DeviceRoot:     "/dev",
		SysClassNVMe:   "/sys/class/nvme",
		SysClassBlock:  "/sys/class/block",
		runner:         execRunner{},
	}
}

func (b *NVMFBackend) Stage(ctx context.Context, _ string, volumeCtx VolumeContext) (string, error) {
	if err := ValidateVolumeContext(volumeCtx); err != nil {
		return "", err
	}
	if ready, _ := b.IsReady(ctx, "", volumeCtx); ready {
		return b.GetDevice(ctx, "", volumeCtx)
	}
	if err := b.runner.Run(ctx, "nvme", "connect", "-t", volumeCtx.Transport, "-n", volumeCtx.NQN, "-a", volumeCtx.Traddr, "-s", volumeCtx.Trsvcid); err != nil {
		return "", err
	}

	waitCtx, cancel := context.WithTimeout(ctx, b.ConnectTimeout)
	defer cancel()
	for {
		devicePath, err := b.GetDevice(waitCtx, "", volumeCtx)
		if err == nil {
			return devicePath, nil
		}
		if !errors.Is(err, ErrDeviceNotFound) {
			return "", err
		}
		select {
		case <-waitCtx.Done():
			return "", waitCtx.Err()
		case <-time.After(b.PollInterval):
		}
	}
}

func (b *NVMFBackend) Unstage(ctx context.Context, _ string, volumeCtx VolumeContext) error {
	if err := ValidateVolumeContext(volumeCtx); err != nil {
		return err
	}
	return b.runner.Run(ctx, "nvme", "disconnect", "-n", volumeCtx.NQN)
}

func (b *NVMFBackend) GetDevice(_ context.Context, _ string, volumeCtx VolumeContext) (string, error) {
	controllers, err := b.controllersForNQN(volumeCtx.NQN)
	if err != nil {
		return "", err
	}
	expectedNSID := strconv.Itoa(volumeCtx.NSID)
	for _, controller := range controllers {
		matches, err := filepath.Glob(filepath.Join(b.SysClassBlock, controller+"n*"))
		if err != nil {
			return "", err
		}
		for _, match := range matches {
			nsid, err := readTrimmed(filepath.Join(match, "nsid"))
			if err == nil && nsid == expectedNSID {
				return filepath.Join(b.DeviceRoot, filepath.Base(match)), nil
			}
		}
	}
	return "", ErrDeviceNotFound
}

func (b *NVMFBackend) IsReady(ctx context.Context, volumeID string, volumeCtx VolumeContext) (bool, error) {
	_, err := b.GetDevice(ctx, volumeID, volumeCtx)
	if err == nil {
		return true, nil
	}
	if errors.Is(err, ErrDeviceNotFound) {
		return false, nil
	}
	return false, err
}

func ValidateVolumeContext(ctx VolumeContext) error {
	if strings.TrimSpace(ctx.Transport) == "" {
		return errors.New("transport is required")
	}
	if ctx.Transport != "rdma" && ctx.Transport != "tcp" {
		return fmt.Errorf("unsupported transport %q", ctx.Transport)
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

var ErrDeviceNotFound = errors.New("nvmf device not found")

type commandRunner interface {
	Run(ctx context.Context, name string, args ...string) error
}

type execRunner struct{}

func (execRunner) Run(ctx context.Context, name string, args ...string) error {
	cmd := exec.CommandContext(ctx, name, args...)
	output, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("%s %s failed: %w: %s", name, strings.Join(args, " "), err, strings.TrimSpace(string(output)))
	}
	return nil
}

func (b *NVMFBackend) controllersForNQN(nqn string) ([]string, error) {
	matches, err := filepath.Glob(filepath.Join(b.SysClassNVMe, "nvme*"))
	if err != nil {
		return nil, err
	}
	var controllers []string
	for _, match := range matches {
		subsysnqn, err := readTrimmed(filepath.Join(match, "subsysnqn"))
		if err == nil && subsysnqn == nqn {
			controllers = append(controllers, filepath.Base(match))
		}
	}
	return controllers, nil
}

func readTrimmed(path string) (string, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return "", err
	}
	return strings.TrimSpace(string(data)), nil
}
