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
