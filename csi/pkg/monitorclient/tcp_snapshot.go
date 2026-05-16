package monitorclient

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"time"

	"fastblock-csi/pkg/volumeid"
	msg "monitor/msg"
)

func (c *TCPClient) CreateSnapshot(ctx context.Context, req CreateSnapshotRequest) (Snapshot, error) {
	if err := ValidateAddress(c.address); err != nil {
		return Snapshot{}, err
	}
	if err := req.Validate(); err != nil {
		return Snapshot{}, err
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_CreateImageSnapshotRequest{
			CreateImageSnapshotRequest: &msg.CreateImageSnapshotRequest{
				PoolName:     req.SourceVolume.Pool,
				ImageName:    req.SourceVolume.Name,
				SnapshotName: req.Name,
			},
		},
	})
	if err != nil {
		return Snapshot{}, err
	}
	payload, ok := resp.Union.(*msg.Response_CreateImageSnapshotResponse)
	if !ok {
		return Snapshot{}, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	switch payload.CreateImageSnapshotResponse.GetErrorcode() {
	case msg.ImageMetadataErrorCode_imageMetadataOk:
		return c.snapshotFromProto(ctx, payload.CreateImageSnapshotResponse.GetMetadata())
	case msg.ImageMetadataErrorCode_imageMetadataInvalidArgument:
		existing, err := c.getSnapshotByName(ctx, req.SourceVolume.Pool, req.SourceVolume.Name, req.Name)
		if err == nil {
			return existing, nil
		}
		return Snapshot{}, fmt.Errorf("create snapshot failed: %s", payload.CreateImageSnapshotResponse.GetErrorcode().String())
	case msg.ImageMetadataErrorCode_imageMetadataNotFound:
		return Snapshot{}, ErrImageNotFound
	default:
		return Snapshot{}, fmt.Errorf("create snapshot failed: %s", payload.CreateImageSnapshotResponse.GetErrorcode().String())
	}
}

func (c *TCPClient) DeleteSnapshot(ctx context.Context, snapshotID string) error {
	if err := ValidateAddress(c.address); err != nil {
		return err
	}
	if strings.TrimSpace(snapshotID) == "" {
		return fmt.Errorf("snapshot id is required")
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_DeleteImageSnapshotRequest{
			DeleteImageSnapshotRequest: &msg.DeleteImageSnapshotRequest{
				SnapshotId: snapshotID,
			},
		},
	})
	if err != nil {
		return err
	}
	payload, ok := resp.Union.(*msg.Response_DeleteImageSnapshotResponse)
	if !ok {
		return fmt.Errorf("unexpected response type %T", resp.Union)
	}
	return snapshotError(payload.DeleteImageSnapshotResponse.GetErrorcode())
}

func (c *TCPClient) GetSnapshot(ctx context.Context, snapshotID string) (Snapshot, error) {
	if err := ValidateAddress(c.address); err != nil {
		return Snapshot{}, err
	}
	if strings.TrimSpace(snapshotID) == "" {
		return Snapshot{}, fmt.Errorf("snapshot id is required")
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_GetSnapshotMetadataByIdRequest{
			GetSnapshotMetadataByIdRequest: &msg.GetSnapshotMetadataByIDRequest{
				SnapshotId: snapshotID,
			},
		},
	})
	if err != nil {
		return Snapshot{}, err
	}
	payload, ok := resp.Union.(*msg.Response_GetSnapshotMetadataByIdResponse)
	if !ok {
		return Snapshot{}, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if err := snapshotError(payload.GetSnapshotMetadataByIdResponse.GetErrorcode()); err != nil {
		return Snapshot{}, err
	}
	if snapshotMetadataDeleted(payload.GetSnapshotMetadataByIdResponse.GetMetadata()) {
		return Snapshot{}, ErrSnapshotNotFound
	}
	return c.snapshotFromProto(ctx, payload.GetSnapshotMetadataByIdResponse.GetMetadata())
}

