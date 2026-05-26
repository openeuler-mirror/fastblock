package exporterclient

import (
	"errors"
	"strings"
)

var exportIDReplacer = strings.NewReplacer(":", "-", "/", "-", " ", "-", ".", "-")

// ExportIDForVolume must stay aligned with the exporter-side export ID derivation.
func ExportIDForVolume(volumeID string) (string, error) {
	trimmed := strings.TrimSpace(volumeID)
	if trimmed == "" {
		return "", errors.New("volume id is required")
	}
	return exportIDReplacer.Replace(trimmed), nil
}
