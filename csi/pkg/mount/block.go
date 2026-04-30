package mount

import (
	"errors"
	"os"
	"path/filepath"
	"strconv"
	"strings"

	"golang.org/x/sys/unix"
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
	return writeStageDeviceLinkWithSysfsRoot(stagePath, devicePath, "/sys/class/block")
}

func writeStageDeviceLinkWithSysfsRoot(stagePath, devicePath, sysClassBlockRoot string) error {
	linkPath, err := CanonicalStageDevicePath(stagePath)
	if err != nil {
		return err
	}
	if err := ValidateBlockTarget(devicePath, stagePath); err != nil {
		return err
	}
	_ = os.Remove(linkPath)
	if _, err := os.Stat(devicePath); err == nil {
		return os.Symlink(devicePath, linkPath)
	}
	dev, err := deviceNumberFromSysfs(devicePath, sysClassBlockRoot)
	if err == nil {
		return unix.Mknod(linkPath, unix.S_IFBLK|0o600, int(dev))
	}
	return os.Symlink(devicePath, linkPath)
}

func deviceNumberFromSysfs(devicePath, sysClassBlockRoot string) (uint64, error) {
	name := filepath.Base(devicePath)
	data, err := os.ReadFile(filepath.Join(sysClassBlockRoot, name, "dev"))
	if err != nil {
		return 0, err
	}
	parts := strings.Split(strings.TrimSpace(string(data)), ":")
	if len(parts) != 2 {
		return 0, errors.New("invalid device major:minor format")
	}
	major, err := strconv.ParseUint(parts[0], 10, 32)
	if err != nil {
		return 0, err
	}
	minor, err := strconv.ParseUint(parts[1], 10, 32)
	if err != nil {
		return 0, err
	}
	return unix.Mkdev(uint32(major), uint32(minor)), nil
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
