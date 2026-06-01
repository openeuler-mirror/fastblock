package monitorclient

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"net"
	"strings"

	"fastblock-csi/pkg/volumeid"
	msg "monitor/msg"

	"github.com/gogo/protobuf/proto"
)

const messageLengthSize = 8

type TCPClient struct {
	address string
	dialer  *net.Dialer
}

func NewTCP(address string) *TCPClient {
	return &TCPClient{
		address: address,
		dialer:  &net.Dialer{},
	}
}

func (c *TCPClient) CreateVolume(ctx context.Context, req CreateVolumeRequest) (Volume, error) {
	if err := ValidateAddress(c.address); err != nil {
		return Volume{}, err
	}
	if err := req.Validate(); err != nil {
		return Volume{}, err
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_CreateImageRequest{
			CreateImageRequest: &msg.CreateImageRequest{
				Poolname:   req.Pool,
				Imagename:  req.Name,
				Size_:      req.CapacityBytes,
				ObjectSize: req.ObjectSize,
			},
		},
	})
	if err != nil {
		return Volume{}, err
	}
	payload, ok := resp.Union.(*msg.Response_CreateImageResponse)
	if !ok {
		return Volume{}, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if payload.CreateImageResponse.GetErrorcode() == msg.CreateImageErrorCode_imageExists {
		existing, err := c.GetVolume(ctx, VolumeRef{Name: req.Name, Pool: req.Pool})
		if err != nil {
			return Volume{}, err
		}
		if existing.CapacityBytes < req.CapacityBytes {
			return Volume{}, fmt.Errorf("existing image capacity %d is smaller than requested %d", existing.CapacityBytes, req.CapacityBytes)
		}
		if existing.ObjectSize != req.ObjectSize {
			return Volume{}, fmt.Errorf("existing image object size %d does not match requested %d", existing.ObjectSize, req.ObjectSize)
		}
		if strings.TrimSpace(existing.Name) != req.Name || strings.TrimSpace(existing.Pool) != req.Pool {
			return Volume{}, fmt.Errorf("existing image identity %s/%s does not match requested %s/%s", existing.Pool, existing.Name, req.Pool, req.Name)
		}
		return existing, nil
	}
	if payload.CreateImageResponse.GetErrorcode() != msg.CreateImageErrorCode_createImageOk {
		return Volume{}, fmt.Errorf("create image failed: %s", payload.CreateImageResponse.GetErrorcode().String())
	}
	return volumeFromImageInfo(payload.CreateImageResponse.GetImageInfo()), nil
}

func (c *TCPClient) DeleteVolume(ctx context.Context, ref VolumeRef) error {
	if err := ValidateAddress(c.address); err != nil {
		return err
	}
	if err := ref.Validate(); err != nil {
		return err
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_RemoveImageRequest{
			RemoveImageRequest: &msg.RemoveImageRequest{
				Poolname:  ref.Pool,
				Imagename: ref.Name,
			},
		},
	})
	if err != nil {
		return err
	}
	payload, ok := resp.Union.(*msg.Response_RemoveImageResponse)
	if !ok {
		return fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if payload.RemoveImageResponse.GetErrorcode() == msg.RemoveImageErrorCode_imageNotFound {
		return nil
	}
	if payload.RemoveImageResponse.GetErrorcode() != msg.RemoveImageErrorCode_removeImageOk {
		return fmt.Errorf("remove image failed: %s", payload.RemoveImageResponse.GetErrorcode().String())
	}
	return nil
}

