package controller

import (
	"errors"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

var (
	ErrVolumePublishedToAnotherNode = errors.New("volume is already published to another node")
	ErrVolumeStillPublished         = errors.New("volume is still published")
	ErrAttachmentNodeMismatch       = errors.New("volume attachment belongs to another node")
)

func toGRPCError(err error) error {
	switch {
	case errors.Is(err, ErrVolumePublishedToAnotherNode),
		errors.Is(err, ErrVolumeStillPublished),
		errors.Is(err, ErrAttachmentNodeMismatch):
		return status.Error(codes.FailedPrecondition, err.Error())
	default:
		return err
	}
}
