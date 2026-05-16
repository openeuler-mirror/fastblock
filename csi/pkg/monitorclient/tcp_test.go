package monitorclient

import (
	"context"
	"encoding/binary"
	"errors"
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

func TestCreateVolumeTreatsImageExistsAsIdempotent(t *testing.T) {
	address := startMockMonitorSequence(t, func(call int, req *msg.Request) *msg.Response {
		switch call {
		case 0:
			if _, ok := req.Union.(*msg.Request_CreateImageRequest); !ok {
				t.Fatalf("unexpected first request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_CreateImageResponse{
					CreateImageResponse: &msg.CreateImageResponse{
						Errorcode: msg.CreateImageErrorCode_imageExists,
					},
				},
			}
		case 1:
			if _, ok := req.Union.(*msg.Request_Get_ImageInfo_Request); !ok {
				t.Fatalf("unexpected second request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_GetImageInfoResponse{
					GetImageInfoResponse: &msg.GetImageInfoResponse{
						Errorcode: msg.GetImageErrorCode_getImageOk,
						ImageInfo: &msg.ImageInfo{
							Poolname:   "fb",
							Imagename:  "img-a",
							Size_:      1 << 20,
							ObjectSize: 4 << 20,
						},
					},
				},
			}
		default:
			t.Fatalf("unexpected call index %d", call)
			return nil
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
		t.Fatalf("create volume idempotency failed: %v", err)
	}
	if volume.Name != "img-a" || volume.Pool != "fb" || volume.ID == "" {
		t.Fatalf("unexpected volume: %+v", volume)
	}
}

func TestCreateVolumeRejectsIncompatibleExistingImage(t *testing.T) {
	address := startMockMonitorSequence(t, func(call int, req *msg.Request) *msg.Response {
		switch call {
		case 0:
			if _, ok := req.Union.(*msg.Request_CreateImageRequest); !ok {
				t.Fatalf("unexpected first request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_CreateImageResponse{
					CreateImageResponse: &msg.CreateImageResponse{
						Errorcode: msg.CreateImageErrorCode_imageExists,
					},
				},
			}
		case 1:
			if _, ok := req.Union.(*msg.Request_Get_ImageInfo_Request); !ok {
				t.Fatalf("unexpected second request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_GetImageInfoResponse{
					GetImageInfoResponse: &msg.GetImageInfoResponse{
						Errorcode: msg.GetImageErrorCode_getImageOk,
						ImageInfo: &msg.ImageInfo{
							Poolname:   "fb",
							Imagename:  "img-a",
							Size_:      512 << 10,
							ObjectSize: 4 << 20,
						},
					},
				},
			}
		default:
			t.Fatalf("unexpected call index %d", call)
			return nil
		}
	})

	client := NewTCP(address)
	if _, err := client.CreateVolume(context.Background(), CreateVolumeRequest{
		Name:          "img-a",
		Pool:          "fb",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
		BlockSize:     4096,
	}); err == nil {
		t.Fatal("expected incompatible existing image to fail")
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

func TestCreateSnapshotTreatsExistingSnapshotAsIdempotent(t *testing.T) {
	address := startMockMonitorSequence(t, func(call int, req *msg.Request) *msg.Response {
		switch call {
		case 0:
			if _, ok := req.Union.(*msg.Request_CreateImageSnapshotRequest); !ok {
				t.Fatalf("unexpected create snapshot request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_CreateImageSnapshotResponse{
					CreateImageSnapshotResponse: &msg.CreateImageSnapshotResponse{
						Errorcode: msg.ImageMetadataErrorCode_imageMetadataInvalidArgument,
					},
				},
			}
		case 1:
			payload, ok := req.Union.(*msg.Request_GetSnapshotIdByNameRequest)
			if !ok {
				t.Fatalf("unexpected get snapshot id request type %T", req.Union)
			}
			if payload.GetSnapshotIdByNameRequest.GetPoolName() != "fb" ||
				payload.GetSnapshotIdByNameRequest.GetImageName() != "img-a" ||
				payload.GetSnapshotIdByNameRequest.GetSnapshotName() != "snap-a" {
				t.Fatalf("unexpected get snapshot id request: %+v", payload.GetSnapshotIdByNameRequest)
			}
			return &msg.Response{
				Union: &msg.Response_GetSnapshotIdByNameResponse{
					GetSnapshotIdByNameResponse: &msg.GetSnapshotIDByNameResponse{
						Errorcode:  msg.ImageMetadataErrorCode_imageMetadataOk,
						SnapshotId: "snap-a",
					},
				},
			}
		case 2:
			if _, ok := req.Union.(*msg.Request_GetSnapshotMetadataByIdRequest); !ok {
				t.Fatalf("unexpected get snapshot request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_GetSnapshotMetadataByIdResponse{
					GetSnapshotMetadataByIdResponse: &msg.GetSnapshotMetadataByIDResponse{
						Errorcode: msg.ImageMetadataErrorCode_imageMetadataOk,
						Metadata: &msg.SnapshotMetadataV2{
							SnapshotId:      "snap-a",
							SnapshotName:    "snap-a",
							SourcePoolName:  "fb",
							SourceImageName: "img-a",
							Status:          "ready",
						},
					},
				},
			}
		case 3:
			if _, ok := req.Union.(*msg.Request_GetImageMetadataByNameRequest); !ok {
				t.Fatalf("unexpected get image metadata request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_GetImageMetadataByNameResponse{
					GetImageMetadataByNameResponse: &msg.GetImageMetadataByNameResponse{
						Errorcode: msg.ImageMetadataErrorCode_imageMetadataOk,
						Metadata: &msg.ImageMetadataV2{
							ImageId:   "img-1",
							PoolName:  "fb",
							ImageName: "img-a",
							Size_:     1 << 20,
						},
					},
				},
			}
		default:
			t.Fatalf("unexpected call index %d", call)
			return nil
		}
	})

	client := NewTCP(address)
	snapshot, err := client.CreateSnapshot(context.Background(), CreateSnapshotRequest{
		Name:         "snap-a",
		SourceVolume: VolumeRef{ID: "fbvolname:fb:img-a", Pool: "fb", Name: "img-a"},
	})
	if err != nil {
		t.Fatalf("create snapshot idempotency failed: %v", err)
	}
	if snapshot.ID != "snap-a" || snapshot.SourceVolume.Name != "img-a" || snapshot.SizeBytes != 1<<20 {
		t.Fatalf("unexpected snapshot: %+v", snapshot)
	}
}

func TestListSnapshotsFiltersDeletedEntries(t *testing.T) {
	address := startMockMonitorSequence(t, func(call int, req *msg.Request) *msg.Response {
		switch call {
		case 0:
			if _, ok := req.Union.(*msg.Request_ListImageMetadataRequest); !ok {
				t.Fatalf("unexpected list image metadata request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_ListImageMetadataResponse{
					ListImageMetadataResponse: &msg.ListImageMetadataResponse{
						Errorcode: msg.ImageMetadataErrorCode_imageMetadataOk,
						Metadata: []*msg.ImageMetadataV2{{
							ImageId:   "img-1",
							PoolName:  "fb",
							ImageName: "img-a",
							Size_:     1 << 20,
						}},
					},
				},
			}
		case 1:
			payload, ok := req.Union.(*msg.Request_ListSnapshotMetadataRequest)
			if !ok {
				t.Fatalf("unexpected list snapshot request type %T", req.Union)
			}
			if payload.ListSnapshotMetadataRequest.GetImageId() != "img-1" {
				t.Fatalf("unexpected list snapshot request: %+v", payload.ListSnapshotMetadataRequest)
			}
			return &msg.Response{
				Union: &msg.Response_ListSnapshotMetadataResponse{
					ListSnapshotMetadataResponse: &msg.ListSnapshotMetadataResponse{
						Errorcode: msg.ImageMetadataErrorCode_imageMetadataOk,
						Metadata: []*msg.SnapshotMetadataV2{
							{
								SnapshotId:      "snap-a",
								SnapshotName:    "snap-a",
								SourcePoolName:  "fb",
								SourceImageName: "img-a",
								Status:          "ready",
							},
							{
								SnapshotId:      "snap-deleted",
								SnapshotName:    "snap-deleted",
								SourcePoolName:  "fb",
								SourceImageName: "img-a",
								Status:          "deleted_pending_gc",
							},
						},
					},
				},
			}
		default:
			t.Fatalf("unexpected call index %d", call)
			return nil
		}
	})

	client := NewTCP(address)
	items, err := client.ListSnapshots(context.Background(), ListSnapshotsRequest{})
	if err != nil {
		t.Fatalf("list snapshots failed: %v", err)
	}
	if len(items) != 1 || items[0].ID != "snap-a" {
		t.Fatalf("unexpected snapshot list: %+v", items)
	}
}

func TestCreateVolumeFromSnapshot(t *testing.T) {
	address := startMockMonitorSequence(t, func(call int, req *msg.Request) *msg.Response {
		switch call {
		case 0:
			if _, ok := req.Union.(*msg.Request_GetSnapshotMetadataByIdRequest); !ok {
				t.Fatalf("unexpected get snapshot request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_GetSnapshotMetadataByIdResponse{
					GetSnapshotMetadataByIdResponse: &msg.GetSnapshotMetadataByIDResponse{
						Errorcode: msg.ImageMetadataErrorCode_imageMetadataOk,
						Metadata: &msg.SnapshotMetadataV2{
							SnapshotId:      "snap-a",
							SnapshotName:    "snap-a",
							SourcePoolName:  "fb",
							SourceImageName: "img-a",
							Status:          "ready",
						},
					},
				},
			}
		case 1:
			if _, ok := req.Union.(*msg.Request_GetImageMetadataByNameRequest); !ok {
				t.Fatalf("unexpected get image metadata request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_GetImageMetadataByNameResponse{
					GetImageMetadataByNameResponse: &msg.GetImageMetadataByNameResponse{
						Errorcode: msg.ImageMetadataErrorCode_imageMetadataOk,
						Metadata: &msg.ImageMetadataV2{
							ImageId:   "img-1",
							PoolName:  "fb",
							ImageName: "img-a",
							Size_:     1 << 20,
						},
					},
				},
			}
		case 2:
			payload, ok := req.Union.(*msg.Request_ProtectSnapshotRequest)
			if !ok {
				t.Fatalf("unexpected protect snapshot request type %T", req.Union)
			}
			if payload.ProtectSnapshotRequest.GetSnapshotId() != "snap-a" {
				t.Fatalf("unexpected protect snapshot request: %+v", payload.ProtectSnapshotRequest)
			}
			return &msg.Response{
				Union: &msg.Response_ProtectSnapshotResponse{
					ProtectSnapshotResponse: &msg.ProtectSnapshotResponse{
						Errorcode: msg.ImageMetadataErrorCode_imageMetadataOk,
					},
				},
			}
		case 3:
			payload, ok := req.Union.(*msg.Request_CreateCloneFromSnapshotRequest)
			if !ok {
				t.Fatalf("unexpected clone request type %T", req.Union)
			}
			if payload.CreateCloneFromSnapshotRequest.GetSnapshotId() != "snap-a" ||
				payload.CreateCloneFromSnapshotRequest.GetCloneImageName() != "img-clone" {
				t.Fatalf("unexpected clone request: %+v", payload.CreateCloneFromSnapshotRequest)
			}
			return &msg.Response{
				Union: &msg.Response_CreateCloneFromSnapshotResponse{
					CreateCloneFromSnapshotResponse: &msg.CreateCloneFromSnapshotResponse{
						Errorcode: msg.ImageMetadataErrorCode_imageMetadataOk,
						Metadata: &msg.ImageMetadataV2{
							ImageId:          "img-clone-id",
							PoolName:         "fb",
							ImageName:        "img-clone",
							Size_:            1 << 20,
							ObjectSize:       4 << 20,
							ParentSnapshotId: "snap-a",
						},
					},
				},
			}
		default:
			t.Fatalf("unexpected call index %d", call)
			return nil
		}
	})

	client := NewTCP(address)
	volume, err := client.CreateVolumeFromSnapshot(context.Background(), CreateVolumeFromSnapshotRequest{
		Name:          "img-clone",
		Pool:          "fb",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
		BlockSize:     4096,
		SnapshotID:    "snap-a",
	})
	if err != nil {
		t.Fatalf("create volume from snapshot failed: %v", err)
	}
	if volume.Name != "img-clone" || volume.Pool != "fb" || volume.ObjectSize != 4<<20 || volume.ID == "" {
		t.Fatalf("unexpected restored volume: %+v", volume)
	}
}

func TestVolumeMetadataCRUD(t *testing.T) {
	address := startMockMonitorSequence(t, func(call int, req *msg.Request) *msg.Response {
		switch call {
		case 0:
			payload, ok := req.Union.(*msg.Request_PutCsiVolumeMetadataRequest)
			if !ok {
				t.Fatalf("unexpected put request type %T", req.Union)
			}
			if payload.PutCsiVolumeMetadataRequest.GetMetadata().GetVolumeId() != "vol-1" {
				t.Fatalf("unexpected metadata payload: %+v", payload.PutCsiVolumeMetadataRequest.GetMetadata())
			}
			return &msg.Response{
				Union: &msg.Response_PutCsiVolumeMetadataResponse{
					PutCsiVolumeMetadataResponse: &msg.PutCSIVolumeMetadataResponse{
						Errorcode: msg.CSIMetadataErrorCode_csiMetadataOk,
					},
				},
			}
		case 1:
			if _, ok := req.Union.(*msg.Request_GetCsiVolumeMetadataRequest); !ok {
				t.Fatalf("unexpected get request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_GetCsiVolumeMetadataResponse{
					GetCsiVolumeMetadataResponse: &msg.GetCSIVolumeMetadataResponse{
						Errorcode: msg.CSIMetadataErrorCode_csiMetadataOk,
						Metadata: &msg.CSIVolumeMetadata{
							VolumeId:      "vol-1",
							PoolName:      "fb",
							ImageName:     "img-a",
							CapacityBytes: 1 << 20,
							ObjectSize:    4 << 20,
							BlockSize:     4096,
							Transport:     "rdma",
							ExportId:      "exp-1",
						},
					},
				},
			}
		case 2:
			if _, ok := req.Union.(*msg.Request_DeleteCsiVolumeMetadataRequest); !ok {
				t.Fatalf("unexpected delete request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_DeleteCsiVolumeMetadataResponse{
					DeleteCsiVolumeMetadataResponse: &msg.DeleteCSIVolumeMetadataResponse{
						Errorcode: msg.CSIMetadataErrorCode_csiMetadataOk,
					},
				},
			}
		default:
			t.Fatalf("unexpected call index %d", call)
			return nil
		}
	})

	client := NewTCP(address)
	metadata := VolumeMetadata{
		Volume: Volume{
			ID:            "vol-1",
			Name:          "img-a",
			Pool:          "fb",
			CapacityBytes: 1 << 20,
			ObjectSize:    4 << 20,
		},
		BlockSize: 4096,
		Transport: "rdma",
		ExportID:  "exp-1",
	}
	if err := client.PutVolumeMetadata(context.Background(), metadata); err != nil {
		t.Fatalf("put volume metadata failed: %v", err)
	}
	got, err := client.GetVolumeMetadata(context.Background(), "vol-1")
	if err != nil {
		t.Fatalf("get volume metadata failed: %v", err)
	}
	if got.ExportID != "exp-1" || got.Volume.Name != "img-a" {
		t.Fatalf("unexpected volume metadata: %+v", got)
	}
	if err := client.DeleteVolumeMetadata(context.Background(), "vol-1"); err != nil {
		t.Fatalf("delete volume metadata failed: %v", err)
	}
}

func TestListVolumeMetadata(t *testing.T) {
	address := startMockMonitor(t, func(req *msg.Request) *msg.Response {
		if _, ok := req.Union.(*msg.Request_ListCsiVolumeMetadataRequest); !ok {
			t.Fatalf("unexpected request type %T", req.Union)
		}
		return &msg.Response{
			Union: &msg.Response_ListCsiVolumeMetadataResponse{
				ListCsiVolumeMetadataResponse: &msg.ListCSIVolumeMetadataResponse{
					Errorcode: msg.CSIMetadataErrorCode_csiMetadataOk,
					Metadata: []*msg.CSIVolumeMetadata{{
						VolumeId:      "vol-1",
						PoolName:      "fb",
						ImageName:     "img-a",
						CapacityBytes: 1 << 20,
						ObjectSize:    4 << 20,
						BlockSize:     4096,
						Transport:     "rdma",
					}},
				},
			},
		}
	})

	client := NewTCP(address)
	items, err := client.ListVolumeMetadata(context.Background())
	if err != nil {
		t.Fatalf("list volume metadata failed: %v", err)
	}
	if len(items) != 1 || items[0].Volume.ID != "vol-1" {
		t.Fatalf("unexpected metadata list: %+v", items)
	}
}

func TestAttachmentCRUD(t *testing.T) {
	address := startMockMonitorSequence(t, func(call int, req *msg.Request) *msg.Response {
		switch call {
		case 0:
			if _, ok := req.Union.(*msg.Request_PutCsiAttachmentRequest); !ok {
				t.Fatalf("unexpected put request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_PutCsiAttachmentResponse{
					PutCsiAttachmentResponse: &msg.PutCSIAttachmentResponse{
						Errorcode: msg.CSIMetadataErrorCode_csiMetadataOk,
					},
				},
			}
		case 1:
			if _, ok := req.Union.(*msg.Request_GetCsiAttachmentRequest); !ok {
				t.Fatalf("unexpected get request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_GetCsiAttachmentResponse{
					GetCsiAttachmentResponse: &msg.GetCSIAttachmentResponse{
						Errorcode: msg.CSIMetadataErrorCode_csiMetadataOk,
						Attachment: &msg.CSIAttachment{
							VolumeId: "vol-1",
							NodeId:   "node-a",
							HostNqn:  "nqn.host.1",
							ExportId: "exp-1",
						},
					},
				},
			}
		case 2:
			if _, ok := req.Union.(*msg.Request_DeleteCsiAttachmentRequest); !ok {
				t.Fatalf("unexpected delete request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_DeleteCsiAttachmentResponse{
					DeleteCsiAttachmentResponse: &msg.DeleteCSIAttachmentResponse{
						Errorcode: msg.CSIMetadataErrorCode_csiMetadataOk,
					},
				},
			}
		default:
			t.Fatalf("unexpected call index %d", call)
			return nil
		}
	})

	client := NewTCP(address)
	attachment := Attachment{
		VolumeID: "vol-1",
		NodeID:   "node-a",
		HostNQN:  "nqn.host.1",
		ExportID: "exp-1",
	}
	if err := client.PutAttachment(context.Background(), attachment); err != nil {
		t.Fatalf("put attachment failed: %v", err)
	}
	got, err := client.GetAttachment(context.Background(), "vol-1")
	if err != nil {
		t.Fatalf("get attachment failed: %v", err)
	}
	if got.NodeID != "node-a" || got.ExportID != "exp-1" {
		t.Fatalf("unexpected attachment: %+v", got)
	}
	if err := client.DeleteAttachment(context.Background(), "vol-1"); err != nil {
		t.Fatalf("delete attachment failed: %v", err)
	}
}

func TestListAttachments(t *testing.T) {
	address := startMockMonitor(t, func(req *msg.Request) *msg.Response {
		if _, ok := req.Union.(*msg.Request_ListCsiAttachmentRequest); !ok {
			t.Fatalf("unexpected request type %T", req.Union)
		}
		return &msg.Response{
			Union: &msg.Response_ListCsiAttachmentResponse{
				ListCsiAttachmentResponse: &msg.ListCSIAttachmentResponse{
					Errorcode: msg.CSIMetadataErrorCode_csiMetadataOk,
					Attachments: []*msg.CSIAttachment{{
						VolumeId: "vol-1",
						NodeId:   "node-a",
						HostNqn:  "nqn.host.1",
						ExportId: "exp-1",
					}},
				},
			},
		}
	})

	client := NewTCP(address)
	items, err := client.ListAttachments(context.Background())
	if err != nil {
		t.Fatalf("list attachments failed: %v", err)
	}
	if len(items) != 1 || items[0].VolumeID != "vol-1" {
		t.Fatalf("unexpected attachments list: %+v", items)
	}
}

func TestMetadataGetTreatsNotFoundAsDedicatedError(t *testing.T) {
	address := startMockMonitor(t, func(req *msg.Request) *msg.Response {
		if _, ok := req.Union.(*msg.Request_GetCsiVolumeMetadataRequest); !ok {
			t.Fatalf("unexpected request type %T", req.Union)
		}
		return &msg.Response{
			Union: &msg.Response_GetCsiVolumeMetadataResponse{
				GetCsiVolumeMetadataResponse: &msg.GetCSIVolumeMetadataResponse{
					Errorcode: msg.CSIMetadataErrorCode_csiMetadataNotFound,
				},
			},
		}
	})

	client := NewTCP(address)
	if _, err := client.GetVolumeMetadata(context.Background(), "missing"); !errors.Is(err, ErrMetadataNotFound) {
		t.Fatalf("expected metadata not found, got %v", err)
	}
}

func TestLeaseCRUD(t *testing.T) {
	address := startMockMonitorSequence(t, func(call int, req *msg.Request) *msg.Response {
		switch call {
		case 0:
			if _, ok := req.Union.(*msg.Request_AcquireCsiLeaseRequest); !ok {
				t.Fatalf("unexpected acquire request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_AcquireCsiLeaseResponse{
					AcquireCsiLeaseResponse: &msg.AcquireCSILeaseResponse{
						Errorcode: msg.CSILeaseErrorCode_csiLeaseOk,
						Lease: &msg.CSIVolumeLease{
							VolumeId:   "vol-1",
							NodeId:     "node-a",
							HostNqn:    "nqn.host.1",
							LeaseId:    7,
							TtlSeconds: 30,
						},
					},
				},
			}
		case 1:
			if _, ok := req.Union.(*msg.Request_GetCsiLeaseRequest); !ok {
				t.Fatalf("unexpected get request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_GetCsiLeaseResponse{
					GetCsiLeaseResponse: &msg.GetCSILeaseResponse{
						Errorcode: msg.CSILeaseErrorCode_csiLeaseOk,
						Lease: &msg.CSIVolumeLease{
							VolumeId:   "vol-1",
							NodeId:     "node-a",
							HostNqn:    "nqn.host.1",
							LeaseId:    7,
							TtlSeconds: 30,
						},
					},
				},
			}
		case 2:
			if _, ok := req.Union.(*msg.Request_RenewCsiLeaseRequest); !ok {
				t.Fatalf("unexpected renew request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_RenewCsiLeaseResponse{
					RenewCsiLeaseResponse: &msg.RenewCSILeaseResponse{
						Errorcode: msg.CSILeaseErrorCode_csiLeaseOk,
						Lease: &msg.CSIVolumeLease{
							VolumeId:   "vol-1",
							NodeId:     "node-a",
							HostNqn:    "nqn.host.1",
							LeaseId:    7,
							TtlSeconds: 30,
						},
					},
				},
			}
		case 3:
			if _, ok := req.Union.(*msg.Request_ReleaseCsiLeaseRequest); !ok {
				t.Fatalf("unexpected release request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_ReleaseCsiLeaseResponse{
					ReleaseCsiLeaseResponse: &msg.ReleaseCSILeaseResponse{
						Errorcode: msg.CSILeaseErrorCode_csiLeaseOk,
					},
				},
			}
		default:
			t.Fatalf("unexpected call index %d", call)
			return nil
		}
	})

	client := NewTCP(address)
	lease, err := client.AcquireLease(context.Background(), Lease{
		VolumeID:   "vol-1",
		NodeID:     "node-a",
		HostNQN:    "nqn.host.1",
		TTLSeconds: 30,
	})
	if err != nil {
		t.Fatalf("acquire lease failed: %v", err)
	}
	if lease.LeaseID != 7 {
		t.Fatalf("unexpected lease: %+v", lease)
	}
	got, err := client.GetLease(context.Background(), "vol-1")
	if err != nil {
		t.Fatalf("get lease failed: %v", err)
	}
	if got.NodeID != "node-a" {
		t.Fatalf("unexpected lease: %+v", got)
	}
	if _, err := client.RenewLease(context.Background(), lease); err != nil {
		t.Fatalf("renew lease failed: %v", err)
	}
	if err := client.ReleaseLease(context.Background(), lease); err != nil {
		t.Fatalf("release lease failed: %v", err)
	}
}

func TestListLeases(t *testing.T) {
	address := startMockMonitor(t, func(req *msg.Request) *msg.Response {
		if _, ok := req.Union.(*msg.Request_ListCsiLeaseRequest); !ok {
			t.Fatalf("unexpected request type %T", req.Union)
		}
		return &msg.Response{
			Union: &msg.Response_ListCsiLeaseResponse{
				ListCsiLeaseResponse: &msg.ListCSILeaseResponse{
					Errorcode: msg.CSILeaseErrorCode_csiLeaseOk,
					Leases: []*msg.CSIVolumeLease{{
						VolumeId:   "vol-1",
						NodeId:     "node-a",
						HostNqn:    "nqn.host.1",
						LeaseId:    7,
						TtlSeconds: 30,
					}},
				},
			},
		}
	})

	client := NewTCP(address)
	items, err := client.ListLeases(context.Background())
	if err != nil {
		t.Fatalf("list leases failed: %v", err)
	}
	if len(items) != 1 || items[0].LeaseID != 7 {
		t.Fatalf("unexpected lease list: %+v", items)
	}
}

func TestLeaseErrorsUseDedicatedSentinels(t *testing.T) {
	address := startMockMonitorSequence(t, func(call int, req *msg.Request) *msg.Response {
		switch call {
		case 0:
			if _, ok := req.Union.(*msg.Request_GetCsiLeaseRequest); !ok {
				t.Fatalf("unexpected request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_GetCsiLeaseResponse{
					GetCsiLeaseResponse: &msg.GetCSILeaseResponse{
						Errorcode: msg.CSILeaseErrorCode_csiLeaseNotFound,
					},
				},
			}
		case 1:
			if _, ok := req.Union.(*msg.Request_AcquireCsiLeaseRequest); !ok {
				t.Fatalf("unexpected request type %T", req.Union)
			}
			return &msg.Response{
				Union: &msg.Response_AcquireCsiLeaseResponse{
					AcquireCsiLeaseResponse: &msg.AcquireCSILeaseResponse{
						Errorcode: msg.CSILeaseErrorCode_csiLeaseConflict,
					},
				},
			}
		default:
			t.Fatalf("unexpected call index %d", call)
			return nil
		}
	})

	client := NewTCP(address)
	if _, err := client.GetLease(context.Background(), "missing"); !errors.Is(err, ErrLeaseNotFound) {
		t.Fatalf("expected lease not found, got %v", err)
	}
	if _, err := client.AcquireLease(context.Background(), Lease{
		VolumeID:   "vol-1",
		NodeID:     "node-a",
		HostNQN:    "nqn.host.1",
		TTLSeconds: 30,
	}); !errors.Is(err, ErrLeaseConflict) {
		t.Fatalf("expected lease conflict, got %v", err)
	}
}

func TestImageAttachmentLifecycle(t *testing.T) {
	address := startMockMonitorSequence(t, func(call int, req *msg.Request) *msg.Response {
		switch call {
		case 0, 2, 4:
			payload, ok := req.Union.(*msg.Request_GetImageMetadataByNameRequest)
			if !ok {
				t.Fatalf("unexpected request type %T", req.Union)
			}
			if payload.GetImageMetadataByNameRequest.GetPoolName() != "fb" || payload.GetImageMetadataByNameRequest.GetImageName() != "img-a" {
				t.Fatalf("unexpected get image metadata request: %+v", payload.GetImageMetadataByNameRequest)
			}
			return &msg.Response{
				Union: &msg.Response_GetImageMetadataByNameResponse{
					GetImageMetadataByNameResponse: &msg.GetImageMetadataByNameResponse{
						Errorcode: msg.ImageMetadataErrorCode_imageMetadataOk,
						Metadata: &msg.ImageMetadataV2{
							ImageId:   "img-1",
							PoolName:  "fb",
							ImageName: "img-a",
						},
					},
				},
			}
		case 1:
			payload, ok := req.Union.(*msg.Request_AttachImageRequest)
			if !ok {
				t.Fatalf("unexpected request type %T", req.Union)
			}
			if payload.AttachImageRequest.GetImageId() != "img-1" || payload.AttachImageRequest.GetClientId() != "node-a" {
				t.Fatalf("unexpected attach request: %+v", payload.AttachImageRequest)
			}
			return &msg.Response{
				Union: &msg.Response_AttachImageResponse{
					AttachImageResponse: &msg.AttachImageResponse{
						Errorcode: msg.ImageMetadataErrorCode_imageMetadataOk,
					},
				},
			}
		case 3:
			payload, ok := req.Union.(*msg.Request_RenewImageLeaseRequest)
			if !ok {
				t.Fatalf("unexpected request type %T", req.Union)
			}
			if payload.RenewImageLeaseRequest.GetImageId() != "img-1" || payload.RenewImageLeaseRequest.GetClientId() != "node-a" {
				t.Fatalf("unexpected renew request: %+v", payload.RenewImageLeaseRequest)
			}
			return &msg.Response{
				Union: &msg.Response_RenewImageLeaseResponse{
					RenewImageLeaseResponse: &msg.RenewImageLeaseResponse{
						Errorcode: msg.ImageMetadataErrorCode_imageMetadataOk,
					},
				},
			}
		case 5:
			payload, ok := req.Union.(*msg.Request_DetachImageRequest)
			if !ok {
				t.Fatalf("unexpected request type %T", req.Union)
			}
			if payload.DetachImageRequest.GetImageId() != "img-1" || payload.DetachImageRequest.GetClientId() != "node-a" {
				t.Fatalf("unexpected detach request: %+v", payload.DetachImageRequest)
			}
			return &msg.Response{
				Union: &msg.Response_DetachImageResponse{
					DetachImageResponse: &msg.DetachImageResponse{
						Errorcode: msg.ImageMetadataErrorCode_imageMetadataOk,
					},
				},
			}
		default:
			t.Fatalf("unexpected call index %d", call)
			return nil
		}
	})

	client := NewTCP(address)
	ref := VolumeRef{Name: "img-a", Pool: "fb"}
	if err := client.AttachImage(context.Background(), ref, "node-a", "csi-controller", 30); err != nil {
		t.Fatalf("attach image failed: %v", err)
	}
	if err := client.RenewImageLease(context.Background(), ref, "node-a", 30); err != nil {
		t.Fatalf("renew image lease failed: %v", err)
	}
	if err := client.DetachImage(context.Background(), ref, "node-a"); err != nil {
		t.Fatalf("detach image failed: %v", err)
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
	return startMockMonitorSequence(t, func(_ int, req *msg.Request) *msg.Response {
		return handler(req)
	})
}

func startMockMonitorSequence(t *testing.T, handler func(int, *msg.Request) *msg.Response) string {
	t.Helper()
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen failed: %v", err)
	}
	t.Cleanup(func() { _ = ln.Close() })

	go func() {
		call := 0
		for {
			conn, err := ln.Accept()
			if err != nil {
				return
			}
			func(callIndex int) {
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

				respBody, err := proto.Marshal(handler(callIndex, req))
				if err != nil {
					return
				}
				binary.LittleEndian.PutUint64(header, uint64(len(respBody)))
				_, _ = conn.Write(append(header, respBody...))
			}(call)
			call++
		}
	}()

	return ln.Addr().String()
}