func (c *TCPClient) GetVolume(ctx context.Context, ref VolumeRef) (Volume, error) {
	if err := ValidateAddress(c.address); err != nil {
		return Volume{}, err
	}
	if err := ref.Validate(); err != nil {
		return Volume{}, err
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_Get_ImageInfo_Request{
			Get_ImageInfo_Request: &msg.GetImageInfoRequest{
				Poolname:  ref.Pool,
				Imagename: ref.Name,
			},
		},
	})
	if err != nil {
		return Volume{}, err
	}
	payload, ok := resp.Union.(*msg.Response_GetImageInfoResponse)
	if !ok {
		return Volume{}, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if payload.GetImageInfoResponse.GetErrorcode() != msg.GetImageErrorCode_getImageOk {
		return Volume{}, fmt.Errorf("get image failed: %s", payload.GetImageInfoResponse.GetErrorcode().String())
	}
	return volumeFromImageInfo(payload.GetImageInfoResponse.GetImageInfo()), nil
}

func (c *TCPClient) ExpandVolume(ctx context.Context, ref VolumeRef, capacityBytes int64) (Volume, error) {
	if err := ValidateAddress(c.address); err != nil {
		return Volume{}, err
	}
	if err := ref.Validate(); err != nil {
		return Volume{}, err
	}
	if capacityBytes <= 0 {
		return Volume{}, fmt.Errorf("invalid capacity bytes %d", capacityBytes)
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_ResizeImageRequest{
			ResizeImageRequest: &msg.ResizeImageRequest{
				Poolname:  ref.Pool,
				Imagename: ref.Name,
				Size_:     capacityBytes,
			},
		},
	})
	if err != nil {
		return Volume{}, err
	}
	payload, ok := resp.Union.(*msg.Response_ResizeImageResponse)
	if !ok {
		return Volume{}, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if payload.ResizeImageResponse.GetErrorcode() != msg.ResizeImageErrorCode_resizeImageOk {
		return Volume{}, fmt.Errorf("resize image failed: %s", payload.ResizeImageResponse.GetErrorcode().String())
	}
	return volumeFromImageInfo(payload.ResizeImageResponse.GetImageInfo()), nil
}

func (c *TCPClient) PutVolumeMetadata(ctx context.Context, metadata VolumeMetadata) error {
	if err := ValidateAddress(c.address); err != nil {
		return err
	}
	if err := metadata.Validate(); err != nil {
		return err
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_PutCsiVolumeMetadataRequest{
			PutCsiVolumeMetadataRequest: &msg.PutCSIVolumeMetadataRequest{
				Metadata: volumeMetadataToProto(metadata),
			},
		},
	})
	if err != nil {
		return err
	}
	payload, ok := resp.Union.(*msg.Response_PutCsiVolumeMetadataResponse)
	if !ok {
		return fmt.Errorf("unexpected response type %T", resp.Union)
	}
	return metadataError(payload.PutCsiVolumeMetadataResponse.GetErrorcode())
}

func (c *TCPClient) GetVolumeMetadata(ctx context.Context, volumeID string) (VolumeMetadata, error) {
	if err := ValidateAddress(c.address); err != nil {
		return VolumeMetadata{}, err
	}
	if strings.TrimSpace(volumeID) == "" {
		return VolumeMetadata{}, fmt.Errorf("volume id is required")
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_GetCsiVolumeMetadataRequest{
			GetCsiVolumeMetadataRequest: &msg.GetCSIVolumeMetadataRequest{
				VolumeId: volumeID,
			},
		},
	})
	if err != nil {
		return VolumeMetadata{}, err
	}
	payload, ok := resp.Union.(*msg.Response_GetCsiVolumeMetadataResponse)
	if !ok {
		return VolumeMetadata{}, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if err := metadataError(payload.GetCsiVolumeMetadataResponse.GetErrorcode()); err != nil {
		return VolumeMetadata{}, err
	}
	return volumeMetadataFromProto(payload.GetCsiVolumeMetadataResponse.GetMetadata()), nil
}

