package node

import (
	"context"
	"testing"

	"fastblock-csi/pkg/backend"
	"fastblock-csi/pkg/driver"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
)

func TestNodeGRPCService(t *testing.T) {
	service := New(driver.Options{
		DriverName: "csi.fastblock.io",
		Endpoint:   "unix:///tmp/node.sock",
		NodeID:     "node-a",
		Mode:       driver.ModeNode,
	}, backend.NewNVMF())
	grpcService := NewGRPCService(service)

	caps, err := grpcService.NodeGetCapabilities(context.Background(), &csi.NodeGetCapabilitiesRequest{})
	if err != nil {
		t.Fatalf("get node capabilities failed: %v", err)
	}
	if len(caps.GetCapabilities()) == 0 {
		t.Fatalf("expected node capabilities")
	}

	info, err := grpcService.NodeGetInfo(context.Background(), &csi.NodeGetInfoRequest{})
	if err != nil {
		t.Fatalf("get node info failed: %v", err)
	}
	if info.GetNodeId() != "node-a" {
		t.Fatalf("unexpected node info: %+v", info)
	}
}