func (c *TCPClient) ListSnapshots(ctx context.Context, req ListSnapshotsRequest) ([]Snapshot, error) {
	if err := ValidateAddress(c.address); err != nil {
		return nil, err
	}
	if req.SnapshotID != "" {
		snapshot, err := c.GetSnapshot(ctx, req.SnapshotID)
		if err != nil {
			if errors.Is(err, ErrSnapshotNotFound) {
				return nil, nil
			}
			return nil, err
		}
		if req.SourceVolumeID != "" && snapshot.SourceVolume.ID != strings.TrimSpace(req.SourceVolumeID) {
			return nil, nil
		}
		return []Snapshot{snapshot}, nil
	}
	if req.SourceVolumeID != "" {
		ref, err := c.resolveVolumeRefByID(ctx, req.SourceVolumeID)
		if err != nil {
			if errors.Is(err, ErrMetadataNotFound) {
				return nil, nil
			}
			return nil, err
		}
		metadata, err := c.imageMetadataByName(ctx, ref.Pool, ref.Name)
		if err != nil {
			if errors.Is(err, ErrImageNotFound) {
				return nil, nil
			}
			return nil, err
		}
		return c.listSnapshotsByImage(ctx, metadata)
	}

	images, err := c.listImageMetadata(ctx)
	if err != nil {
		return nil, err
	}
	result := make([]Snapshot, 0)
	for _, image := range images {
		items, err := c.listSnapshotsByImage(ctx, image)
		if err != nil {
			return nil, err
		}
		result = append(result, items...)
	}
	return result, nil
}

func (c *TCPClient) CreateVolumeFromSnapshot(ctx context.Context, req CreateVolumeFromSnapshotRequest) (Volume, error) {
	if err := ValidateAddress(c.address); err != nil {
		return Volume{}, err
	}
	if err := req.Validate(); err != nil {
		return Volume{}, err
	}
	snapshot, err := c.GetSnapshot(ctx, req.SnapshotID)
	if err != nil {
		return Volume{}, err
	}
	if snapshot.SourceVolume.Pool != "" && snapshot.SourceVolume.Pool != req.Pool {
		return Volume{}, fmt.Errorf("snapshot restore into different pool is not supported: source=%s target=%s", snapshot.SourceVolume.Pool, req.Pool)
	}
	if err := c.protectSnapshot(ctx, req.SnapshotID); err != nil {
		return Volume{}, err
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_CreateCloneFromSnapshotRequest{
			CreateCloneFromSnapshotRequest: &msg.CreateCloneFromSnapshotRequest{
				SnapshotId:     req.SnapshotID,
				CloneImageName: req.Name,
			},
		},
	})
	if err != nil {
		return Volume{}, err
	}
	payload, ok := resp.Union.(*msg.Response_CreateCloneFromSnapshotResponse)
	if !ok {
		return Volume{}, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	switch payload.CreateCloneFromSnapshotResponse.GetErrorcode() {
	case msg.ImageMetadataErrorCode_imageMetadataOk:
		return c.volumeFromImageMetadata(ctx, payload.CreateCloneFromSnapshotResponse.GetMetadata(), req)
	case msg.ImageMetadataErrorCode_imageMetadataInvalidArgument:
		existing, err := c.getCloneByName(ctx, req.Pool, req.Name)
		if err == nil && strings.TrimSpace(existing.ParentSnapshotId) == strings.TrimSpace(req.SnapshotID) {
			return c.volumeFromImageMetadata(ctx, existing, req)
		}
		return Volume{}, fmt.Errorf("create clone from snapshot failed: %s", payload.CreateCloneFromSnapshotResponse.GetErrorcode().String())
	case msg.ImageMetadataErrorCode_imageMetadataNotFound:
		return Volume{}, ErrSnapshotNotFound
	default:
		return Volume{}, fmt.Errorf("create clone from snapshot failed: %s", payload.CreateCloneFromSnapshotResponse.GetErrorcode().String())
	}
}

func (c *TCPClient) getSnapshotByName(ctx context.Context, pool, image, snapshot string) (Snapshot, error) {
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_GetSnapshotIdByNameRequest{
			GetSnapshotIdByNameRequest: &msg.GetSnapshotIDByNameRequest{
				PoolName:     pool,
				ImageName:    image,
				SnapshotName: snapshot,
			},
		},
	})
	if err != nil {
		return Snapshot{}, err
	}
	payload, ok := resp.Union.(*msg.Response_GetSnapshotIdByNameResponse)
	if !ok {
		return Snapshot{}, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if err := snapshotError(payload.GetSnapshotIdByNameResponse.GetErrorcode()); err != nil {
		return Snapshot{}, err
	}
	return c.GetSnapshot(ctx, payload.GetSnapshotIdByNameResponse.GetSnapshotId())
}