func (c *TCPClient) DeleteVolumeMetadata(ctx context.Context, volumeID string) error {
	if err := ValidateAddress(c.address); err != nil {
		return err
	}
	if strings.TrimSpace(volumeID) == "" {
		return fmt.Errorf("volume id is required")
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_DeleteCsiVolumeMetadataRequest{
			DeleteCsiVolumeMetadataRequest: &msg.DeleteCSIVolumeMetadataRequest{
				VolumeId: volumeID,
			},
		},
	})
	if err != nil {
		return err
	}
	payload, ok := resp.Union.(*msg.Response_DeleteCsiVolumeMetadataResponse)
	if !ok {
		return fmt.Errorf("unexpected response type %T", resp.Union)
	}
	return metadataError(payload.DeleteCsiVolumeMetadataResponse.GetErrorcode())
}

func (c *TCPClient) ListVolumeMetadata(ctx context.Context) ([]VolumeMetadata, error) {
	if err := ValidateAddress(c.address); err != nil {
		return nil, err
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_ListCsiVolumeMetadataRequest{
			ListCsiVolumeMetadataRequest: &msg.ListCSIVolumeMetadataRequest{},
		},
	})
	if err != nil {
		return nil, err
	}
	payload, ok := resp.Union.(*msg.Response_ListCsiVolumeMetadataResponse)
	if !ok {
		return nil, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if err := metadataError(payload.ListCsiVolumeMetadataResponse.GetErrorcode()); err != nil {
		return nil, err
	}
	items := payload.ListCsiVolumeMetadataResponse.GetMetadata()
	result := make([]VolumeMetadata, 0, len(items))
	for _, item := range items {
		result = append(result, volumeMetadataFromProto(item))
	}
	return result, nil
}

func (c *TCPClient) PutAttachment(ctx context.Context, attachment Attachment) error {
	if err := ValidateAddress(c.address); err != nil {
		return err
	}
	if err := attachment.Validate(); err != nil {
		return err
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_PutCsiAttachmentRequest{
			PutCsiAttachmentRequest: &msg.PutCSIAttachmentRequest{
				Attachment: attachmentToProto(attachment),
			},
		},
	})
	if err != nil {
		return err
	}
	payload, ok := resp.Union.(*msg.Response_PutCsiAttachmentResponse)
	if !ok {
		return fmt.Errorf("unexpected response type %T", resp.Union)
	}
	return metadataError(payload.PutCsiAttachmentResponse.GetErrorcode())
}

func (c *TCPClient) GetAttachment(ctx context.Context, volumeID string) (Attachment, error) {
	if err := ValidateAddress(c.address); err != nil {
		return Attachment{}, err
	}
	if strings.TrimSpace(volumeID) == "" {
		return Attachment{}, fmt.Errorf("volume id is required")
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_GetCsiAttachmentRequest{
			GetCsiAttachmentRequest: &msg.GetCSIAttachmentRequest{
				VolumeId: volumeID,
			},
		},
	})
	if err != nil {
		return Attachment{}, err
	}
	payload, ok := resp.Union.(*msg.Response_GetCsiAttachmentResponse)
	if !ok {
		return Attachment{}, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if err := metadataError(payload.GetCsiAttachmentResponse.GetErrorcode()); err != nil {
		return Attachment{}, err
	}
	return attachmentFromProto(payload.GetCsiAttachmentResponse.GetAttachment()), nil
}

func (c *TCPClient) DeleteAttachment(ctx context.Context, volumeID string) error {
	if err := ValidateAddress(c.address); err != nil {
		return err
	}
	if strings.TrimSpace(volumeID) == "" {
		return fmt.Errorf("volume id is required")
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_DeleteCsiAttachmentRequest{
			DeleteCsiAttachmentRequest: &msg.DeleteCSIAttachmentRequest{
				VolumeId: volumeID,
			},
		},
	})
	if err != nil {
		return err
	}
	payload, ok := resp.Union.(*msg.Response_DeleteCsiAttachmentResponse)
	if !ok {
		return fmt.Errorf("unexpected response type %T", resp.Union)
	}
	return metadataError(payload.DeleteCsiAttachmentResponse.GetErrorcode())
}

