package controller

import (
	"context"
	"errors"
	"strings"

	"fastblock-csi/pkg/exporterclient"
)

func (s *Service) reconcileState(ctx context.Context, volumeID string) error {
	metadata, metadataOK, err := s.volumes.Get(ctx, volumeID)
	if err != nil {
		return err
	}
	attachment, attachmentOK, err := s.attachments.Get(ctx, volumeID)
	if err != nil {
		return err
	}
	_, leaseOK, err := s.currentLease(ctx, volumeID)
	if err != nil {
		return err
	}

	exportID := ""
	if metadataOK {
		exportID = strings.TrimSpace(metadata.ExportID)
	}
	if exportID == "" && attachmentOK {
		exportID = strings.TrimSpace(attachment.ExportID)
	}

	exportExists := false
	if exportID != "" {
		_, err := s.exporter.GetExport(ctx, exportID)
		if err == nil {
			exportExists = true
		} else if !errors.Is(err, exporterclient.ErrNotFound) {
			return err
		}
	}

	if attachmentOK && !leaseOK && !exportExists {
		if err := s.attachments.Delete(ctx, volumeID); err != nil {
			return err
		}
		attachmentOK = false
	}
	if metadataOK && !exportExists && strings.TrimSpace(metadata.ExportID) != "" {
		metadata.ExportID = ""
		if err := s.volumes.Put(ctx, metadata); err != nil {
			return err
		}
	}
	if attachmentOK && metadataOK && strings.TrimSpace(metadata.ExportID) == "" && strings.TrimSpace(attachment.ExportID) != "" && exportExists {
		metadata.ExportID = attachment.ExportID
		if err := s.volumes.Put(ctx, metadata); err != nil {
			return err
		}
	}
	return nil
}
