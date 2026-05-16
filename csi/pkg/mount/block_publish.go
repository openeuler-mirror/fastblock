package mount

import (
	"context"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
)

type BlockPublisher struct {
	runner commandRunner
}

type commandRunner interface {
	Run(ctx context.Context, name string, args ...string) error
}

type execRunner struct{}

func NewBlockPublisher() *BlockPublisher {
	return &BlockPublisher{runner: execRunner{}}
}

func (execRunner) Run(ctx context.Context, name string, args ...string) error {
	cmd := exec.CommandContext(ctx, name, args...)
	output, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("%s %s failed: %w: %s", name, strings.Join(args, " "), err, strings.TrimSpace(string(output)))
	}
	return nil
}

func (p *BlockPublisher) PublishBlockDevice(ctx context.Context, devicePath, stagePath, targetPath string) error {
	if err := ValidateBlockPublishTarget(devicePath, stagePath, targetPath); err != nil {
		return err
	}
	if same, err := isAlreadyPublished(devicePath, targetPath); err != nil {
		return err
	} else if same {
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(targetPath), 0o755); err != nil {
		return err
	}
	file, err := os.OpenFile(targetPath, os.O_CREATE, 0o644)
	if err != nil {
		return err
	}
	_ = file.Close()
	return p.runner.Run(ctx, "mount", "--bind", devicePath, targetPath)
}

func (p *BlockPublisher) UnpublishBlockDevice(ctx context.Context, targetPath string) error {
	if strings.TrimSpace(targetPath) == "" {
		return fmt.Errorf("target path is required")
	}
	if !filepath.IsAbs(targetPath) {
		return fmt.Errorf("target path must be absolute")
	}
	if _, err := os.Stat(targetPath); err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}
	if err := p.runner.Run(ctx, "umount", targetPath); err != nil {
		return err
	}
	return os.Remove(targetPath)
}

func isAlreadyPublished(devicePath, targetPath string) (bool, error) {
	deviceInfo, err := os.Stat(devicePath)
	if err != nil {
		if os.IsNotExist(err) {
			return false, nil
		}
		return false, err
	}
	targetInfo, err := os.Stat(targetPath)
	if err != nil {
		if os.IsNotExist(err) {
			return false, nil
		}
		return false, err
	}
	return os.SameFile(deviceInfo, targetInfo), nil
}