func (c *TCPClient) ListAttachments(ctx context.Context) ([]Attachment, error) {
	if err := ValidateAddress(c.address); err != nil {
		return nil, err
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_ListCsiAttachmentRequest{
			ListCsiAttachmentRequest: &msg.ListCSIAttachmentRequest{},
		},
	})
	if err != nil {
		return nil, err
	}
	payload, ok := resp.Union.(*msg.Response_ListCsiAttachmentResponse)
	if !ok {
		return nil, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if err := metadataError(payload.ListCsiAttachmentResponse.GetErrorcode()); err != nil {
		return nil, err
	}
	items := payload.ListCsiAttachmentResponse.GetAttachments()
	result := make([]Attachment, 0, len(items))
	for _, item := range items {
		result = append(result, attachmentFromProto(item))
	}
	return result, nil
}

func (c *TCPClient) AcquireLease(ctx context.Context, lease Lease) (Lease, error) {
	if err := ValidateAddress(c.address); err != nil {
		return Lease{}, err
	}
	if err := lease.Validate(); err != nil {
		return Lease{}, err
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_AcquireCsiLeaseRequest{
			AcquireCsiLeaseRequest: &msg.AcquireCSILeaseRequest{
				VolumeId:   lease.VolumeID,
				NodeId:     lease.NodeID,
				HostNqn:    lease.HostNQN,
				TtlSeconds: lease.TTLSeconds,
			},
		},
	})
	if err != nil {
		return Lease{}, err
	}
	payload, ok := resp.Union.(*msg.Response_AcquireCsiLeaseResponse)
	if !ok {
		return Lease{}, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if err := leaseError(payload.AcquireCsiLeaseResponse.GetErrorcode()); err != nil {
		return Lease{}, err
	}
	return leaseFromProto(payload.AcquireCsiLeaseResponse.GetLease()), nil
}

func (c *TCPClient) GetLease(ctx context.Context, volumeID string) (Lease, error) {
	if err := ValidateAddress(c.address); err != nil {
		return Lease{}, err
	}
	if strings.TrimSpace(volumeID) == "" {
		return Lease{}, fmt.Errorf("volume id is required")
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_GetCsiLeaseRequest{
			GetCsiLeaseRequest: &msg.GetCSILeaseRequest{
				VolumeId: volumeID,
			},
		},
	})
	if err != nil {
		return Lease{}, err
	}
	payload, ok := resp.Union.(*msg.Response_GetCsiLeaseResponse)
	if !ok {
		return Lease{}, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if err := leaseError(payload.GetCsiLeaseResponse.GetErrorcode()); err != nil {
		return Lease{}, err
	}
	return leaseFromProto(payload.GetCsiLeaseResponse.GetLease()), nil
}

func (c *TCPClient) RenewLease(ctx context.Context, lease Lease) (Lease, error) {
	if err := ValidateAddress(c.address); err != nil {
		return Lease{}, err
	}
	if strings.TrimSpace(lease.VolumeID) == "" || strings.TrimSpace(lease.NodeID) == "" || strings.TrimSpace(lease.HostNQN) == "" {
		return Lease{}, fmt.Errorf("volume id, node id and host nqn are required")
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_RenewCsiLeaseRequest{
			RenewCsiLeaseRequest: &msg.RenewCSILeaseRequest{
				VolumeId: lease.VolumeID,
				NodeId:   lease.NodeID,
				HostNqn:  lease.HostNQN,
			},
		},
	})
	if err != nil {
		return Lease{}, err
	}
	payload, ok := resp.Union.(*msg.Response_RenewCsiLeaseResponse)
	if !ok {
		return Lease{}, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if err := leaseError(payload.RenewCsiLeaseResponse.GetErrorcode()); err != nil {
		return Lease{}, err
	}
	return leaseFromProto(payload.RenewCsiLeaseResponse.GetLease()), nil
}

