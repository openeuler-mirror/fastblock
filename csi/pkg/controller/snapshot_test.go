package controller

import (
	"context"
	"testing"
	"time"

	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/exporterclient"
	"fastblock-csi/pkg/monitorclient"
	"fastblock-csi/pkg/volumeid"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

type stubSnapshotMonitorClient struct {
	stubMonitorClient
	snapshots                    map[string]monitorclient.Snapshot
	createSnapshotReq            monitorclient.CreateSnapshotRequest
	listSnapshotsReq             monitorclient.ListSnapshotsRequest
	deleteSnapshotID             string
	createVolumeFromSnapshotReq  monitorclient.CreateVolumeFromSnapshotRequest
	createVolumeFromSnapshotResp monitorclient.Volume
}

func (c *stubSnapshotMonitorClient) CreateSnapshot(_ context.Context, req monitorclient.CreateSnapshotRequest) (monitorclient.Snapshot, error) {
	c.createSnapshotReq = req
	if c.snapshots == nil {
		c.snapshots = map[string]monitorclient.Snapshot{}
	}
	snapshotID := "snap-" + req.Name
	if existing, ok := c.snapshots[snapshotID]; ok {
		return existing, nil
	}
	snapshot := monitorclient.Snapshot{
		ID:           snapshotID,
		Name:         req.Name,
		SourceVolume: req.SourceVolume,
		CreationTime: time.Unix(1714608000, 0).UTC(),
		SizeBytes:    1 << 20,
		ReadyToUse:   true,
	}
	c.snapshots[snapshotID] = snapshot
	return snapshot, nil
}

func (c *stubSnapshotMonitorClient) DeleteSnapshot(_ context.Context, snapshotID string) error {
	c.deleteSnapshotID = snapshotID
	if _, ok := c.snapshots[snapshotID]; !ok {
		return monitorclient.ErrSnapshotNotFound
	}
	delete(c.snapshots, snapshotID)
	return nil
}

func (c *stubSnapshotMonitorClient) GetSnapshot(_ context.Context, snapshotID string) (monitorclient.Snapshot, error) {
	snapshot, ok := c.snapshots[snapshotID]
	if !ok {
		return monitorclient.Snapshot{}, monitorclient.ErrSnapshotNotFound
	}
	return snapshot, nil
}

func (c *stubSnapshotMonitorClient) ListSnapshots(_ context.Context, req monitorclient.ListSnapshotsRequest) ([]monitorclient.Snapshot, error) {
	c.listSnapshotsReq = req
	items := make([]monitorclient.Snapshot, 0, len(c.snapshots))
	for _, snapshot := range c.snapshots {
		if req.SnapshotID != "" && snapshot.ID != req.SnapshotID {
			continue
		}
		if req.SourceVolumeID != "" && snapshot.SourceVolume.ID != req.SourceVolumeID {
			continue
		}
		items = append(items, snapshot)
	}
	return items, nil
}

func (c *stubSnapshotMonitorClient) CreateVolumeFromSnapshot(_ context.Context, req monitorclient.CreateVolumeFromSnapshotRequest) (monitorclient.Volume, error) {
	c.createVolumeFromSnapshotReq = req
	if _, ok := c.snapshots[req.SnapshotID]; !ok {
		return monitorclient.Volume{}, monitorclient.ErrSnapshotNotFound
	}
	if c.createVolumeFromSnapshotResp.ID != "" {
		volume := c.createVolumeFromSnapshotResp
		volume = normalizeVolume(volume, monitorclient.VolumeRef{Name: req.Name, Pool: req.Pool})
		if volume.CapacityBytes == 0 {
			volume.CapacityBytes = req.CapacityBytes
		}
		if volume.ObjectSize == 0 {
			volume.ObjectSize = req.ObjectSize
		}
		return volume, nil
	}
	volumeID, _ := volumeid.EncodeNameRef(volumeid.NameRef{Pool: req.Pool, Name: req.Name})
	return monitorclient.Volume{
		ID:            volumeID,
		Name:          req.Name,
		Pool:          req.Pool,
		CapacityBytes: req.CapacityBytes,
		ObjectSize:    req.ObjectSize,
	}, nil
}

func TestControllerCapabilitiesIncludeSnapshotsWhenSupported(t *testing.T) {
	service := New(
		driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"},
		&stubSnapshotMonitorClient{},
		exporterclient.NewNoop(),
	)
	grpcService := NewGRPCService(service)

	resp, err := grpcService.ControllerGetCapabilities(context.Background(), &csi.ControllerGetCapabilitiesRequest{})
	if err != nil {
		t.Fatalf("get capabilities failed: %v", err)
	}
	if len(resp.GetCapabilities()) != 4 {
		t.Fatalf("unexpected capability count: %d", len(resp.GetCapabilities()))
	}
	if resp.GetCapabilities()[2].GetRpc().GetType() != csi.ControllerServiceCapability_RPC_CREATE_DELETE_SNAPSHOT {
		t.Fatalf("missing create/delete snapshot capability: %+v", resp.GetCapabilities()[2])
	}
	if resp.GetCapabilities()[3].GetRpc().GetType() != csi.ControllerServiceCapability_RPC_LIST_SNAPSHOTS {
		t.Fatalf("missing list snapshots capability: %+v", resp.GetCapabilities()[3])
	}
}

func TestControllerSnapshotLifecycle(t *testing.T) {
	monitor := &stubSnapshotMonitorClient{}
	service := New(
		driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"},
		monitor,
		exporterclient.NewNoop(),
	)
	grpcService := NewGRPCService(service)
	sourceVolumeID, _ := volumeid.EncodeNameRef(volumeid.NameRef{Pool: "fb", Name: "img-a"})

	createResp, err := grpcService.CreateSnapshot(context.Background(), &csi.CreateSnapshotRequest{
		Name:           "snap-a",
		SourceVolumeId: sourceVolumeID,
	})
	if err != nil {
		t.Fatalf("create snapshot failed: %v", err)
	}
	if createResp.GetSnapshot() == nil || createResp.GetSnapshot().GetSnapshotId() == "" {
		t.Fatalf("unexpected create snapshot response: %+v", createResp)
	}
	if monitor.createSnapshotReq.SourceVolume.ID != sourceVolumeID {
		t.Fatalf("unexpected create snapshot source volume: %+v", monitor.createSnapshotReq.SourceVolume)
	}

	listResp, err := grpcService.ListSnapshots(context.Background(), &csi.ListSnapshotsRequest{
		SourceVolumeId: sourceVolumeID,
	})
	if err != nil {
		t.Fatalf("list snapshots failed: %v", err)
	}
	if len(listResp.GetEntries()) != 1 {
		t.Fatalf("unexpected snapshot list response: %+v", listResp)
	}
	if listResp.GetEntries()[0].GetSnapshot().GetSnapshotId() != createResp.GetSnapshot().GetSnapshotId() {
		t.Fatalf("unexpected listed snapshot: %+v", listResp.GetEntries()[0].GetSnapshot())
	}

	if _, err := grpcService.DeleteSnapshot(context.Background(), &csi.DeleteSnapshotRequest{
		SnapshotId: createResp.GetSnapshot().GetSnapshotId(),
	}); err != nil {
		t.Fatalf("delete snapshot failed: %v", err)
	}
	if monitor.deleteSnapshotID != createResp.GetSnapshot().GetSnapshotId() {
		t.Fatalf("unexpected deleted snapshot id: %q", monitor.deleteSnapshotID)
	}
}

func TestCreateVolumeFromSnapshot(t *testing.T) {
	sourceVolumeID, _ := volumeid.EncodeNameRef(volumeid.NameRef{Pool: "fb", Name: "img-a"})
	monitor := &stubSnapshotMonitorClient{
		snapshots: map[string]monitorclient.Snapshot{
			"snap-a": {
				ID:           "snap-a",
				Name:         "snap-a",
				SourceVolume: monitorclient.VolumeRef{ID: sourceVolumeID, Pool: "fb", Name: "img-a"},
				CreationTime: time.Unix(1714608000, 0).UTC(),
				SizeBytes:    1 << 20,
				ReadyToUse:   true,
			},
		},
	}
	service := New(
		driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"},
		monitor,
		&stubExporterClient{},
	)
	grpcService := NewGRPCService(service)

	resp, err := grpcService.CreateVolume(context.Background(), &csi.CreateVolumeRequest{
		Name: "img-from-snap",
		CapacityRange: &csi.CapacityRange{
			RequiredBytes: 1 << 20,
		},
		Parameters: map[string]string{
			"pool":       "fb",
			"objectSize": "4194304",
			"blockSize":  "4096",
			"transport":  "rdma",
		},
		VolumeContentSource: &csi.VolumeContentSource{
			Type: &csi.VolumeContentSource_Snapshot{
				Snapshot: &csi.VolumeContentSource_SnapshotSource{
					SnapshotId: "snap-a",
				},
			},
		},
	})
	if err != nil {
		t.Fatalf("create volume from snapshot failed: %v", err)
	}
	if resp.GetVolume() == nil || resp.GetVolume().GetContentSource().GetSnapshot().GetSnapshotId() != "snap-a" {
		t.Fatalf("unexpected create volume from snapshot response: %+v", resp)
	}
	if monitor.createVolumeFromSnapshotReq.SnapshotID != "snap-a" {
		t.Fatalf("unexpected create volume from snapshot request: %+v", monitor.createVolumeFromSnapshotReq)
	}
}

func TestCreateVolumeFromSnapshotRejectsNotReadySnapshot(t *testing.T) {
	sourceVolumeID, _ := volumeid.EncodeNameRef(volumeid.NameRef{Pool: "fb", Name: "img-a"})
	monitor := &stubSnapshotMonitorClient{
		snapshots: map[string]monitorclient.Snapshot{
			"snap-a": {
				ID:           "snap-a",
				Name:         "snap-a",
				SourceVolume: monitorclient.VolumeRef{ID: sourceVolumeID, Pool: "fb", Name: "img-a"},
				CreationTime: time.Unix(1714608000, 0).UTC(),
				SizeBytes:    1 << 20,
				ReadyToUse:   false,
			},
		},
	}
	service := New(
		driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"},
		monitor,
		&stubExporterClient{},
	)
	grpcService := NewGRPCService(service)

	_, err := grpcService.CreateVolume(context.Background(), &csi.CreateVolumeRequest{
		Name: "img-from-snap",
		CapacityRange: &csi.CapacityRange{
			RequiredBytes: 1 << 20,
		},
		Parameters: map[string]string{
			"pool":       "fb",
			"objectSize": "4194304",
			"blockSize":  "4096",
			"transport":  "rdma",
		},
		VolumeContentSource: &csi.VolumeContentSource{
			Type: &csi.VolumeContentSource_Snapshot{
				Snapshot: &csi.VolumeContentSource_SnapshotSource{
					SnapshotId: "snap-a",
				},
			},
		},
	})
	if status.Code(err) != codes.FailedPrecondition {
		t.Fatalf("expected failed precondition for not ready snapshot, got %v", err)
	}
}

func TestSnapshotRPCsAreUnimplementedWithoutSnapshotBackend(t *testing.T) {
	service := New(
		driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"},
		&stubMonitorClient{},
		&stubExporterClient{},
	)
	grpcService := NewGRPCService(service)

	if _, err := grpcService.CreateSnapshot(context.Background(), &csi.CreateSnapshotRequest{
		Name:           "snap-a",
		SourceVolumeId: "fbvolname:fb:img-a",
	}); status.Code(err) != codes.Unimplemented {
		t.Fatalf("expected create snapshot unimplemented, got %v", err)
	}
	if _, err := grpcService.ListSnapshots(context.Background(), &csi.ListSnapshotsRequest{}); status.Code(err) != codes.Unimplemented {
		t.Fatalf("expected list snapshots unimplemented, got %v", err)
	}
	if _, err := grpcService.DeleteSnapshot(context.Background(), &csi.DeleteSnapshotRequest{
		SnapshotId: "snap-a",
	}); status.Code(err) != codes.Unimplemented {
		t.Fatalf("expected delete snapshot unimplemented, got %v", err)
	}
	if _, err := grpcService.CreateVolume(context.Background(), &csi.CreateVolumeRequest{
		Name: "img-from-snap",
		CapacityRange: &csi.CapacityRange{
			RequiredBytes: 1 << 20,
		},
		Parameters: map[string]string{
			"pool":       "fb",
			"objectSize": "4194304",
			"blockSize":  "4096",
			"transport":  "rdma",
		},
		VolumeContentSource: &csi.VolumeContentSource{
			Type: &csi.VolumeContentSource_Snapshot{
				Snapshot: &csi.VolumeContentSource_SnapshotSource{
					SnapshotId: "snap-a",
				},
			},
		},
	}); status.Code(err) != codes.Unimplemented {
		t.Fatalf("expected create volume from snapshot unimplemented, got %v", err)
	}
}
