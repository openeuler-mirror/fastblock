package mount

import (
	"errors"
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
