package monitorclient

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"net"

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
