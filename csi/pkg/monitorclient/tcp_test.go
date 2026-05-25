package monitorclient

import (
	"context"
	"encoding/binary"
	"io"
	"net"
	"testing"

	msg "monitor/msg"

	"github.com/gogo/protobuf/proto"
)

func TestCreateVolume(t *testing.T) {
	address := startMockMonitor(t, func(req *msg.Request) *msg.Response {
		payload, ok := req.Union.(*msg.Request_CreateImageRequest)
		if !ok {
			t.Fatalf("unexpected request type %T", req.Union)
		}
		if payload.CreateImageRequest.GetImagename() != "img-a" {
			t.Fatalf("unexpected image name: %s", payload.CreateImageRequest.GetImagename())
		}
		return &msg.Response{
			Union: &msg.Response_CreateImageResponse{
				CreateImageResponse: &msg.CreateImageResponse{
					Errorcode: msg.CreateImageErrorCode_createImageOk,
					ImageInfo: &msg.ImageInfo{
						Poolname:   "fb",
						Imagename:  "img-a",
						Size_:      1 << 20,
						ObjectSize: 4 << 20,
					},
				},
			},
		}
	})

	client := NewTCP(address)
	volume, err := client.CreateVolume(context.Background(), CreateVolumeRequest{
		Name:          "img-a",
		Pool:          "fb",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
		BlockSize:     4096,
	})
	if err != nil {
		t.Fatalf("create volume failed: %v", err)
	}
	if volume.Name != "img-a" || volume.Pool != "fb" || volume.ID == "" {
		t.Fatalf("unexpected volume: %+v", volume)
	}
}

func TestGetVolume(t *testing.T) {
	address := startMockMonitor(t, func(req *msg.Request) *msg.Response {
		if _, ok := req.Union.(*msg.Request_Get_ImageInfo_Request); !ok {
			t.Fatalf("unexpected request type %T", req.Union)
		}
		return &msg.Response{
			Union: &msg.Response_GetImageInfoResponse{
				GetImageInfoResponse: &msg.GetImageInfoResponse{
					Errorcode: msg.GetImageErrorCode_getImageOk,
					ImageInfo: &msg.ImageInfo{
						Poolname:   "fb",
						Imagename:  "img-b",
						Size_:      2 << 20,
						ObjectSize: 4 << 20,
					},
				},
			},
		}
	})

	client := NewTCP(address)
	volume, err := client.GetVolume(context.Background(), VolumeRef{Name: "img-b", Pool: "fb"})
	if err != nil {
		t.Fatalf("get volume failed: %v", err)
	}
	if volume.CapacityBytes != 2<<20 {
		t.Fatalf("unexpected volume: %+v", volume)
	}
}

func TestInputValidation(t *testing.T) {
	client := NewTCP("127.0.0.1:3333")
	if _, err := client.CreateVolume(context.Background(), CreateVolumeRequest{}); err == nil {
		t.Fatal("expected create validation error")
	}
	if err := client.DeleteVolume(context.Background(), VolumeRef{}); err == nil {
		t.Fatal("expected delete validation error")
	}
	if _, err := client.ExpandVolume(context.Background(), VolumeRef{Name: "img-a", Pool: "fb"}, 0); err == nil {
		t.Fatal("expected expand validation error")
	}
}

func TestDeleteVolumeTreatsImageNotFoundAsSuccess(t *testing.T) {
	address := startMockMonitor(t, func(req *msg.Request) *msg.Response {
		if _, ok := req.Union.(*msg.Request_RemoveImageRequest); !ok {
			t.Fatalf("unexpected request type %T", req.Union)
		}
		return &msg.Response{
			Union: &msg.Response_RemoveImageResponse{
				RemoveImageResponse: &msg.RemoveImageResponse{
					Errorcode: msg.RemoveImageErrorCode_imageNotFound,
				},
			},
		}
	})

	client := NewTCP(address)
	if err := client.DeleteVolume(context.Background(), VolumeRef{Name: "img-missing", Pool: "fb"}); err != nil {
		t.Fatalf("delete volume should ignore missing image: %v", err)
	}
}

func TestVolumeRefHelper(t *testing.T) {
	ref := (Volume{ID: "fbvol:cluster:1:2", Name: "img-a", Pool: "fb"}).Ref()
	if ref.ID != "fbvol:cluster:1:2" || ref.Name != "img-a" || ref.Pool != "fb" {
		t.Fatalf("unexpected ref: %+v", ref)
	}
}

func TestValidateAddress(t *testing.T) {
	if err := ValidateAddress("127.0.0.1:3333"); err != nil {
		t.Fatalf("unexpected address validation error: %v", err)
	}
	if err := ValidateAddress(""); err == nil {
		t.Fatal("expected empty address validation error")
	}
	if err := ValidateAddress("bad-address"); err == nil {
		t.Fatal("expected malformed address validation error")
	}
}

func startMockMonitor(t *testing.T, handler func(*msg.Request) *msg.Response) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen failed: %v", err)
	}
	t.Cleanup(func() { _ = ln.Close() })

	go func() {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		defer conn.Close()

		header := make([]byte, messageLengthSize)
		if _, err := io.ReadFull(conn, header); err != nil {
			return
		}
		size := binary.LittleEndian.Uint64(header)
		body := make([]byte, size)
		if _, err := io.ReadFull(conn, body); err != nil {
			return
		}
		req := &msg.Request{}
		if err := proto.Unmarshal(body, req); err != nil {
			return
		}

		respBody, err := proto.Marshal(handler(req))
		if err != nil {
			return
		}
		binary.LittleEndian.PutUint64(header, uint64(len(respBody)))
		_, _ = conn.Write(append(header, respBody...))
	}()

	return ln.Addr().String()
}
