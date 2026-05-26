package controller

import (
	"context"
	"errors"
	"testing"

	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/exporterclient"
	"fastblock-csi/pkg/monitorclient"
	"fastblock-csi/pkg/volumeid"
)

type stubMonitorClient struct {
	createReq monitorclient.CreateVolumeRequest
	createVol monitorclient.Volume
	deleteRef monitorclient.VolumeRef
	getRef    monitorclient.VolumeRef
	getVol    monitorclient.Volume
	expandRef monitorclient.VolumeRef
	expandCap int64
}

func (c *stubMonitorClient) CreateVolume(_ context.Context, req monitorclient.CreateVolumeRequest) (monitorclient.Volume, error) {
	c.createReq = req
	if c.createVol.ID != "" {
		volume := c.createVol
		volume = normalizeVolume(volume, monitorclient.VolumeRef{Name: req.Name, Pool: req.Pool})
		if volume.CapacityBytes == 0 {
			volume.CapacityBytes = req.CapacityBytes
		}
		if volume.ObjectSize == 0 {
			volume.ObjectSize = req.ObjectSize
		}
		return volume, nil
	}
	id, _ := volumeid.EncodeNameRef(volumeid.NameRef{Pool: req.Pool, Name: req.Name})
	return monitorclient.Volume{
		ID:            id,
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
	if c.getVol.ID != "" {
		return normalizeVolume(c.getVol, ref), nil
	}
	return monitorclient.Volume{Name: ref.Name, Pool: ref.Pool}, nil
}

func (c *stubMonitorClient) ExpandVolume(_ context.Context, ref monitorclient.VolumeRef, capacityBytes int64) (monitorclient.Volume, error) {
	c.expandRef = ref
	c.expandCap = capacityBytes
	return monitorclient.Volume{Name: ref.Name, Pool: ref.Pool, CapacityBytes: capacityBytes}, nil
}

type stubExporterClient struct {
	createReq exporterclient.CreateExportRequest
	createCnt int
	deleteID  string
	deleteCnt int
	allowID   string
	allowNQN  string
	allowCnt  int
	denyID    string
	denyNQN   string
	denyCnt   int
}

func (c *stubExporterClient) CreateExport(_ context.Context, req exporterclient.CreateExportRequest) (exporterclient.Export, error) {
	c.createReq = req
	c.createCnt++
	exportID, err := exporterclient.ExportIDForVolume(req.VolumeID)
	if err != nil {
		return exporterclient.Export{}, err
	}
	return exporterclient.Export{ID: exportID, NQN: "nqn.1", NSID: 1, Traddr: "10.0.0.1", Trsvcid: "4420"}, nil
}

func (c *stubExporterClient) GetExport(_ context.Context, exportID string) (exporterclient.Export, error) {
	return exporterclient.Export{ID: exportID, NQN: "nqn.1", NSID: 1, Traddr: "10.0.0.1", Trsvcid: "4420"}, nil
}

func (c *stubExporterClient) DeleteExport(_ context.Context, exportID string) error {
	c.deleteID = exportID
	c.deleteCnt++
	return nil
}

func (c *stubExporterClient) AllowHost(_ context.Context, exportID, hostNQN string) error {
	c.allowID = exportID
	c.allowNQN = hostNQN
	c.allowCnt++
	return nil
}

func (c *stubExporterClient) DenyHost(_ context.Context, exportID, hostNQN string) error {
	c.denyID = exportID
	c.denyNQN = hostNQN
	c.denyCnt++
	return nil
}

func TestCreateVolumeAndPublish(t *testing.T) {
	monitor := &stubMonitorClient{}
	exporter := &stubExporterClient{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)
	expectedExportID := mustExportIDForVolume(t, "fbvolname:fb:img-a")

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
	result, err := svc.PublishVolume(context.Background(), PublishVolumeRequest{
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
	if exporter.createReq.ImageName != "img-a" || exporter.allowID != expectedExportID {
		t.Fatalf("unexpected exporter state: %+v", exporter)
	}
	if result.Export.ID != expectedExportID || result.PublishContext["nqn"] == "" {
		t.Fatalf("unexpected publish result: %+v", result)
	}
}

func TestDeleteGetExpandAndUnpublish(t *testing.T) {
	monitor := &stubMonitorClient{}
	exporter := &stubExporterClient{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)
	ref := monitorclient.VolumeRef{Name: "img-b", Pool: "fb"}

	if _, err := svc.GetVolume(context.Background(), GetVolumeRequest{Volume: ref}); err != nil {
		t.Fatalf("get volume failed: %v", err)
	}
	if _, err := svc.ExpandVolume(context.Background(), ExpandVolumeRequest{Volume: ref, CapacityBytes: 2 << 20}); err != nil {
		t.Fatalf("expand volume failed: %v", err)
	}
	if err := svc.DeleteVolume(context.Background(), DeleteVolumeRequest{Volume: ref}); err != nil {
		t.Fatalf("delete volume failed: %v", err)
	}
	if err := svc.UnpublishVolume(context.Background(), UnpublishVolumeRequest{ExportID: "exp-9", HostNQN: "nqn.host.2"}); err != nil {
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

func TestRequestConstructors(t *testing.T) {
	ref := monitorclient.VolumeRef{ID: "fbvol:cluster:1:2", Name: "img-a", Pool: "fb"}
	volume := monitorclient.Volume{ID: "fbvol:cluster:1:2", Name: "img-a", Pool: "fb", CapacityBytes: 1 << 20, ObjectSize: 4 << 20}

	if req := NewDeleteVolumeRequest(ref); req.Volume.Name != "img-a" {
		t.Fatalf("unexpected delete request: %+v", req)
	}
	if req := NewGetVolumeRequest(ref); req.Volume.Pool != "fb" {
		t.Fatalf("unexpected get request: %+v", req)
	}
	if req := NewExpandVolumeRequest(ref, 2<<20); req.CapacityBytes != 2<<20 {
		t.Fatalf("unexpected expand request: %+v", req)
	}
	if req := NewPublishVolumeRequest(volume, 4096, "rdma", "nqn.host.1"); req.HostNQN != "nqn.host.1" {
		t.Fatalf("unexpected publish request: %+v", req)
	}
	if req := NewUnpublishVolumeRequest("exp-1", "nqn.host.1"); req.ExportID != "exp-1" {
		t.Fatalf("unexpected unpublish request: %+v", req)
	}
}

func TestControllerPublishRequestUsesHostNQNPrecedence(t *testing.T) {
	monitor := &stubMonitorClient{}
	exporter := &stubExporterClient{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)
	volume := monitorclient.Volume{
		ID:            "fbvolname:fb:img-a",
		Name:          "img-a",
		Pool:          "fb",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
	}

	if _, err := svc.ControllerPublishVolume(context.Background(), ControllerPublishRequest{
		Volume:    volume,
		BlockSize: 4096,
		Transport: "rdma",
		NodeID:    "node-a",
		Secrets:   map[string]string{"hostNQN": "nqn.secret"},
	}); err != nil {
		t.Fatalf("controller publish failed: %v", err)
	}
	if exporter.allowNQN != "nqn.secret" {
		t.Fatalf("expected secret hostNQN to win, got %q", exporter.allowNQN)
	}

	if err := svc.ControllerUnpublishVolume(context.Background(), ControllerUnpublishRequest{
		VolumeID: "fbvolname:fb:img-a",
		ExportID: "exp-1",
		NodeID:   "node-a",
	}); err != nil {
		t.Fatalf("controller unpublish failed: %v", err)
	}
	if exporter.denyNQN != "nqn.secret" {
		t.Fatalf("expected stored hostNQN on unpublish, got %q", exporter.denyNQN)
	}
}

func TestControllerPublishUsesDefaultHostNQN(t *testing.T) {
	monitor := &stubMonitorClient{}
	exporter := &stubExporterClient{}
	svc := NewWithDefaultHostNQN(
		driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"},
		monitor,
		exporter,
		"nqn.default",
	)
	volume := monitorclient.Volume{
		ID:            "fbvolname:fb:img-a",
		Name:          "img-a",
		Pool:          "fb",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
	}

	if _, err := svc.ControllerPublishVolume(context.Background(), ControllerPublishRequest{
		Volume:    volume,
		BlockSize: 4096,
		Transport: "rdma",
		NodeID:    "node-a",
	}); err != nil {
		t.Fatalf("controller publish failed: %v", err)
	}
	if exporter.allowNQN != "nqn.default" {
		t.Fatalf("expected default hostNQN to win, got %q", exporter.allowNQN)
	}

	if err := svc.ControllerUnpublishVolume(context.Background(), ControllerUnpublishRequest{
		VolumeID: "fbvolname:fb:img-a",
		ExportID: "exp-1",
		NodeID:   "node-a",
	}); err != nil {
		t.Fatalf("controller unpublish failed: %v", err)
	}
	if exporter.denyNQN != "nqn.default" {
		t.Fatalf("expected default hostNQN on unpublish, got %q", exporter.denyNQN)
	}
}

func TestControllerPublishIsIdempotentOnSameNode(t *testing.T) {
	monitor := &stubMonitorClient{}
	exporter := &stubExporterClient{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)
	volume := monitorclient.Volume{
		ID:            "fbvolname:fb:img-a",
		Name:          "img-a",
		Pool:          "fb",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
	}

	first, err := svc.ControllerPublishVolume(context.Background(), ControllerPublishRequest{
		Volume:    volume,
		BlockSize: 4096,
		Transport: "rdma",
		NodeID:    "node-a",
		Secrets:   map[string]string{"hostNQN": "nqn.host.1"},
	})
	if err != nil {
		t.Fatalf("first controller publish failed: %v", err)
	}
	second, err := svc.ControllerPublishVolume(context.Background(), ControllerPublishRequest{
		Volume:    volume,
		BlockSize: 4096,
		Transport: "rdma",
		NodeID:    "node-a",
		Secrets:   map[string]string{"hostNQN": "nqn.host.1"},
	})
	if err != nil {
		t.Fatalf("second controller publish failed: %v", err)
	}
	if first.Export.ID != second.Export.ID {
		t.Fatalf("expected same export id, got %q and %q", first.Export.ID, second.Export.ID)
	}
	if exporter.createCnt != 2 || exporter.allowCnt != 2 {
		t.Fatalf("expected idempotent exporter calls to succeed twice, got create=%d allow=%d", exporter.createCnt, exporter.allowCnt)
	}
}

func TestControllerPublishRejectsDifferentNodeAttachment(t *testing.T) {
	monitor := &stubMonitorClient{}
	exporter := &stubExporterClient{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)
	volume := monitorclient.Volume{
		ID:            "fbvolname:fb:img-a",
		Name:          "img-a",
		Pool:          "fb",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
	}

	if _, err := svc.ControllerPublishVolume(context.Background(), ControllerPublishRequest{
		Volume:    volume,
		BlockSize: 4096,
		Transport: "rdma",
		NodeID:    "node-a",
		Secrets:   map[string]string{"hostNQN": "nqn.host.1"},
	}); err != nil {
		t.Fatalf("initial controller publish failed: %v", err)
	}
	if _, err := svc.ControllerPublishVolume(context.Background(), ControllerPublishRequest{
		Volume:    volume,
		BlockSize: 4096,
		Transport: "rdma",
		NodeID:    "node-b",
		Secrets:   map[string]string{"hostNQN": "nqn.host.2"},
	}); !errors.Is(err, ErrVolumePublishedToAnotherNode) {
		t.Fatalf("expected attachment conflict, got %v", err)
	}
	if exporter.createCnt != 1 || exporter.allowCnt != 1 {
		t.Fatalf("expected conflicting publish to stop before exporter call, got create=%d allow=%d", exporter.createCnt, exporter.allowCnt)
	}
}

func TestDeleteVolumeRejectsAttachedVolume(t *testing.T) {
	monitor := &stubMonitorClient{}
	exporter := &stubExporterClient{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)
	volume := monitorclient.Volume{
		ID:            "fbvolname:fb:img-a",
		Name:          "img-a",
		Pool:          "fb",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
	}

	if _, err := svc.ControllerPublishVolume(context.Background(), ControllerPublishRequest{
		Volume:    volume,
		BlockSize: 4096,
		Transport: "rdma",
		NodeID:    "node-a",
		Secrets:   map[string]string{"hostNQN": "nqn.host.1"},
	}); err != nil {
		t.Fatalf("controller publish failed: %v", err)
	}
	err := svc.DeleteVolume(context.Background(), DeleteVolumeRequest{
		Volume: monitorclient.VolumeRef{ID: volume.ID, Name: volume.Name, Pool: volume.Pool},
	})
	if !errors.Is(err, ErrVolumeStillPublished) {
		t.Fatalf("expected attached delete rejection, got %v", err)
	}
}

func TestControllerUnpublishRejectsDifferentNodeAttachment(t *testing.T) {
	monitor := &stubMonitorClient{}
	exporter := &stubExporterClient{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)
	volume := monitorclient.Volume{
		ID:            "fbvolname:fb:img-a",
		Name:          "img-a",
		Pool:          "fb",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
	}
	exportID := mustExportIDForVolume(t, volume.ID)

	if _, err := svc.ControllerPublishVolume(context.Background(), ControllerPublishRequest{
		Volume:    volume,
		BlockSize: 4096,
		Transport: "rdma",
		NodeID:    "node-a",
		Secrets:   map[string]string{"hostNQN": "nqn.host.1"},
	}); err != nil {
		t.Fatalf("controller publish failed: %v", err)
	}
	err := svc.ControllerUnpublishVolume(context.Background(), ControllerUnpublishRequest{
		VolumeID: volume.ID,
		ExportID: exportID,
		NodeID:   "node-b",
		Secrets:  map[string]string{"hostNQN": "nqn.host.2"},
	})
	if !errors.Is(err, ErrAttachmentNodeMismatch) {
		t.Fatalf("expected unpublish attachment conflict, got %v", err)
	}
	if exporter.denyCnt != 0 || exporter.deleteCnt != 0 {
		t.Fatalf("expected conflicting unpublish to stop before exporter call, got deny=%d delete=%d", exporter.denyCnt, exporter.deleteCnt)
	}
}
func mustExportIDForVolume(t *testing.T, volumeID string) string {
	t.Helper()
	exportID, err := exporterclient.ExportIDForVolume(volumeID)
	if err != nil {
		t.Fatalf("derive export id failed: %v", err)
	}
	return exportID
}
