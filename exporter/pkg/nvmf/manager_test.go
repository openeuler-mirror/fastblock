package nvmf

import (
	"context"
	"testing"

	"fastblock-exporter/pkg/api"
	"fastblock-exporter/pkg/config"
)

type rpcCall struct {
	method string
	params map[string]any
}

type stubCaller struct {
	calls []rpcCall
}

func (c *stubCaller) Call(_ context.Context, method string, params any, result any) error {
	mapped, _ := params.(map[string]any)
	c.calls = append(c.calls, rpcCall{method: method, params: mapped})
	switch method {
	case "bdev_fastblock_create":
		if out, ok := result.(*string); ok {
			*out = mapped["name"].(string)
		}
	case "nvmf_subsystem_add_ns":
		if out, ok := result.(*int); ok {
			*out = 11
		}
	}
	return nil
}

func TestCreateExport(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	rpc := &stubCaller{}
	manager := newLocalManagerWithRPC(cfg, rpc)

	export, err := manager.CreateExport(context.Background(), api.CreateExportRequest{
		VolumeID:      "fbvol:cluster-a:1:7",
		PoolName:      "fb",
		ImageName:     "img-7",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
		BlockSize:     4096,
		Transport:     "rdma",
	})
	if err != nil {
		t.Fatalf("create export failed: %v", err)
	}
	if len(rpc.calls) != 4 {
		t.Fatalf("unexpected call count: %d", len(rpc.calls))
	}
	if rpc.calls[0].method != "bdev_fastblock_create" {
		t.Fatalf("unexpected first method: %s", rpc.calls[0].method)
	}
	if export.ID == "" || export.NQN == "" || export.NSID != 11 {
		t.Fatalf("unexpected export: %+v", export)
	}
}

func TestDeleteAndHostACL(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	rpc := &stubCaller{}
	manager := newLocalManagerWithRPC(cfg, rpc)

	if err := manager.DeleteExport(context.Background(), "fbvol-cluster-a-1-7"); err != nil {
		t.Fatalf("delete export failed: %v", err)
	}
	if err := manager.AllowHost(context.Background(), "fbvol-cluster-a-1-7", "nqn.host.1"); err != nil {
		t.Fatalf("allow host failed: %v", err)
	}
	if err := manager.DenyHost(context.Background(), "fbvol-cluster-a-1-7", "nqn.host.1"); err != nil {
		t.Fatalf("deny host failed: %v", err)
	}
	if len(rpc.calls) != 4 {
		t.Fatalf("unexpected call count: %d", len(rpc.calls))
	}
	if rpc.calls[0].method != "nvmf_delete_subsystem" || rpc.calls[1].method != "bdev_fastblock_delete" {
		t.Fatalf("unexpected delete methods: %+v", rpc.calls)
	}
	if rpc.calls[2].method != "nvmf_subsystem_add_host" || rpc.calls[3].method != "nvmf_subsystem_remove_host" {
		t.Fatalf("unexpected host methods: %+v", rpc.calls)
	}
}
