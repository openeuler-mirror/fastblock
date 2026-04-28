package controller

import (
	"context"
	"testing"

	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/exporterclient"
	"fastblock-csi/pkg/monitorclient"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
)

func TestControllerGRPCService(t *testing.T) {
	service := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitorclient.NewNoop(), exporterclient.NewNoop())
	grpcService := NewGRPCService(service)

	resp, err := grpcService.ControllerGetCapabilities(context.Background(), &csi.ControllerGetCapabilitiesRequest{})
	if err != nil {
		t.Fatalf("get capabilities failed: %v", err)
	}
	if len(resp.GetCapabilities()) == 0 {
		t.Fatalf("expected controller capabilities")
	}

	validateResp, err := grpcService.ValidateVolumeCapabilities(context.Background(), &csi.ValidateVolumeCapabilitiesRequest{
		VolumeCapabilities: []*csi.VolumeCapability{
			{
				AccessType: &csi.VolumeCapability_Block{Block: &csi.VolumeCapability_BlockVolume{}},
				AccessMode: &csi.VolumeCapability_AccessMode{Mode: csi.VolumeCapability_AccessMode_SINGLE_NODE_WRITER},
			},
		},
	})
	if err != nil {
		t.Fatalf("validate volume capabilities failed: %v", err)
	}
	if validateResp.GetConfirmed() == nil {
		t.Fatalf("expected confirmed capabilities")
	}
}

func TestControllerGRPCCreateAndDeleteVolume(t *testing.T) {
	monitor := &stubMonitorClient{}
	service := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporterclient.NewNoop())
	grpcService := NewGRPCService(service)

	createResp, err := grpcService.CreateVolume(context.Background(), &csi.CreateVolumeRequest{
		Name: "img-a",
		CapacityRange: &csi.CapacityRange{
			RequiredBytes: 1 << 20,
		},
		Parameters: map[string]string{
			"pool":       "fb",
			"objectSize": "4194304",
			"blockSize":  "4096",
			"transport":  "rdma",
		},
	})
	if err != nil {
		t.Fatalf("create volume failed: %v", err)
	}
	if createResp.GetVolume() == nil || createResp.GetVolume().GetVolumeId() == "" {
		t.Fatalf("unexpected create response: %+v", createResp)
	}

	_, err = grpcService.DeleteVolume(context.Background(), &csi.DeleteVolumeRequest{
		VolumeId: createResp.GetVolume().GetVolumeId(),
	})
	if err != nil {
		t.Fatalf("delete volume failed: %v", err)
	}
	if monitor.deleteRef.Name != "img-a" || monitor.deleteRef.Pool != "fb" {
		t.Fatalf("unexpected delete ref: %+v", monitor.deleteRef)
	}
}

func TestControllerGRPCPublishAndUnpublishVolume(t *testing.T) {
	monitor := &stubMonitorClient{}
	exporter := &stubExporterClient{}
	service := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, monitor, exporter)
	grpcService := NewGRPCService(service)

	req := &csi.ControllerPublishVolumeRequest{
		VolumeId: "fbvolname:fb:img-a",
		NodeId:   "node-a",
		VolumeContext: map[string]string{
			"pool":          "fb",
			"name":          "img-a",
			"transport":     "rdma",
			"blockSize":     "4096",
			"objectSize":    "4194304",
			"capacityBytes": "1048576",
		},
		Secrets: map[string]string{
			"hostNQN": "nqn.host.1",
		},
	}

	resp, err := grpcService.ControllerPublishVolume(context.Background(), req)
	if err != nil {
		t.Fatalf("publish volume failed: %v", err)
	}
	if resp.GetPublishContext() == nil || resp.GetPublishContext()["nqn"] == "" {
		t.Fatalf("unexpected publish response: %+v", resp)
	}
	if exporter.allowID != "exp-1" || exporter.allowNQN != "nqn.host.1" {
		t.Fatalf("unexpected exporter allow state: %+v", exporter)
	}

	_, err = grpcService.ControllerUnpublishVolume(context.Background(), &csi.ControllerUnpublishVolumeRequest{
		VolumeId: "exp-1",
		NodeId:   "node-a",
		Secrets: map[string]string{
			"hostNQN": "nqn.host.1",
		},
	})
	if err != nil {
		t.Fatalf("unpublish volume failed: %v", err)
	}
	if exporter.denyID != "exp-1" || exporter.deleteID != "exp-1" {
		t.Fatalf("unexpected exporter unpublish state: %+v", exporter)
	}
}

func TestControllerGRPCRequestValidation(t *testing.T) {
	service := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, &stubMonitorClient{}, &stubExporterClient{})
	grpcService := NewGRPCService(service)

	if _, err := grpcService.CreateVolume(context.Background(), &csi.CreateVolumeRequest{Name: "img-a"}); err == nil {
		t.Fatal("expected create volume validation error")
	}
	if _, err := grpcService.DeleteVolume(context.Background(), &csi.DeleteVolumeRequest{}); err == nil {
		t.Fatal("expected delete volume validation error")
	}
	if _, err := grpcService.ControllerPublishVolume(context.Background(), &csi.ControllerPublishVolumeRequest{}); err == nil {
		t.Fatal("expected controller publish validation error")
	}
	if _, err := grpcService.ControllerUnpublishVolume(context.Background(), &csi.ControllerUnpublishVolumeRequest{}); err == nil {
		t.Fatal("expected controller unpublish validation error")
	}
}

func TestValidateVolumeCapabilitiesRejectsUnsupportedMode(t *testing.T) {
	service := New(driver.Options{DriverName: "csi.fastblock.io", Endpoint: "unix:///tmp/controller.sock"}, &stubMonitorClient{}, &stubExporterClient{})
	grpcService := NewGRPCService(service)

	resp, err := grpcService.ValidateVolumeCapabilities(context.Background(), &csi.ValidateVolumeCapabilitiesRequest{
		VolumeCapabilities: []*csi.VolumeCapability{
			{
				AccessType: &csi.VolumeCapability_Mount{Mount: &csi.VolumeCapability_MountVolume{}},
				AccessMode: &csi.VolumeCapability_AccessMode{Mode: csi.VolumeCapability_AccessMode_SINGLE_NODE_WRITER},
			},
		},
	})
	if err != nil {
		t.Fatalf("validate volume capabilities failed: %v", err)
	}
	if resp.GetConfirmed() != nil {
		t.Fatalf("expected unsupported capability to be unconfirmed")
	}
}
