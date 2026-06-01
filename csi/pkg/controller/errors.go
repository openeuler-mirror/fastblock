package controller

import (
	"errors"

	"fastblock-csi/pkg/monitorclient"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

var (
	ErrVolumePublishedToAnotherNode = errors.New("volume is already published to another node")
	ErrVolumeStillPublished         = errors.New("volume is still published")
	ErrAttachmentNodeMismatch       = errors.New("volume attachment belongs to another node")
	ErrSnapshotNotSupported         = errors.New("snapshot operations are not supported by the monitor backend")
	ErrSnapshotNotReady             = errors.New("snapshot is not ready to use")
	ErrSnapshotRestoreSizeTooSmall  = errors.New("requested volume capacity is smaller than the snapshot size")
)

func toGRPCError(err error) error {
	switch {
	case errors.Is(err, ErrVolumePublishedToAnotherNode),
		errors.Is(err, ErrVolumeStillPublished),
		errors.Is(err, ErrAttachmentNodeMismatch):
		return status.Error(codes.FailedPrecondition, err.Error())
	case errors.Is(err, ErrSnapshotNotSupported):
		return status.Error(codes.Unimplemented, err.Error())
	case errors.Is(err, ErrSnapshotNotReady):
		return status.Error(codes.FailedPrecondition, err.Error())
	case errors.Is(err, ErrSnapshotRestoreSizeTooSmall):
		return status.Error(codes.InvalidArgument, err.Error())
	case errors.Is(err, monitorclient.ErrSnapshotNotFound):
		return status.Error(codes.NotFound, err.Error())
	default:
		return err
	}
}