func (c *TCPClient) protectSnapshot(ctx context.Context, snapshotID string) error {
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_ProtectSnapshotRequest{
			ProtectSnapshotRequest: &msg.ProtectSnapshotRequest{
				SnapshotId: snapshotID,
			},
		},
	})
	if err != nil {
		return err
	}
	payload, ok := resp.Union.(*msg.Response_ProtectSnapshotResponse)
	if !ok {
		return fmt.Errorf("unexpected response type %T", resp.Union)
	}
	return snapshotError(payload.ProtectSnapshotResponse.GetErrorcode())
}

func (c *TCPClient) listSnapshotsByImage(ctx context.Context, image *msg.ImageMetadataV2) ([]Snapshot, error) {
	if image == nil || strings.TrimSpace(image.GetImageId()) == "" {
		return nil, nil
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_ListSnapshotMetadataRequest{
			ListSnapshotMetadataRequest: &msg.ListSnapshotMetadataRequest{
				ImageId: image.GetImageId(),
			},
		},
	})
	if err != nil {
		return nil, err
	}
	payload, ok := resp.Union.(*msg.Response_ListSnapshotMetadataResponse)
	if !ok {
		return nil, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if err := snapshotError(payload.ListSnapshotMetadataResponse.GetErrorcode()); err != nil {
		return nil, err
	}
	items := payload.ListSnapshotMetadataResponse.GetMetadata()
	result := make([]Snapshot, 0, len(items))
	for _, item := range items {
		if snapshotMetadataDeleted(item) {
			continue
		}
		snapshot := snapshotFromMetadata(item, image)
		result = append(result, snapshot)
	}
	return result, nil
}

func (c *TCPClient) listImageMetadata(ctx context.Context) ([]*msg.ImageMetadataV2, error) {
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_ListImageMetadataRequest{
			ListImageMetadataRequest: &msg.ListImageMetadataRequest{},
		},
	})
	if err != nil {
		return nil, err
	}
	payload, ok := resp.Union.(*msg.Response_ListImageMetadataResponse)
	if !ok {
		return nil, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if err := imageMetadataError(payload.ListImageMetadataResponse.GetErrorcode()); err != nil {
		return nil, err
	}
	return payload.ListImageMetadataResponse.GetMetadata(), nil
}

func (c *TCPClient) imageMetadataByName(ctx context.Context, pool, image string) (*msg.ImageMetadataV2, error) {
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_GetImageMetadataByNameRequest{
			GetImageMetadataByNameRequest: &msg.GetImageMetadataByNameRequest{
				PoolName:  pool,
				ImageName: image,
			},
		},
	})
	if err != nil {
		return nil, err
	}
	payload, ok := resp.Union.(*msg.Response_GetImageMetadataByNameResponse)
	if !ok {
		return nil, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if err := imageMetadataError(payload.GetImageMetadataByNameResponse.GetErrorcode()); err != nil {
		return nil, err
	}
	return payload.GetImageMetadataByNameResponse.GetMetadata(), nil
}

func (c *TCPClient) getCloneByName(ctx context.Context, pool, image string) (*msg.ImageMetadataV2, error) {
	return c.imageMetadataByName(ctx, pool, image)
}

func (c *TCPClient) resolveVolumeRefByID(ctx context.Context, volumeID string) (VolumeRef, error) {
	ref := VolumeRef{ID: strings.TrimSpace(volumeID)}
	nameRef, err := volumeid.DecodeNameRef(volumeID)
	if err == nil {
		ref.Pool = nameRef.Pool
		ref.Name = nameRef.Name
		return ref, nil
	}
	metadata, err := c.GetVolumeMetadata(ctx, volumeID)
	if err != nil {
		return VolumeRef{}, err
	}
	return metadata.Volume.Ref(), nil
}

