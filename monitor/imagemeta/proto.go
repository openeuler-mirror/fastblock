package imagemeta

import (
	"context"
	"errors"
	"time"

	"monitor/etcdapi"
	"monitor/msg"
)

func PutImageProto(ctx context.Context, client *etcdapi.EtcdClient, metadata *msg.ImageMetadataV2) msg.ImageMetadataErrorCode {
	item, err := imageFromProto(metadata)
	if err != nil {
		return toImageMetadataError(err)
	}
	if err := PutImage(ctx, client, item); err != nil {
		return toImageMetadataError(err)
	}
	return msg.ImageMetadataErrorCode_imageMetadataOk
}

func GetImageProto(ctx context.Context, client *etcdapi.EtcdClient, imageID string) (msg.ImageMetadataErrorCode, *msg.ImageMetadataV2) {
	item, err := GetImage(ctx, client, imageID)
	if err != nil {
		return toImageMetadataError(err), nil
	}
	return msg.ImageMetadataErrorCode_imageMetadataOk, imageToProto(item)
}

func DeleteImageProto(ctx context.Context, client *etcdapi.EtcdClient, imageID string) msg.ImageMetadataErrorCode {
	if err := DeleteImage(ctx, client, imageID); err != nil {
		return toImageMetadataError(err)
	}
	return msg.ImageMetadataErrorCode_imageMetadataOk
}

func ListImagesProto(ctx context.Context, client *etcdapi.EtcdClient) (msg.ImageMetadataErrorCode, []*msg.ImageMetadataV2) {
	items, err := ListImages(ctx, client)
	if err != nil {
		return toImageMetadataError(err), nil
	}
	resp := make([]*msg.ImageMetadataV2, 0, len(items))
	for _, item := range items {
		resp = append(resp, imageToProto(item))
	}
	return msg.ImageMetadataErrorCode_imageMetadataOk, resp
}

func PutSnapshotProto(ctx context.Context, client *etcdapi.EtcdClient, metadata *msg.SnapshotMetadataV2) msg.ImageMetadataErrorCode {
	item, err := snapshotFromProto(metadata)
	if err != nil {
		return toImageMetadataError(err)
	}
	if err := PutSnapshot(ctx, client, item); err != nil {
		return toImageMetadataError(err)
	}
	return msg.ImageMetadataErrorCode_imageMetadataOk
}

func GetSnapshotProto(ctx context.Context, client *etcdapi.EtcdClient, imageID, snapshotID string) (msg.ImageMetadataErrorCode, *msg.SnapshotMetadataV2) {
	item, err := GetSnapshot(ctx, client, imageID, snapshotID)
	if err != nil {
		return toImageMetadataError(err), nil
	}
	return msg.ImageMetadataErrorCode_imageMetadataOk, snapshotToProto(item)
}

func DeleteSnapshotProto(ctx context.Context, client *etcdapi.EtcdClient, imageID, snapshotID string) msg.ImageMetadataErrorCode {
	if err := DeleteSnapshot(ctx, client, imageID, snapshotID); err != nil {
		return toImageMetadataError(err)
	}
	return msg.ImageMetadataErrorCode_imageMetadataOk
}

func ListSnapshotsProto(ctx context.Context, client *etcdapi.EtcdClient, imageID string) (msg.ImageMetadataErrorCode, []*msg.SnapshotMetadataV2) {
	items, err := ListSnapshots(ctx, client, imageID)
	if err != nil {
		return toImageMetadataError(err), nil
	}
	resp := make([]*msg.SnapshotMetadataV2, 0, len(items))
	for _, item := range items {
		resp = append(resp, snapshotToProto(item))
	}
	return msg.ImageMetadataErrorCode_imageMetadataOk, resp
}

func imageToProto(item *ImageMetadata) *msg.ImageMetadataV2 {
	if item == nil {
		return nil
	}
	return &msg.ImageMetadataV2{
		ImageId:           item.ImageID,
		PoolId:            item.PoolID,
		PoolName:          item.PoolName,
		ImageName:         item.ImageName,
		Size_:             item.Size,
		ObjectSize:        item.ObjectSize,
		Features:          item.Features,
		Status:            string(item.Status),
		ParentSnapshotId:  item.ParentSnapshotID,
		Depth:             item.Depth,
		CreatedAtUnixNano: item.CreatedAt.UnixNano(),
		UpdatedAtUnixNano: item.UpdatedAt.UnixNano(),
		Generation:        item.Generation,
	}
}

