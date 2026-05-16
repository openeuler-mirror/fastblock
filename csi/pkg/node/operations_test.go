package node

import (
	"context"
	"testing"

	"fastblock-csi/pkg/backend"
	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/mount"
)

type stubBackend struct {
	stageID    string
	stageCtx   backend.VolumeContext
	unstageID  string
	getID      string
	readyID    string
	stageCalls int
	readyCalls int
}

type stubPublisher struct {
	devicePath string
	stagePath  string
	targetPath string
	unpublish  string
}

func (p *stubPublisher) PublishBlockDevice(_ context.Context, devicePath, stagePath, targetPath string) error {
	p.devicePath = devicePath
	p.stagePath = stagePath
	p.targetPath = targetPath
	return nil
}

func (p *stubPublisher) UnpublishBlockDevice(_ context.Context, targetPath string) error {
	p.unpublish = targetPath
	return nil
}

func (b *stubBackend) Stage(_ context.Context, volumeID string, volumeCtx backend.VolumeContext) (string, error) {
	b.stageID = volumeID
	b.stageCtx = volumeCtx
	b.stageCalls++
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
	b.readyCalls++
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
	req := PublishContextStageRequest{VolumeID: "fbvol:cluster:1:4", PublishContext: publishContext}
	device, err := svc.StageVolumeFromPublishContext(context.Background(), req)
	if err != nil {
		t.Fatalf("stage from publish context failed: %v", err)
	}
	ready, err := svc.IsReadyFromPublishContext(context.Background(), req)
	if err != nil {
		t.Fatalf("ready from publish context failed: %v", err)
	}
	if _, err := svc.GetDeviceFromPublishContext(context.Background(), req); err != nil {
		t.Fatalf("get device from publish context failed: %v", err)
	}
	if err := svc.UnstageVolumeFromPublishContext(context.Background(), req); err != nil {
		t.Fatalf("unstage from publish context failed: %v", err)
	}
	if device != "/dev/nvme0n1" || !ready {
		t.Fatalf("unexpected helper results: device=%s ready=%v", device, ready)
	}
}

func TestPublishAndUnpublishVolume(t *testing.T) {
	publisher := &stubPublisher{}
	service := &Service{
		opts:      driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/node.sock", NodeID: "node-a"},
		backend:   &stubBackend{},
		publisher: publisher,
	}
	stagePath := t.TempDir()
	if err := mount.WriteStageState(stagePath, mount.StageState{
		VolumeID:   "fbvolname:fb:img-a",
		DevicePath: "/dev/nvme0n1",
	}); err != nil {
		t.Fatalf("write stage state failed: %v", err)
	}
	if err := service.PublishVolume(context.Background(), PublishVolumeRequest{
		VolumeID:          "fbvolname:fb:img-a",
		StagingTargetPath: stagePath,
		TargetPath:        "/var/lib/kubelet/pods/pod/volumeDevices/publish",
	}); err != nil {
		t.Fatalf("publish volume failed: %v", err)
	}
	stageDevicePath, err := mount.CanonicalStageDevicePath(stagePath)
	if err != nil {
		t.Fatalf("canonical stage device path failed: %v", err)
	}
	if publisher.devicePath != stageDevicePath || publisher.stagePath != stagePath {
		t.Fatalf("unexpected publish call: %+v", publisher)
	}
	if err := service.UnpublishVolume(context.Background(), UnpublishVolumeRequest{
		VolumeID:   "fbvolname:fb:img-a",
		TargetPath: "/var/lib/kubelet/pods/pod/volumeDevices/publish",
	}); err != nil {
		t.Fatalf("unpublish volume failed: %v", err)
	}
	if publisher.unpublish != "/var/lib/kubelet/pods/pod/volumeDevices/publish" {
		t.Fatalf("unexpected unpublish call: %+v", publisher)
	}
}