func (c *TCPClient) ReleaseLease(ctx context.Context, lease Lease) error {
	if err := ValidateAddress(c.address); err != nil {
		return err
	}
	if strings.TrimSpace(lease.VolumeID) == "" || strings.TrimSpace(lease.NodeID) == "" || strings.TrimSpace(lease.HostNQN) == "" {
		return fmt.Errorf("volume id, node id and host nqn are required")
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_ReleaseCsiLeaseRequest{
			ReleaseCsiLeaseRequest: &msg.ReleaseCSILeaseRequest{
				VolumeId: lease.VolumeID,
				NodeId:   lease.NodeID,
				HostNqn:  lease.HostNQN,
			},
		},
	})
	if err != nil {
		return err
	}
	payload, ok := resp.Union.(*msg.Response_ReleaseCsiLeaseResponse)
	if !ok {
		return fmt.Errorf("unexpected response type %T", resp.Union)
	}
	return leaseError(payload.ReleaseCsiLeaseResponse.GetErrorcode())
}

func (c *TCPClient) ListLeases(ctx context.Context) ([]Lease, error) {
	if err := ValidateAddress(c.address); err != nil {
		return nil, err
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_ListCsiLeaseRequest{
			ListCsiLeaseRequest: &msg.ListCSILeaseRequest{},
		},
	})
	if err != nil {
		return nil, err
	}
	payload, ok := resp.Union.(*msg.Response_ListCsiLeaseResponse)
	if !ok {
		return nil, fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if err := leaseError(payload.ListCsiLeaseResponse.GetErrorcode()); err != nil {
		return nil, err
	}
	items := payload.ListCsiLeaseResponse.GetLeases()
	result := make([]Lease, 0, len(items))
	for _, item := range items {
		result = append(result, leaseFromProto(item))
	}
	return result, nil
}

func (c *TCPClient) AttachImage(ctx context.Context, ref VolumeRef, clientID, clientType string, leaseDurationSeconds int64) error {
	if err := ValidateAddress(c.address); err != nil {
		return err
	}
	if err := ref.Validate(); err != nil {
		return err
	}
	if strings.TrimSpace(clientID) == "" {
		return fmt.Errorf("client id is required")
	}
	if strings.TrimSpace(clientType) == "" {
		return fmt.Errorf("client type is required")
	}
	if leaseDurationSeconds <= 0 {
		return fmt.Errorf("invalid lease duration seconds %d", leaseDurationSeconds)
	}
	imageID, err := c.imageIDByRef(ctx, ref)
	if err != nil {
		return err
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_AttachImageRequest{
			AttachImageRequest: &msg.AttachImageRequest{
				ImageId:              imageID,
				ClientId:             clientID,
				ClientType:           clientType,
				LeaseDurationSeconds: leaseDurationSeconds,
			},
		},
	})
	if err != nil {
		return err
	}
	payload, ok := resp.Union.(*msg.Response_AttachImageResponse)
	if !ok {
		return fmt.Errorf("unexpected response type %T", resp.Union)
	}
	return imageMetadataError(payload.AttachImageResponse.GetErrorcode())
}

func (c *TCPClient) DetachImage(ctx context.Context, ref VolumeRef, clientID string) error {
	if err := ValidateAddress(c.address); err != nil {
		return err
	}
	if err := ref.Validate(); err != nil {
		return err
	}
	if strings.TrimSpace(clientID) == "" {
		return fmt.Errorf("client id is required")
	}
	imageID, err := c.imageIDByRef(ctx, ref)
	if err != nil {
		return err
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_DetachImageRequest{
			DetachImageRequest: &msg.DetachImageRequest{
				ImageId:  imageID,
				ClientId: clientID,
			},
		},
	})
	if err != nil {
		return err
	}
	payload, ok := resp.Union.(*msg.Response_DetachImageResponse)
	if !ok {
		return fmt.Errorf("unexpected response type %T", resp.Union)
	}
	return imageMetadataError(payload.DetachImageResponse.GetErrorcode())
}