func imageFromProto(metadata *msg.ImageMetadataV2) (*ImageMetadata, error) {
	if metadata == nil {
		return nil, errors.New("image metadata is required")
	}
	item := &ImageMetadata{
		ImageID:          metadata.GetImageId(),
		PoolID:           metadata.GetPoolId(),
		PoolName:         metadata.GetPoolName(),
		ImageName:        metadata.GetImageName(),
		Size:             metadata.GetSize_(),
		ObjectSize:       metadata.GetObjectSize(),
		Features:         append([]string(nil), metadata.GetFeatures()...),
		Status:           ImageStatus(metadata.GetStatus()),
		ParentSnapshotID: metadata.GetParentSnapshotId(),
		Depth:            metadata.GetDepth(),
		Generation:       metadata.GetGeneration(),
	}
	if ts := metadata.GetCreatedAtUnixNano(); ts != 0 {
		item.CreatedAt = time.Unix(0, ts).UTC()
	}
	if ts := metadata.GetUpdatedAtUnixNano(); ts != 0 {
		item.UpdatedAt = time.Unix(0, ts).UTC()
	}
	return item, nil
}

func snapshotToProto(item *SnapshotMetadata) *msg.SnapshotMetadataV2 {
	if item == nil {
		return nil
	}
	return &msg.SnapshotMetadataV2{
		SnapshotId:        item.SnapshotID,
		SnapshotName:      item.SnapshotName,
		SourceImageId:     item.SourceImageID,
		SourcePoolId:      item.SourcePoolID,
		SourcePoolName:    item.SourcePoolName,
		SourceImageName:   item.SourceImageName,
		SnapSeq:           item.SnapSeq,
		Status:            string(item.Status),
		Protected:         item.Protected,
		CreatedAtUnixNano: item.CreatedAt.UnixNano(),
		UpdatedAtUnixNano: item.UpdatedAt.UnixNano(),
		OperationId:       item.OperationID,
		ChildCount:        item.ChildCount,
	}
}

func snapshotFromProto(metadata *msg.SnapshotMetadataV2) (*SnapshotMetadata, error) {
	if metadata == nil {
		return nil, errors.New("snapshot metadata is required")
	}
	item := &SnapshotMetadata{
		SnapshotID:      metadata.GetSnapshotId(),
		SnapshotName:    metadata.GetSnapshotName(),
		SourceImageID:   metadata.GetSourceImageId(),
		SourcePoolID:    metadata.GetSourcePoolId(),
		SourcePoolName:  metadata.GetSourcePoolName(),
		SourceImageName: metadata.GetSourceImageName(),
		SnapSeq:         metadata.GetSnapSeq(),
		Status:          SnapshotStatus(metadata.GetStatus()),
		Protected:       metadata.GetProtected(),
		OperationID:     metadata.GetOperationId(),
		ChildCount:      metadata.GetChildCount(),
	}
	if ts := metadata.GetCreatedAtUnixNano(); ts != 0 {
		item.CreatedAt = time.Unix(0, ts).UTC()
	}
	if ts := metadata.GetUpdatedAtUnixNano(); ts != 0 {
		item.UpdatedAt = time.Unix(0, ts).UTC()
	}
	return item, nil
}

func toImageMetadataError(err error) msg.ImageMetadataErrorCode {
	switch {
	case err == nil:
		return msg.ImageMetadataErrorCode_imageMetadataOk
	case errors.Is(err, ErrImageNotFound), errors.Is(err, ErrSnapshotNotFound), errors.Is(err, ErrOperationNotFound):
		return msg.ImageMetadataErrorCode_imageMetadataNotFound
	default:
		if isInvalidArgument(err) {
			return msg.ImageMetadataErrorCode_imageMetadataInvalidArgument
		}
		return msg.ImageMetadataErrorCode_imageMetadataInternalError
	}
}

func isInvalidArgument(err error) bool {
	if err == nil {
		return false
	}
	text := err.Error()
	switch text {
	case "client is required",
		"client and image metadata are required",
		"client and snapshot metadata are required",
		"image metadata is required",
		"snapshot metadata is required",
		"image id, pool name and image name are required",
		"image size and object size must be positive",
		"invalid image status",
		"snapshot id, snapshot name and source image id are required",
		"source pool name and source image name are required",
		"invalid snapshot status":
		return true
	default:
		return false
	}
}
