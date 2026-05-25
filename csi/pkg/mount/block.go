package mount

import (
	"errors"
	"os"
	"path/filepath"
	"strings"
)

func ValidateBlockTarget(devicePath, stagePath string) error {
	if strings.TrimSpace(devicePath) == "" {
		return errors.New("device path is required")
	}
	if strings.TrimSpace(stagePath) == "" {
		return errors.New("stage path is required")
	}
	if !filepath.IsAbs(devicePath) {
		return errors.New("device path must be absolute")
	}
	if !filepath.IsAbs(stagePath) {
		return errors.New("stage path must be absolute")
	}
	return nil
}

func ValidateBlockPublishTarget(devicePath, stagePath, targetPath string) error {
	if err := ValidateBlockTarget(devicePath, stagePath); err != nil {
		return err
	}
	if strings.TrimSpace(targetPath) == "" {
		return errors.New("target path is required")
	}
	if !filepath.IsAbs(targetPath) {
		return errors.New("target path must be absolute")
	}
	if filepath.Clean(stagePath) == filepath.Clean(targetPath) {
		return errors.New("stage path and target path must be different")
	}
	return nil
}

func CanonicalStageDevicePath(stagePath string) (string, error) {
	if strings.TrimSpace(stagePath) == "" {
		return "", errors.New("stage path is required")
	}
	if !filepath.IsAbs(stagePath) {
		return "", errors.New("stage path must be absolute")
	}
	return filepath.Join(stagePath, "device"), nil
}

func CanonicalPublishDevicePath(targetPath string) (string, error) {
	if strings.TrimSpace(targetPath) == "" {
		return "", errors.New("target path is required")
	}
	if !filepath.IsAbs(targetPath) {
		return "", errors.New("target path must be absolute")
	}
	return filepath.Join(targetPath, "device"), nil
}

func WriteStageDeviceLink(stagePath, devicePath string) error {
	linkPath, err := CanonicalStageDevicePath(stagePath)
	if err != nil {
		return err
	}
	if err := ValidateBlockTarget(devicePath, stagePath); err != nil {
		return err
	}
	_ = os.Remove(linkPath)
	return os.Symlink(devicePath, linkPath)
}

func RemoveStageDeviceLink(stagePath string) error {
	linkPath, err := CanonicalStageDevicePath(stagePath)
	if err != nil {
		return err
	}
	err = os.Remove(linkPath)
	if err != nil && !os.IsNotExist(err) {
		return err
	}
	return nil
}
