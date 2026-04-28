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