func (c *TCPClient) RenewImageLease(ctx context.Context, ref VolumeRef, clientID string, leaseDurationSeconds int64) error {
	if err := ValidateAddress(c.address); err != nil {
		return err
	}
	if err := ref.Validate(); err != nil {
		return err
	}
	if strings.TrimSpace(clientID) == "" {
		return fmt.Errorf("client id is required")
	}
	if leaseDurationSeconds <= 0 {
		return fmt.Errorf("invalid lease duration seconds %d", leaseDurationSeconds)
	}
	imageID, err := c.imageIDByRef(ctx, ref)
	if err != nil {
		return err
	}
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_RenewImageLeaseRequest{
			RenewImageLeaseRequest: &msg.RenewImageLeaseRequest{
				ImageId:              imageID,
				ClientId:             clientID,
				LeaseDurationSeconds: leaseDurationSeconds,
			},
		},
	})
	if err != nil {
		return err
	}
	payload, ok := resp.Union.(*msg.Response_RenewImageLeaseResponse)
	if !ok {
		return fmt.Errorf("unexpected response type %T", resp.Union)
	}
	return imageMetadataError(payload.RenewImageLeaseResponse.GetErrorcode())
}

func (c *TCPClient) roundTrip(ctx context.Context, req *msg.Request) (*msg.Response, error) {
	conn, err := c.dialer.DialContext(ctx, "tcp", c.address)
	if err != nil {
		return nil, err
	}
	defer conn.Close()
	if deadline, ok := ctx.Deadline(); ok {
		_ = conn.SetDeadline(deadline)
	}

	data, err := proto.Marshal(req)
	if err != nil {
		return nil, err
	}
	header := make([]byte, messageLengthSize)
	binary.LittleEndian.PutUint64(header, uint64(len(data)))
	if _, err := conn.Write(append(header, data...)); err != nil {
		return nil, err
	}

	if _, err := io.ReadFull(conn, header); err != nil {
		return nil, err
	}
	size := binary.LittleEndian.Uint64(header)
	body := make([]byte, size)
	if _, err := io.ReadFull(conn, body); err != nil {
		return nil, err
	}

	resp := &msg.Response{}
	if err := proto.Unmarshal(body, resp); err != nil {
		return nil, err
	}
	return resp, nil
}

func (c *TCPClient) imageIDByRef(ctx context.Context, ref VolumeRef) (string, error) {
	resp, err := c.roundTrip(ctx, &msg.Request{
		Union: &msg.Request_GetImageMetadataByNameRequest{
			GetImageMetadataByNameRequest: &msg.GetImageMetadataByNameRequest{
				PoolName:  ref.Pool,
				ImageName: ref.Name,
			},
		},
	})
	if err != nil {
		return "", err
	}
	payload, ok := resp.Union.(*msg.Response_GetImageMetadataByNameResponse)
	if !ok {
		return "", fmt.Errorf("unexpected response type %T", resp.Union)
	}
	if err := imageMetadataError(payload.GetImageMetadataByNameResponse.GetErrorcode()); err != nil {
		return "", err
	}
	metadata := payload.GetImageMetadataByNameResponse.GetMetadata()
	if metadata == nil || strings.TrimSpace(metadata.GetImageId()) == "" {
		return "", fmt.Errorf("monitor returned empty image metadata for %s/%s", ref.Pool, ref.Name)
	}
	return metadata.GetImageId(), nil
}

func volumeFromImageInfo(info *msg.ImageInfo) Volume {
	if info == nil {
		return Volume{}
	}
	encodedID, _ := volumeid.EncodeNameRef(volumeid.NameRef{
		Pool: info.GetPoolname(),
		Name: info.GetImagename(),
	})
	return Volume{
		ID:            encodedID,
		Name:          info.GetImagename(),
		Pool:          info.GetPoolname(),
		CapacityBytes: info.GetSize_(),
		ObjectSize:    info.GetObjectSize(),
	}
}

