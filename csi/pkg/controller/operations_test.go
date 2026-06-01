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

type stubMetadataMonitorClient struct {
	stubMonitorClient
	volumeMetadata     map[string]monitorclient.VolumeMetadata
	attachments        map[string]monitorclient.Attachment
	leases             map[string]monitorclient.Lease
	imageAttachments   map[string]string
	putVolumeCalls     int
	putAttachmentCalls int
	acquireLeaseCalls  int
	releaseLeaseCalls  int
	attachImageCalls   int
	detachImageCalls   int
	renewImageCalls    int
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

func (c *stubMetadataMonitorClient) PutVolumeMetadata(_ context.Context, metadata monitorclient.VolumeMetadata) error {
	if c.volumeMetadata == nil {
		c.volumeMetadata = map[string]monitorclient.VolumeMetadata{}
	}
	c.volumeMetadata[metadata.Volume.ID] = metadata
	c.putVolumeCalls++
	return nil
}

func (c *stubMetadataMonitorClient) GetVolumeMetadata(_ context.Context, volumeID string) (monitorclient.VolumeMetadata, error) {
	metadata, ok := c.volumeMetadata[volumeID]
	if !ok {
		return monitorclient.VolumeMetadata{}, monitorclient.ErrMetadataNotFound
	}
	return metadata, nil
}

func (c *stubMetadataMonitorClient) DeleteVolumeMetadata(_ context.Context, volumeID string) error {
	delete(c.volumeMetadata, volumeID)
	return nil
}

func (c *stubMetadataMonitorClient) ListVolumeMetadata(_ context.Context) ([]monitorclient.VolumeMetadata, error) {
	items := make([]monitorclient.VolumeMetadata, 0, len(c.volumeMetadata))
	for _, item := range c.volumeMetadata {
		items = append(items, item)
	}
	return items, nil
}

func (c *stubMetadataMonitorClient) PutAttachment(_ context.Context, attachment monitorclient.Attachment) error {
	if c.attachments == nil {
		c.attachments = map[string]monitorclient.Attachment{}
	}
	c.attachments[attachment.VolumeID] = attachment
	c.putAttachmentCalls++
	return nil
}

func (c *stubMetadataMonitorClient) GetAttachment(_ context.Context, volumeID string) (monitorclient.Attachment, error) {
	attachment, ok := c.attachments[volumeID]
	if !ok {
		return monitorclient.Attachment{}, monitorclient.ErrMetadataNotFound
	}
	return attachment, nil
}

func (c *stubMetadataMonitorClient) DeleteAttachment(_ context.Context, volumeID string) error {
	delete(c.attachments, volumeID)
	return nil
}

func (c *stubMetadataMonitorClient) ListAttachments(_ context.Context) ([]monitorclient.Attachment, error) {
	items := make([]monitorclient.Attachment, 0, len(c.attachments))
	for _, item := range c.attachments {
		items = append(items, item)
	}
	return items, nil
}

func (c *stubMetadataMonitorClient) AcquireLease(_ context.Context, lease monitorclient.Lease) (monitorclient.Lease, error) {
	if c.leases == nil {
		c.leases = map[string]monitorclient.Lease{}
	}
	if existing, ok := c.leases[lease.VolumeID]; ok {
		if existing.NodeID != lease.NodeID || existing.HostNQN != lease.HostNQN {
			return monitorclient.Lease{}, monitorclient.ErrLeaseConflict
		}
		c.acquireLeaseCalls++
		return existing, nil
	}
	lease.LeaseID = int64(len(c.leases) + 1)
	c.leases[lease.VolumeID] = lease
	c.acquireLeaseCalls++
	return lease, nil
}

func (c *stubMetadataMonitorClient) GetLease(_ context.Context, volumeID string) (monitorclient.Lease, error) {
	lease, ok := c.leases[volumeID]
	if !ok {
		return monitorclient.Lease{}, monitorclient.ErrLeaseNotFound
	}
	return lease, nil
}

func (c *stubMetadataMonitorClient) RenewLease(_ context.Context, lease monitorclient.Lease) (monitorclient.Lease, error) {
	existing, ok := c.leases[lease.VolumeID]
	if !ok {
		return monitorclient.Lease{}, monitorclient.ErrLeaseNotFound
	}
	if existing.NodeID != lease.NodeID || existing.HostNQN != lease.HostNQN {
		return monitorclient.Lease{}, monitorclient.ErrLeaseConflict
	}
	return existing, nil
}

func (c *stubMetadataMonitorClient) ReleaseLease(_ context.Context, lease monitorclient.Lease) error {
	existing, ok := c.leases[lease.VolumeID]
	if !ok {
		return nil
	}
	if existing.NodeID != lease.NodeID || existing.HostNQN != lease.HostNQN {
		return monitorclient.ErrLeaseConflict
	}
	delete(c.leases, lease.VolumeID)
	c.releaseLeaseCalls++
	return nil
}

func (c *stubMetadataMonitorClient) ListLeases(_ context.Context) ([]monitorclient.Lease, error) {
	items := make([]monitorclient.Lease, 0, len(c.leases))
	for _, item := range c.leases {
		items = append(items, item)
	}
	return items, nil
}

func (c *stubMetadataMonitorClient) AttachImage(_ context.Context, ref monitorclient.VolumeRef, clientID, _ string, _ int64) error {
	if c.imageAttachments == nil {
		c.imageAttachments = map[string]string{}
	}
	c.imageAttachments[ref.ID] = clientID
	c.attachImageCalls++
	return nil
}

func (c *stubMetadataMonitorClient) DetachImage(_ context.Context, ref monitorclient.VolumeRef, clientID string) error {
	if c.imageAttachments != nil {
		if existing, ok := c.imageAttachments[ref.ID]; ok && existing == clientID {
			delete(c.imageAttachments, ref.ID)
		}
	}
	c.detachImageCalls++
	return nil
}

func (c *stubMetadataMonitorClient) RenewImageLease(_ context.Context, ref monitorclient.VolumeRef, clientID string, _ int64) error {
	if c.imageAttachments == nil {
		c.imageAttachments = map[string]string{}
	}
	if _, ok := c.imageAttachments[ref.ID]; !ok {
		return monitorclient.ErrImageNotFound
	}
	c.imageAttachments[ref.ID] = clientID
	c.renewImageCalls++
	return nil
}

type stubExporterClient struct {
	createReq exporterclient.CreateExportRequest
	createCnt int
	deleteID  string
	deleteCnt int
	getErr    error
	getExport exporterclient.Export
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
	if c.getErr != nil {
		return exporterclient.Export{}, c.getErr
	}
	if c.getExport.ID != "" {
		return c.getExport, nil
	}
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
	metadata, ok, err := svc.volumes.Get(context.Background(), volume.ID)
	if err != nil {
		t.Fatalf("get stored volume metadata failed: %v", err)
	}
	if !ok || metadata.ExportID != first.Export.ID {
		t.Fatalf("expected volume metadata export id %q, got %+v", first.Export.ID, metadata)
	}
}

func TestControllerPublishTracksImageAttachmentLifecycle(t *testing.T) {
	monitor := &stubMetadataMonitorClient{}
	exporter := &stubExporterClient{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)
	volume := monitorclient.Volume{
		ID:            "fbvolname:fb:img-attach",
		Name:          "img-attach",
		Pool:          "fb",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
	}

	result, err := svc.ControllerPublishVolume(context.Background(), ControllerPublishRequest{
		Volume:    volume,
		BlockSize: 4096,
		Transport: "rdma",
		NodeID:    "node-a",
		Secrets:   map[string]string{"hostNQN": "nqn.host.1"},
	})
	if err != nil {
		t.Fatalf("controller publish failed: %v", err)
	}
	if monitor.attachImageCalls != 1 {
		t.Fatalf("expected image attachment on publish, got %d calls", monitor.attachImageCalls)
	}
	if got := monitor.imageAttachments[volume.ID]; got != "node-a" {
		t.Fatalf("expected image attachment client id node-a, got %q", got)
	}

	if err := svc.ControllerUnpublishVolume(context.Background(), ControllerUnpublishRequest{
		VolumeID: volume.ID,
		ExportID: result.Export.ID,
		NodeID:   "node-a",
		Secrets:  map[string]string{"hostNQN": "nqn.host.1"},
	}); err != nil {
		t.Fatalf("controller unpublish failed: %v", err)
	}
	if monitor.detachImageCalls != 1 {
		t.Fatalf("expected image detach on unpublish, got %d calls", monitor.detachImageCalls)
	}
	if _, ok := monitor.imageAttachments[volume.ID]; ok {
		t.Fatalf("expected image attachment to be removed after unpublish")
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

func TestDeleteVolumeUsesStoredExportID(t *testing.T) {
	monitor := &stubMonitorClient{}
	exporter := &stubExporterClient{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)
	volume := monitorclient.Volume{
		ID:            "opaque-volume-id",
		Name:          "img-a",
		Pool:          "fb",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
	}
	if err := svc.volumes.Put(context.Background(), VolumeMetadata{
		Volume:    volume,
		BlockSize: 4096,
		Transport: "rdma",
		ExportID:  "exp-stored",
	}); err != nil {
		t.Fatalf("put stored volume metadata failed: %v", err)
	}

	if err := svc.DeleteVolume(context.Background(), DeleteVolumeRequest{
		Volume: monitorclient.VolumeRef{ID: volume.ID, Name: volume.Name, Pool: volume.Pool},
	}); err != nil {
		t.Fatalf("delete volume failed: %v", err)
	}
	if exporter.deleteID != "exp-stored" {
		t.Fatalf("expected stored export id, got %q", exporter.deleteID)
	}
}

func TestControllerUnpublishUsesStoredExportIDWithoutAttachment(t *testing.T) {
	monitor := &stubMonitorClient{}
	exporter := &stubExporterClient{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)
	volume := monitorclient.Volume{
		ID:            "opaque-volume-id",
		Name:          "img-a",
		Pool:          "fb",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
	}
	if err := svc.volumes.Put(context.Background(), VolumeMetadata{
		Volume:    volume,
		BlockSize: 4096,
		Transport: "rdma",
		ExportID:  "exp-stored",
	}); err != nil {
		t.Fatalf("put stored volume metadata failed: %v", err)
	}

	if err := svc.ControllerUnpublishVolume(context.Background(), ControllerUnpublishRequest{
		VolumeID: volume.ID,
		NodeID:   "node-a",
		Secrets:  map[string]string{"hostNQN": "nqn.host.1"},
	}); err != nil {
		t.Fatalf("controller unpublish failed: %v", err)
	}
	if exporter.denyID != "exp-stored" || exporter.deleteID != "exp-stored" {
		t.Fatalf("expected stored export id, got deny=%q delete=%q", exporter.denyID, exporter.deleteID)
	}
}

func TestControllerUnpublishWithoutNodeIDUsesStoredAttachmentOwner(t *testing.T) {
	monitor := &stubMetadataMonitorClient{
		attachments: map[string]monitorclient.Attachment{
			"vol-1": {
				VolumeID: "vol-1",
				NodeID:   "kerneldev",
				HostNQN:  "nqn.host.1",
				ExportID: "exp-1",
			},
		},
		leases: map[string]monitorclient.Lease{
			"vol-1": {
				VolumeID:   "vol-1",
				NodeID:     "kerneldev",
				HostNQN:    "nqn.host.1",
				LeaseID:    1,
				TTLSeconds: defaultLeaseTTLSeconds,
			},
		},
	}
	exporter := &stubExporterClient{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)

	if err := svc.ControllerUnpublishVolume(context.Background(), ControllerUnpublishRequest{
		VolumeID: "vol-1",
	}); err != nil {
		t.Fatalf("controller unpublish failed: %v", err)
	}
	if exporter.denyID != "exp-1" || exporter.deleteID != "exp-1" {
		t.Fatalf("expected stored export id, got deny=%q delete=%q", exporter.denyID, exporter.deleteID)
	}
	if monitor.releaseLeaseCalls == 0 {
		t.Fatal("expected lease release call")
	}
}

func TestNewUsesMonitorMetadataStoreWhenAvailable(t *testing.T) {
	monitor := &stubMetadataMonitorClient{}
	exporter := &stubExporterClient{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)
	if _, ok := svc.volumes.(*monitorVolumeStore); !ok {
		t.Fatalf("expected monitor-backed volume store, got %T", svc.volumes)
	}
	if _, ok := svc.attachments.(*monitorAttachmentStore); !ok {
		t.Fatalf("expected monitor-backed attachment store, got %T", svc.attachments)
	}

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
	if _, err := svc.ControllerPublishVolume(context.Background(), ControllerPublishRequest{
		Volume:    volume,
		BlockSize: 4096,
		Transport: "rdma",
		NodeID:    "node-a",
		Secrets:   map[string]string{"hostNQN": "nqn.host.1"},
	}); err != nil {
		t.Fatalf("controller publish failed: %v", err)
	}
	if monitor.putVolumeCalls == 0 {
		t.Fatal("expected volume metadata writes")
	}
	if monitor.putAttachmentCalls == 0 {
		t.Fatal("expected attachment metadata writes")
	}
	if monitor.acquireLeaseCalls == 0 {
		t.Fatal("expected lease acquire calls")
	}
}

func TestDeleteVolumeRejectsActiveLease(t *testing.T) {
	monitor := &stubMetadataMonitorClient{
		leases: map[string]monitorclient.Lease{
			"vol-1": {
				VolumeID:   "vol-1",
				NodeID:     "node-a",
				HostNQN:    "nqn.host.1",
				LeaseID:    1,
				TTLSeconds: defaultLeaseTTLSeconds,
			},
		},
	}
	exporter := &stubExporterClient{}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)
	err := svc.DeleteVolume(context.Background(), DeleteVolumeRequest{
		Volume: monitorclient.VolumeRef{ID: "vol-1", Name: "img-a", Pool: "fb"},
	})
	if !errors.Is(err, ErrVolumeStillPublished) {
		t.Fatalf("expected active lease to block delete, got %v", err)
	}
}

func TestReconcileReacquiresMissingLeaseForActiveAttachment(t *testing.T) {
	monitor := &stubMetadataMonitorClient{
		volumeMetadata: map[string]monitorclient.VolumeMetadata{
			"vol-1": {
				Volume: monitorclient.Volume{
					ID:            "vol-1",
					Name:          "img-a",
					Pool:          "fb",
					CapacityBytes: 1 << 20,
					ObjectSize:    4 << 20,
				},
				BlockSize: 4096,
				Transport: "rdma",
				ExportID:  "exp-1",
			},
		},
		attachments: map[string]monitorclient.Attachment{
			"vol-1": {
				VolumeID: "vol-1",
				NodeID:   "node-a",
				HostNQN:  "nqn.host.1",
				ExportID: "exp-1",
			},
		},
	}
	exporter := &stubExporterClient{
		getExport: exporterclient.Export{ID: "exp-1", NQN: "nqn.1", NSID: 1, Traddr: "10.0.0.1", Trsvcid: "4420"},
	}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)

	if err := svc.Reconcile(context.Background()); err != nil {
		t.Fatalf("reconcile failed: %v", err)
	}
	if monitor.acquireLeaseCalls == 0 {
		t.Fatal("expected reconcile to reacquire missing lease")
	}
	lease, err := monitor.GetLease(context.Background(), "vol-1")
	if err != nil {
		t.Fatalf("get lease failed: %v", err)
	}
	if lease.NodeID != "node-a" || lease.HostNQN != "nqn.host.1" {
		t.Fatalf("unexpected lease after reconcile: %+v", lease)
	}
	svc.leaseRenewer.Stop("vol-1")
}

func TestReconcileDeletesOrphanExportAndLease(t *testing.T) {
	monitor := &stubMetadataMonitorClient{
		volumeMetadata: map[string]monitorclient.VolumeMetadata{
			"vol-1": {
				Volume: monitorclient.Volume{
					ID:            "vol-1",
					Name:          "img-a",
					Pool:          "fb",
					CapacityBytes: 1 << 20,
					ObjectSize:    4 << 20,
				},
				BlockSize: 4096,
				Transport: "rdma",
				ExportID:  "exp-1",
			},
		},
		leases: map[string]monitorclient.Lease{
			"vol-1": {
				VolumeID:   "vol-1",
				NodeID:     "node-a",
				HostNQN:    "nqn.host.1",
				LeaseID:    1,
				TTLSeconds: defaultLeaseTTLSeconds,
			},
		},
	}
	exporter := &stubExporterClient{
		getExport: exporterclient.Export{ID: "exp-1", NQN: "nqn.1", NSID: 1, Traddr: "10.0.0.1", Trsvcid: "4420"},
	}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)

	if err := svc.Reconcile(context.Background()); err != nil {
		t.Fatalf("reconcile failed: %v", err)
	}
	if exporter.deleteID != "exp-1" {
		t.Fatalf("expected orphan export to be deleted, got %q", exporter.deleteID)
	}
	if monitor.releaseLeaseCalls == 0 {
		t.Fatal("expected orphan lease to be released")
	}
	metadata, err := monitor.GetVolumeMetadata(context.Background(), "vol-1")
	if err != nil {
		t.Fatalf("get volume metadata failed: %v", err)
	}
	if metadata.ExportID != "" {
		t.Fatalf("expected export id to be cleared, got %+v", metadata)
	}
}