func (c *TCPClient) snapshotFromProto(ctx context.Context, metadata *msg.SnapshotMetadataV2) (Snapshot, error) {
	if metadata == nil {
		return Snapshot{}, fmt.Errorf("snapshot metadata is required")
	}
	var image *msg.ImageMetadataV2
	if pool := strings.TrimSpace(metadata.GetSourcePoolName()); pool != "" && strings.TrimSpace(metadata.GetSourceImageName()) != "" {
		found, err := c.imageMetadataByName(ctx, pool, metadata.GetSourceImageName())
		if err != nil && !errors.Is(err, ErrImageNotFound) {
			return Snapshot{}, err
		}
		image = found
	}
	return snapshotFromMetadata(metadata, image), nil
}

func (c *TCPClient) volumeFromImageMetadata(_ context.Context, metadata *msg.ImageMetadataV2, req CreateVolumeFromSnapshotRequest) (Volume, error) {
	if metadata == nil || strings.TrimSpace(metadata.GetImageId()) == "" {
		return Volume{}, fmt.Errorf("monitor returned empty clone metadata")
	}
	encodedID, err := volumeid.EncodeNameRef(volumeid.NameRef{
		Pool: metadata.GetPoolName(),
		Name: metadata.GetImageName(),
	})
	if err != nil {
		return Volume{}, err
	}
	capacityBytes := metadata.GetSize_()
	if capacityBytes == 0 {
		capacityBytes = req.CapacityBytes
	}
	return Volume{
		ID:            encodedID,
		Name:          metadata.GetImageName(),
		Pool:          metadata.GetPoolName(),
		CapacityBytes: capacityBytes,
		ObjectSize:    metadata.GetObjectSize(),
	}, nil
}

func snapshotFromMetadata(metadata *msg.SnapshotMetadataV2, image *msg.ImageMetadataV2) Snapshot {
	if metadata == nil {
		return Snapshot{}
	}
	sourcePool := strings.TrimSpace(metadata.GetSourcePoolName())
	sourceImage := strings.TrimSpace(metadata.GetSourceImageName())
	sourceID := ""
	if sourcePool != "" && sourceImage != "" {
		sourceID, _ = volumeid.EncodeNameRef(volumeid.NameRef{
			Pool: sourcePool,
			Name: sourceImage,
		})
	}
	sizeBytes := int64(0)
	if image != nil {
		sizeBytes = image.GetSize_()
		if sourcePool == "" {
			sourcePool = image.GetPoolName()
		}
		if sourceImage == "" {
			sourceImage = image.GetImageName()
		}
		if sourceID == "" && sourcePool != "" && sourceImage != "" {
			sourceID, _ = volumeid.EncodeNameRef(volumeid.NameRef{
				Pool: sourcePool,
				Name: sourceImage,
			})
		}
	}
	return Snapshot{
		ID:   metadata.GetSnapshotId(),
		Name: metadata.GetSnapshotName(),
		SourceVolume: VolumeRef{
			ID:   sourceID,
			Pool: sourcePool,
			Name: sourceImage,
		},
		CreationTime: snapshotCreationTime(metadata),
		SizeBytes:    sizeBytes,
		ReadyToUse:   strings.EqualFold(strings.TrimSpace(metadata.GetStatus()), "ready"),
	}
}

func snapshotMetadataDeleted(metadata *msg.SnapshotMetadataV2) bool {
	if metadata == nil {
		return true
	}
	status := strings.TrimSpace(metadata.GetStatus())
	return strings.EqualFold(status, "deleted_pending_gc") || strings.EqualFold(status, "deleting")
}

func snapshotCreationTime(metadata *msg.SnapshotMetadataV2) time.Time {
	if metadata == nil {
		return time.Time{}
	}
	if ts := metadata.GetCreatedAtUnixNano(); ts != 0 {
		return time.Unix(0, ts).UTC()
	}
	return time.Time{}
}

func snapshotError(code msg.ImageMetadataErrorCode) error {
	switch code {
	case msg.ImageMetadataErrorCode_imageMetadataOk:
		return nil
	case msg.ImageMetadataErrorCode_imageMetadataNotFound:
		return ErrSnapshotNotFound
	case msg.ImageMetadataErrorCode_imageMetadataInvalidArgument:
		return fmt.Errorf("monitor snapshot invalid argument")
	default:
		return fmt.Errorf("monitor snapshot operation failed: %s", code.String())
	}
}
