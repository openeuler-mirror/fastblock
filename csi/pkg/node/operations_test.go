package node

import (
	"context"
	"testing"

	"fastblock-csi/pkg/backend"
	"fastblock-csi/pkg/driver"
)

type stubBackend struct {
	stageID   string
	stageCtx  backend.VolumeContext
	unstageID string
	getID     string
	readyID   string
}

func (b *stubBackend) Stage(_ context.Context, volumeID string, volumeCtx backend.VolumeContext) (string, error) {
	b.stageID = volumeID
	b.stageCtx = volumeCtx
	return "/dev/nvme0n1", nil
}

func (b *stubBackend) Unstage(_ context.Context, volumeID string, _ backend.VolumeContext) error {
	b.unstageID = volumeID
	return nil
}

func (b *stubBackend) GetDevice(_ context.Context, volumeID string, _ backend.VolumeContext) (string, error) {
	b.getID = volumeID
	return "/dev/nvme0n1", nil
}

func (b *stubBackend) IsReady(_ context.Context, volumeID string, _ backend.VolumeContext) (bool, error) {
	b.readyID = volumeID
	return true, nil
}

func TestStageAndReadiness(t *testing.T) {
	backendStub := &stubBackend{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/node.sock", NodeID: "node-a"}, backendStub)
	req := StageVolumeRequest{
		VolumeID: "fbvol:cluster:1:2",
		VolumeContext: backend.VolumeContext{
			Transport: "rdma",
			NQN:       "nqn.test",
			Traddr:    "10.0.0.10",
			Trsvcid:   "4420",
			NSID:      1,
		},
	}

	device, err := svc.StageVolume(context.Background(), req)
	if err != nil {
		t.Fatalf("stage failed: %v", err)
	}
	ready, err := svc.IsReady(context.Background(), req)
	if err != nil {
		t.Fatalf("ready failed: %v", err)
	}
	if device != "/dev/nvme0n1" || !ready {
		t.Fatalf("unexpected stage result: device=%s ready=%v", device, ready)
	}
	if backendStub.stageID != req.VolumeID || backendStub.readyID != req.VolumeID {
		t.Fatalf("unexpected backend state: %+v", backendStub)
	}
}

func TestGetDeviceAndUnstage(t *testing.T) {
	backendStub := &stubBackend{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/node.sock", NodeID: "node-a"}, backendStub)
	req := StageVolumeRequest{
		VolumeID: "fbvol:cluster:1:3",
		VolumeContext: backend.VolumeContext{
			Transport: "tcp",
			NQN:       "nqn.test",
			Traddr:    "10.0.0.11",
			Trsvcid:   "4420",
			NSID:      2,
		},
	}

	if _, err := svc.GetDevice(context.Background(), req); err != nil {
		t.Fatalf("get device failed: %v", err)
	}
	if err := svc.UnstageVolume(context.Background(), req); err != nil {
		t.Fatalf("unstage failed: %v", err)
	}
	if backendStub.getID != req.VolumeID || backendStub.unstageID != req.VolumeID {
		t.Fatalf("unexpected backend state: %+v", backendStub)
	}
}

func TestStageVolumeRequestValidation(t *testing.T) {
	if err := (StageVolumeRequest{}).Validate(); err == nil {
		t.Fatal("expected empty request validation error")
	}
	req := StageVolumeRequest{
		VolumeID: "fbvol:cluster:1:9",
		VolumeContext: backend.VolumeContext{
			Transport: "rdma",
			NQN:       "nqn.test",
			Traddr:    "10.0.0.10",
			Trsvcid:   "4420",
			NSID:      1,
		},
	}
	if err := req.Validate(); err != nil {
		t.Fatalf("unexpected validation error: %v", err)
	}
	req.VolumeContext.Transport = "bad"
	if err := req.Validate(); err == nil {
		t.Fatal("expected invalid transport error")
	}
}

func TestPublishContextHelpers(t *testing.T) {
	backendStub := &stubBackend{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/node.sock", NodeID: "node-a"}, backendStub)
	publishContext := map[string]string{
		driver.PublishContextTransport: "rdma",
		driver.PublishContextNQN:       "nqn.test",
		driver.PublishContextTraddr:    "10.0.0.10",
		driver.PublishContextTrsvcid:   "4420",
		driver.PublishContextNSID:      "3",
	}
	device, err := svc.StageVolumeFromPublishContext(context.Background(), "fbvol:cluster:1:4", publishContext)
	if err != nil {
		t.Fatalf("stage from publish context failed: %v", err)
	}
	ready, err := svc.IsReadyFromPublishContext(context.Background(), "fbvol:cluster:1:4", publishContext)
	if err != nil {
		t.Fatalf("ready from publish context failed: %v", err)
	}
	if _, err := svc.GetDeviceFromPublishContext(context.Background(), "fbvol:cluster:1:4", publishContext); err != nil {
		t.Fatalf("get device from publish context failed: %v", err)
	}
	if err := svc.UnstageVolumeFromPublishContext(context.Background(), "fbvol:cluster:1:4", publishContext); err != nil {
		t.Fatalf("unstage from publish context failed: %v", err)
	}
	if device != "/dev/nvme0n1" || !ready {
		t.Fatalf("unexpected helper results: device=%s ready=%v", device, ready)
	}
}
