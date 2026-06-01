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
	SysModuleRoot  string
	HostNQNPath    string
	runner         commandRunner
	lookPath       func(string) (string, error)
}

func NewNVMF() *NVMFBackend {
	return &NVMFBackend{
		ConnectTimeout: 30 * time.Second,
		PollInterval:   200 * time.Millisecond,
		DeviceRoot:     "/dev",
		SysClassNVMe:   "/sys/class/nvme",
		SysClassBlock:  "/sys/class/block",
		SysModuleRoot:  "/sys/module",
		HostNQNPath:    "/etc/nvme/hostnqn",
		runner:         execRunner{},
		lookPath:       exec.LookPath,
	}
}

func (b *NVMFBackend) Stage(ctx context.Context, _ string, volumeCtx VolumeContext) (string, error) {
	if err := ValidateVolumeContext(volumeCtx); err != nil {
		return "", err
	}
	if err := b.preflight(volumeCtx); err != nil {
		return "", err
	}
	if ready, _ := b.IsReady(ctx, "", volumeCtx); ready {
		return b.GetDevice(ctx, "", volumeCtx)
	}
	if err := b.runner.Run(ctx, "nvme", buildConnectArgs(volumeCtx)...); err != nil {
		if !isAlreadyConnectedError(err) {
			return "", err
		}
		devicePath, waitErr := b.waitForDevice(ctx, volumeCtx)
		if waitErr == nil {
			return devicePath, nil
		}
		if !errors.Is(waitErr, context.DeadlineExceeded) {
			return "", waitErr
		}
		if err := b.runner.Run(ctx, "nvme", buildDisconnectArgs(volumeCtx.NQN)...); err != nil && !isAlreadyConnectedError(err) {
			return "", err
		}
		if err := b.runner.Run(ctx, "nvme", buildConnectArgs(volumeCtx)...); err != nil {
			return "", err
		}
	}
	return b.waitForDevice(ctx, volumeCtx)
}

func (b *NVMFBackend) Unstage(ctx context.Context, _ string, volumeCtx VolumeContext) error {
	if err := ValidateVolumeContext(volumeCtx); err != nil {
		return err
	}
	err := b.runner.Run(ctx, "nvme", buildDisconnectArgs(volumeCtx.NQN)...)
	if err != nil && isAlreadyDisconnectedError(err) {
		return nil
	}
	return err
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

func (b *NVMFBackend) preflight(volumeCtx VolumeContext) error {
	if b.lookPath == nil {
		b.lookPath = exec.LookPath
	}
	if _, err := b.lookPath("nvme"); err != nil {
		return fmt.Errorf("nvme command not available: %w", err)
	}
	hostNQN, err := readTrimmed(b.HostNQNPath)
	if err != nil {
		return fmt.Errorf("read hostnqn failed: %w", err)
	}
	if hostNQN == "" {
		return errors.New("hostnqn is empty")
	}
	module := transportModule(volumeCtx.Transport)
	if module == "" {
		return fmt.Errorf("unsupported transport %q", volumeCtx.Transport)
	}
	if _, err := os.Stat(filepath.Join(b.SysModuleRoot, module)); err != nil {
		return fmt.Errorf("required kernel module %s not loaded: %w", module, err)
	}
	return nil
}

func transportModule(transport string) string {
	switch transport {
	case "tcp":
		return "nvme_tcp"
	case "rdma":
		return "nvme_rdma"
	default:
		return ""
	}
}

func buildConnectArgs(volumeCtx VolumeContext) []string {
	return []string{
		"connect",
		"-t", volumeCtx.Transport,
		"-n", volumeCtx.NQN,
		"-a", volumeCtx.Traddr,
		"-s", volumeCtx.Trsvcid,
	}
}

func buildDisconnectArgs(nqn string) []string {
	return []string{
		"disconnect",
		"-n", nqn,
	}
}

func isAlreadyConnectedError(err error) bool {
	if err == nil {
		return false
	}
	return strings.Contains(strings.ToLower(err.Error()), "already connected")
}

func isAlreadyDisconnectedError(err error) bool {
	if err == nil {
		return false
	}
	text := strings.ToLower(err.Error())
	return strings.Contains(text, "not connected") ||
		strings.Contains(text, "no controller") ||
		strings.Contains(text, "no matching") ||
		strings.Contains(text, "failed to disconnect")
}

func (b *NVMFBackend) waitForDevice(ctx context.Context, volumeCtx VolumeContext) (string, error) {
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