func TestReconcileTreatsIncompleteExportAsOrphan(t *testing.T) {
	monitor := &stubMetadataMonitorClient{
		volumeMetadata: map[string]monitorclient.VolumeMetadata{
			"vol-1": {
				Volume: monitorclient.Volume{
					ID:            "vol-1",
					Name:          "img-a",
					Pool:          "fb",
					CapacityBytes: 1 << 20,
					ObjectSize:    4 << 20,
				},
				BlockSize: 4096,
				Transport: "rdma",
				ExportID:  "exp-1",
			},
		},
		leases: map[string]monitorclient.Lease{
			"vol-1": {
				VolumeID:   "vol-1",
				NodeID:     "node-a",
				HostNQN:    "nqn.host.1",
				LeaseID:    1,
				TTLSeconds: defaultLeaseTTLSeconds,
			},
		},
	}
	exporter := &stubExporterClient{
		getErr: exporterclient.ErrIncomplete,
	}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)

	if err := svc.Reconcile(context.Background()); err != nil {
		t.Fatalf("reconcile failed: %v", err)
	}
	if exporter.deleteID != "exp-1" {
		t.Fatalf("expected incomplete export to be deleted, got %q", exporter.deleteID)
	}
	if monitor.releaseLeaseCalls == 0 {
		t.Fatal("expected lease to be released for incomplete export")
	}
	metadata, err := monitor.GetVolumeMetadata(context.Background(), "vol-1")
	if err != nil {
		t.Fatalf("get volume metadata failed: %v", err)
	}
	if metadata.ExportID != "" {
		t.Fatalf("expected export id cleared for incomplete export, got %+v", metadata)
	}
}

func TestControllerPublishReconcilesStaleAttachmentWithoutLeaseOrExport(t *testing.T) {
	monitor := &stubMetadataMonitorClient{
		attachments: map[string]monitorclient.Attachment{
			"vol-1": {
				VolumeID: "vol-1",
				NodeID:   "node-b",
				HostNQN:  "nqn.host.old",
				ExportID: "exp-stale",
			},
		},
	}
	exporter := &stubExporterClient{getErr: exporterclient.ErrNotFound}
	svc := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)
	volume := monitorclient.Volume{
		ID:            "vol-1",
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
	attachment, err := monitor.GetAttachment(context.Background(), "vol-1")
	if err != nil {
		t.Fatalf("get attachment failed: %v", err)
	}
	if attachment.NodeID != "node-a" || attachment.HostNQN != "nqn.host.1" {
		t.Fatalf("expected stale attachment to be replaced, got %+v", attachment)
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
