package driver

import (
	"context"
	"testing"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
)

func TestIdentityService(t *testing.T) {
	svc := NewIdentityService(Options{DriverName: "csi.fastblock.io"})

	info, err := svc.GetPluginInfo(context.Background(), &csi.GetPluginInfoRequest{})
	if err != nil {
		t.Fatalf("get plugin info failed: %v", err)
	}
	if info.GetName() != "csi.fastblock.io" {
		t.Fatalf("unexpected plugin info: %+v", info)
	}

	caps, err := svc.GetPluginCapabilities(context.Background(), &csi.GetPluginCapabilitiesRequest{})
	if err != nil {
		t.Fatalf("get plugin capabilities failed: %v", err)
	}
	if len(caps.GetCapabilities()) == 0 {
		t.Fatalf("expected plugin capabilities")
	}

	probe, err := svc.Probe(context.Background(), &csi.ProbeRequest{})
	if err != nil {
		t.Fatalf("probe failed: %v", err)
	}
	if probe.GetReady() == nil || !probe.GetReady().GetValue() {
		t.Fatalf("unexpected probe response: %+v", probe)
	}
}
