package controller

import (
	"context"
	"testing"

	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/exporterclient"
	"fastblock-csi/pkg/monitorclient"
)

type stubMonitorClient struct {
	createReq monitorclient.CreateVolumeRequest
	deleteRef monitorclient.VolumeRef
	getRef    monitorclient.VolumeRef
	expandRef monitorclient.VolumeRef
	expandCap int64
}

func (c *stubMonitorClient) CreateVolume(_ context.Context, req monitorclient.CreateVolumeRequest) (monitorclient.Volume, error) {
	c.createReq = req
	return monitorclient.Volume{
		ID:            "fbvol:cluster:1:2",
		Name:          req.Name,
		Pool:          req.Pool,
		CapacityBytes: req.CapacityBytes,
		ObjectSize:    req.ObjectSize,
	}, nil
}

func (c *stubMonitorClient) DeleteVolume(_ context.Context, ref monitorclient.VolumeRef) error {
	c.deleteRef = ref
	return nil
}

func (c *stubMonitorClient) GetVolume(_ context.Context, ref monitorclient.VolumeRef) (monitorclient.Volume, error) {
	c.getRef = ref
	return monitorclient.Volume{Name: ref.Name, Pool: ref.Pool}, nil
}

func (c *stubMonitorClient) ExpandVolume(_ context.Context, ref monitorclient.VolumeRef, capacityBytes int64) (monitorclient.Volume, error) {
	c.expandRef = ref
	c.expandCap = capacityBytes
	return monitorclient.Volume{Name: ref.Name, Pool: ref.Pool, CapacityBytes: capacityBytes}, nil
}

type stubExporterClient struct {
	createReq exporterclient.CreateExportRequest
	deleteID  string
	allowID   string
	allowNQN  string
	denyID    string
	denyNQN   string
}

func (c *stubExporterClient) CreateExport(_ context.Context, req exporterclient.CreateExportRequest) (exporterclient.Export, error) {
	c.createReq = req
	return exporterclient.Export{ID: "exp-1", NQN: "nqn.1", NSID: 1, Traddr: "10.0.0.1", Trsvcid: "4420"}, nil
}

func (c *stubExporterClient) DeleteExport(_ context.Context, exportID string) error {
	c.deleteID = exportID
	return nil
}

func (c *stubExporterClient) AllowHost(_ context.Context, exportID, hostNQN string) error {
	c.allowID = exportID
	c.allowNQN = hostNQN
	return nil
}

func (c *stubExporterClient) DenyHost(_ context.Context, exportID, hostNQN string) error {
	c.denyID = exportID
	c.denyNQN = hostNQN
	return nil
}

func TestCreateVolumeAndPublish(t *testing.T) {
	monitor := &stubMonitorClient{}
	exporter := &stubExporterClient{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)

	volume, err := svc.CreateVolume(context.Background(), CreateVolumeRequest{
		Name:          "img-a",
		Pool:          "fb",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
		BlockSize:     4096,
		Transport:     "rdma",
	})
	if err != nil {
		t.Fatalf("create volume failed: %v", err)
	}
	export, err := svc.PublishVolume(context.Background(), PublishVolumeRequest{
		Volume:    volume,
		BlockSize: 4096,
		Transport: "rdma",
		HostNQN:   "nqn.host.1",
	})
	if err != nil {
		t.Fatalf("publish volume failed: %v", err)
	}
	if monitor.createReq.Name != "img-a" {
		t.Fatalf("unexpected create req: %+v", monitor.createReq)
	}
	if exporter.createReq.ImageName != "img-a" || exporter.allowID != "exp-1" {
		t.Fatalf("unexpected exporter state: %+v", exporter)
	}
	if export.ID != "exp-1" {
		t.Fatalf("unexpected export: %+v", export)
	}
}

func TestDeleteGetExpandAndUnpublish(t *testing.T) {
	monitor := &stubMonitorClient{}
	exporter := &stubExporterClient{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)
	ref := monitorclient.VolumeRef{Name: "img-b", Pool: "fb"}

	if _, err := svc.GetVolume(context.Background(), ref); err != nil {
		t.Fatalf("get volume failed: %v", err)
	}
	if _, err := svc.ExpandVolume(context.Background(), ref, 2<<20); err != nil {
		t.Fatalf("expand volume failed: %v", err)
	}
	if err := svc.DeleteVolume(context.Background(), ref); err != nil {
		t.Fatalf("delete volume failed: %v", err)
	}
	if err := svc.UnpublishVolume(context.Background(), "exp-9", "nqn.host.2"); err != nil {
		t.Fatalf("unpublish volume failed: %v", err)
	}
	if monitor.deleteRef.Name != "img-b" || monitor.expandCap != 2<<20 {
		t.Fatalf("unexpected monitor state: %+v", monitor)
	}
	if exporter.denyID != "exp-9" || exporter.deleteID != "exp-9" {
		t.Fatalf("unexpected exporter state: %+v", exporter)
	}
}

func TestRequestValidation(t *testing.T) {
	if err := (CreateVolumeRequest{}).Validate(); err == nil {
		t.Fatal("expected create request validation error")
	}
	validPublish := PublishVolumeRequest{
		Volume: monitorclient.Volume{
			ID:            "fbvol:cluster:1:2",
			Name:          "img-a",
			Pool:          "fb",
			CapacityBytes: 1 << 20,
			ObjectSize:    4 << 20,
		},
		BlockSize: 4096,
		Transport: "rdma",
	}
	if err := validPublish.Validate(); err != nil {
		t.Fatalf("unexpected publish validation error: %v", err)
	}
	validPublish.Transport = "bad"
	if err := validPublish.Validate(); err == nil {
		t.Fatal("expected publish request validation error")
	}
}