func volumeMetadataToProto(metadata VolumeMetadata) *msg.CSIVolumeMetadata {
	return &msg.CSIVolumeMetadata{
		VolumeId:      metadata.Volume.ID,
		PoolName:      metadata.Volume.Pool,
		ImageName:     metadata.Volume.Name,
		CapacityBytes: metadata.Volume.CapacityBytes,
		ObjectSize:    metadata.Volume.ObjectSize,
		BlockSize:     metadata.BlockSize,
		Transport:     metadata.Transport,
		ExportId:      metadata.ExportID,
	}
}

func volumeMetadataFromProto(metadata *msg.CSIVolumeMetadata) VolumeMetadata {
	if metadata == nil {
		return VolumeMetadata{}
	}
	return VolumeMetadata{
		Volume: Volume{
			ID:            metadata.GetVolumeId(),
			Name:          metadata.GetImageName(),
			Pool:          metadata.GetPoolName(),
			CapacityBytes: metadata.GetCapacityBytes(),
			ObjectSize:    metadata.GetObjectSize(),
		},
		BlockSize: metadata.GetBlockSize(),
		Transport: metadata.GetTransport(),
		ExportID:  metadata.GetExportId(),
	}
}

func attachmentToProto(attachment Attachment) *msg.CSIAttachment {
	return &msg.CSIAttachment{
		VolumeId: attachment.VolumeID,
		NodeId:   attachment.NodeID,
		HostNqn:  attachment.HostNQN,
		ExportId: attachment.ExportID,
	}
}

func attachmentFromProto(attachment *msg.CSIAttachment) Attachment {
	if attachment == nil {
		return Attachment{}
	}
	return Attachment{
		VolumeID: attachment.GetVolumeId(),
		NodeID:   attachment.GetNodeId(),
		HostNQN:  attachment.GetHostNqn(),
		ExportID: attachment.GetExportId(),
	}
}

func leaseFromProto(lease *msg.CSIVolumeLease) Lease {
	if lease == nil {
		return Lease{}
	}
	return Lease{
		VolumeID:   lease.GetVolumeId(),
		NodeID:     lease.GetNodeId(),
		HostNQN:    lease.GetHostNqn(),
		LeaseID:    lease.GetLeaseId(),
		TTLSeconds: lease.GetTtlSeconds(),
	}
}

func metadataError(code msg.CSIMetadataErrorCode) error {
	switch code {
	case msg.CSIMetadataErrorCode_csiMetadataOk:
		return nil
	case msg.CSIMetadataErrorCode_csiMetadataNotFound:
		return ErrMetadataNotFound
	case msg.CSIMetadataErrorCode_csiMetadataInvalidArgument:
		return fmt.Errorf("monitor metadata invalid argument")
	default:
		return fmt.Errorf("monitor metadata operation failed: %s", code.String())
	}
}

func leaseError(code msg.CSILeaseErrorCode) error {
	switch code {
	case msg.CSILeaseErrorCode_csiLeaseOk:
		return nil
	case msg.CSILeaseErrorCode_csiLeaseNotFound:
		return ErrLeaseNotFound
	case msg.CSILeaseErrorCode_csiLeaseConflict:
		return ErrLeaseConflict
	case msg.CSILeaseErrorCode_csiLeaseInvalidArgument:
		return fmt.Errorf("monitor lease invalid argument")
	default:
		return fmt.Errorf("monitor lease operation failed: %s", code.String())
	}
}

func imageMetadataError(code msg.ImageMetadataErrorCode) error {
	switch code {
	case msg.ImageMetadataErrorCode_imageMetadataOk:
		return nil
	case msg.ImageMetadataErrorCode_imageMetadataNotFound:
		return ErrImageNotFound
	case msg.ImageMetadataErrorCode_imageMetadataInvalidArgument:
		return fmt.Errorf("monitor image metadata invalid argument")
	default:
		return fmt.Errorf("monitor image metadata operation failed: %s", code.String())
	}
}
