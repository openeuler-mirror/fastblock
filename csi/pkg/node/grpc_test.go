package node

import (
	"context"
	"testing"

	"fastblock-csi/pkg/backend"
	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/mount"

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

func TestNodeStageAndUnstageVolume(t *testing.T) {
	service := New(driver.Options{
		DriverName: "csi.fastblock.io",
		Endpoint:   "unix:///tmp/node.sock",
		NodeID:     "node-a",
		Mode:       driver.ModeNode,
	}, &stubBackend{})
	grpcService := NewGRPCService(service)
	stagePath := t.TempDir()

	_, err := grpcService.NodeStageVolume(context.Background(), &csi.NodeStageVolumeRequest{
		VolumeId:          "fbvolname:fb:img-a",
		StagingTargetPath: stagePath,
		PublishContext: map[string]string{
			driver.PublishContextTransport: "rdma",
			driver.PublishContextNQN:       "nqn.test",
			driver.PublishContextTraddr:    "10.0.0.10",
			driver.PublishContextTrsvcid:   "4420",
			driver.PublishContextNSID:      "1",
		},
	})
	if err != nil {
		t.Fatalf("node stage volume failed: %v", err)
	}
	state, err := mount.ReadStageState(stagePath)
	if err != nil {
		t.Fatalf("read stage state failed: %v", err)
	}
	if state.VolumeID != "fbvolname:fb:img-a" {
		t.Fatalf("unexpected stage state: %+v", state)
	}

	_, err = grpcService.NodeUnstageVolume(context.Background(), &csi.NodeUnstageVolumeRequest{
		VolumeId:          "fbvolname:fb:img-a",
		StagingTargetPath: stagePath,
	})
	if err != nil {
		t.Fatalf("node unstage volume failed: %v", err)
	}
}

func TestNodeGRPCRequestValidation(t *testing.T) {
	service := New(driver.Options{
		DriverName: "csi.fastblock.io",
		Endpoint:   "unix:///tmp/node.sock",
		NodeID:     "node-a",
		Mode:       driver.ModeNode,
	}, &stubBackend{})
	grpcService := NewGRPCService(service)

	if _, err := grpcService.NodeStageVolume(context.Background(), &csi.NodeStageVolumeRequest{}); err == nil {
		t.Fatal("expected node stage validation error")
	}
	if _, err := grpcService.NodeUnstageVolume(context.Background(), &csi.NodeUnstageVolumeRequest{}); err == nil {
		t.Fatal("expected node unstage validation error")
	}
}
