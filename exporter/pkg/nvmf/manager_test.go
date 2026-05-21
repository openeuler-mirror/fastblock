package nvmf

import (
	"context"
	"errors"
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
	fail  map[string]error
}

func (c *stubCaller) Call(_ context.Context, method string, params any, result any) error {
	mapped, _ := params.(map[string]any)
	c.calls = append(c.calls, rpcCall{method: method, params: mapped})
	if err, ok := c.fail[method]; ok {
		return err
	}
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

func TestCreateExportRollbackOnListenerFailure(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	rpc := &stubCaller{fail: map[string]error{
		"nvmf_subsystem_add_listener": errors.New("listener failed"),
	}}
	manager := newLocalManagerWithRPC(cfg, rpc)

	_, err := manager.CreateExport(context.Background(), api.CreateExportRequest{
		VolumeID:      "fbvol:cluster-a:1:7",
		PoolName:      "fb",
		ImageName:     "img-7",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
		BlockSize:     4096,
		Transport:     "rdma",
	})
	if err == nil {
		t.Fatal("expected create export failure")
	}
	if len(rpc.calls) != 6 {
		t.Fatalf("unexpected call count: %d", len(rpc.calls))
	}
	if rpc.calls[4].method != "nvmf_delete_subsystem" || rpc.calls[5].method != "bdev_fastblock_delete" {
		t.Fatalf("unexpected rollback calls: %+v", rpc.calls)
	}
}

func TestBuildRPCParamsHelpers(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	manager := newLocalManagerWithRPC(cfg, &stubCaller{})

	bdevParams := manager.buildCreateBdevParams(api.CreateExportRequest{
		PoolName:      "fb",
		ImageName:     "img-1",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
		BlockSize:     4096,
	}, "fbdev-exp1")
	if bdevParams["monitor_address"] != "10.0.0.20:3333" {
		t.Fatalf("unexpected bdev params: %+v", bdevParams)
	}

	subsystemParams := buildCreateSubsystemParams("nqn.test", "SERIAL1")
	if subsystemParams["allow_any_host"] != false {
		t.Fatalf("unexpected subsystem params: %+v", subsystemParams)
	}

	listenerParams := manager.buildListenerParams("rdma", "nqn.test")
	address := listenerParams["listen_address"].(map[string]any)
	if address["trtype"] != "RDMA" || address["traddr"] != "10.0.0.10" {
		t.Fatalf("unexpected listener params: %+v", listenerParams)
	}
}
