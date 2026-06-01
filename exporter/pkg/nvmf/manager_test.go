package nvmf

import (
	"context"
	"errors"
	"testing"

	"fastblock-exporter/pkg/api"
	"fastblock-exporter/pkg/config"
	"fastblock-exporter/pkg/spdkrpc"
)

type rpcCall struct {
	method string
	params map[string]any
}

type stubCaller struct {
	calls            []rpcCall
	callCount        map[string]int
	fail             map[string]error
	failSeq          map[string][]error
	getSubsystems    []subsystemInfo
	getSubsystemsSeq [][]subsystemInfo
}

func (c *stubCaller) Call(_ context.Context, method string, params any, result any) error {
	mapped, _ := params.(map[string]any)
	c.calls = append(c.calls, rpcCall{method: method, params: mapped})
	if c.callCount == nil {
		c.callCount = map[string]int{}
	}
	callIndex := c.callCount[method]
	c.callCount[method] = callIndex + 1
	if errs, ok := c.failSeq[method]; ok && callIndex < len(errs) && errs[callIndex] != nil {
		return errs[callIndex]
	}
	if err, ok := c.fail[method]; ok {
		return err
	}
	switch method {
	case "bdev_fastblock_register_existing":
		if out, ok := result.(*string); ok {
			*out = mapped["name"].(string)
		}
	case "nvmf_subsystem_add_ns":
		if out, ok := result.(*int); ok {
			*out = 11
		}
	case "nvmf_get_subsystems":
		if out, ok := result.(*[]subsystemInfo); ok {
			if callIndex < len(c.getSubsystemsSeq) {
				*out = append([]subsystemInfo(nil), c.getSubsystemsSeq[callIndex]...)
				return nil
			}
			*out = append([]subsystemInfo(nil), c.getSubsystems...)
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
	if len(rpc.calls) != 6 {
		t.Fatalf("unexpected call count: %d", len(rpc.calls))
	}
	if rpc.calls[0].method != "nvmf_get_subsystems" {
		t.Fatalf("unexpected first method: %s", rpc.calls[0].method)
	}
	if rpc.calls[1].method != "nvmf_create_transport" {
		t.Fatalf("unexpected second method: %s", rpc.calls[1].method)
	}
	if rpc.calls[2].method != "bdev_fastblock_register_existing" {
		t.Fatalf("unexpected third method: %s", rpc.calls[2].method)
	}
	if export.ID == "" || export.NQN == "" || export.NSID != 11 {
		t.Fatalf("unexpected export: %+v", export)
	}
}

func TestCreateExportReturnsExistingExport(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	rpc := &stubCaller{
		getSubsystems: []subsystemInfo{{
			NQN: "nqn.2026-04.io.fastblock:fbvol-cluster-a-1-7",
			Namespaces: []subsystemNS{{
				NSID: 11,
			}},
			ListenAddresses: []subsystemAddress{{
				Traddr:  "10.0.0.10",
				Trsvcid: "4420",
			}},
		}},
	}
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
	if len(rpc.calls) != 1 {
		t.Fatalf("expected existing export fast path, got %d calls", len(rpc.calls))
	}
	if rpc.calls[0].method != "nvmf_get_subsystems" {
		t.Fatalf("unexpected first method: %s", rpc.calls[0].method)
	}
	if export.ID == "" || export.NQN == "" || export.NSID != 11 {
		t.Fatalf("unexpected export: %+v", export)
	}
}

func TestCreateExportReusesExistingExportOnAlreadyExists(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	rpc := &stubCaller{
		failSeq: map[string][]error{
			"bdev_fastblock_register_existing": {
				&spdkrpc.ResponseError{Code: -17, Message: "bdev already exists"},
			},
		},
		getSubsystemsSeq: [][]subsystemInfo{
			nil,
			{{
				NQN: "nqn.2026-04.io.fastblock:fbvol-cluster-a-1-7",
				Namespaces: []subsystemNS{{
					NSID: 11,
				}},
				ListenAddresses: []subsystemAddress{{
					Traddr:  "10.0.0.10",
					Trsvcid: "4420",
				}},
			}},
		},
	}
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
	if export.ID != "fbvol-cluster-a-1-7" || export.NSID != 11 {
		t.Fatalf("unexpected export reuse result: %+v", export)
	}
	if len(rpc.calls) != 4 {
		t.Fatalf("unexpected call count: %d", len(rpc.calls))
	}
}

func TestCreateExportCleansStaleBdevAndRetries(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	rpc := &stubCaller{
		failSeq: map[string][]error{
			"bdev_fastblock_register_existing": {
				&spdkrpc.ResponseError{Code: -17, Message: "bdev already exists"},
				nil,
			},
		},
	}
	manager := newLocalManagerWithRPC(cfg, rpc)

	export, err := manager.CreateExport(context.Background(), api.CreateExportRequest{
		VolumeID:  "fbvol:cluster-a:1:7",
		PoolName:  "fb",
		ImageName: "img-7",
		BlockSize: 4096,
		Transport: "rdma",
	})
	if err != nil {
		t.Fatalf("create export failed: %v", err)
	}
	if export.ID != "fbvol-cluster-a-1-7" || export.NSID != 11 {
		t.Fatalf("unexpected export: %+v", export)
	}
	if len(rpc.calls) != 10 {
		t.Fatalf("unexpected call count: %d", len(rpc.calls))
	}
	if rpc.calls[1].method != "nvmf_create_transport" || rpc.calls[2].method != "bdev_fastblock_register_existing" || rpc.calls[4].method != "bdev_fastblock_delete" || rpc.calls[6].method != "bdev_fastblock_register_existing" {
		t.Fatalf("unexpected stale bdev recovery calls: %+v", rpc.calls)
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

func TestDeleteExportStillDeletesBdevWhenSubsystemMissing(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	rpc := &stubCaller{
		fail: map[string]error{
			"nvmf_delete_subsystem": &spdkrpc.ResponseError{Code: -19, Message: "No such device"},
		},
	}
	manager := newLocalManagerWithRPC(cfg, rpc)

	if err := manager.DeleteExport(context.Background(), "fbvol-cluster-a-1-7"); err != nil {
		t.Fatalf("delete export failed: %v", err)
	}
	if len(rpc.calls) != 2 {
		t.Fatalf("unexpected call count: %d", len(rpc.calls))
	}
	if rpc.calls[1].method != "bdev_fastblock_delete" {
		t.Fatalf("expected bdev delete after missing subsystem, got %+v", rpc.calls)
	}
}

func TestAllowHostIsIdempotent(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	rpc := &stubCaller{
		fail: map[string]error{
			"nvmf_subsystem_add_host": &spdkrpc.ResponseError{Code: -17, Message: "host already exists"},
		},
	}
	manager := newLocalManagerWithRPC(cfg, rpc)

	if err := manager.AllowHost(context.Background(), "fbvol-cluster-a-1-7", "nqn.host.1"); err != nil {
		t.Fatalf("allow host should be idempotent: %v", err)
	}
}

func TestDenyHostIsIdempotent(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	rpc := &stubCaller{
		fail: map[string]error{
			"nvmf_subsystem_remove_host": &spdkrpc.ResponseError{Code: -2, Message: "host not found"},
		},
	}
	manager := newLocalManagerWithRPC(cfg, rpc)

	if err := manager.DenyHost(context.Background(), "fbvol-cluster-a-1-7", "nqn.host.1"); err != nil {
		t.Fatalf("deny host should be idempotent: %v", err)
	}
}

func TestDenyHostTreatsInvalidParamsAsIdempotent(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	rpc := &stubCaller{
		fail: map[string]error{
			"nvmf_subsystem_remove_host": &spdkrpc.ResponseError{Code: -32602, Message: "Invalid parameters"},
		},
	}
	manager := newLocalManagerWithRPC(cfg, rpc)

	if err := manager.DenyHost(context.Background(), "fbvol-cluster-a-1-7", "nqn.host.1"); err != nil {
		t.Fatalf("deny host should treat invalid params as idempotent: %v", err)
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
	if len(rpc.calls) != 8 {
		t.Fatalf("unexpected call count: %d", len(rpc.calls))
	}
	if rpc.calls[6].method != "nvmf_delete_subsystem" || rpc.calls[7].method != "bdev_fastblock_delete" {
		t.Fatalf("unexpected rollback calls: %+v", rpc.calls)
	}
}

func TestBuildRPCParamsHelpers(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	manager := newLocalManagerWithRPC(cfg, &stubCaller{})

	bdevParams := manager.buildRegisterExistingBdevParams(api.CreateExportRequest{
		PoolName:      "fb",
		ImageName:     "img-1",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
		BlockSize:     4096,
	}, "fbdev-exp1")
	if bdevParams["monitor_address"] != "10.0.0.20:3333" {
		t.Fatalf("unexpected bdev params: %+v", bdevParams)
	}
	if _, ok := bdevParams["image_size"]; ok {
		t.Fatalf("register existing params should not include image_size: %+v", bdevParams)
	}
	if _, ok := bdevParams["object_size"]; ok {
		t.Fatalf("register existing params should not include object_size: %+v", bdevParams)
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

	transportParams := buildTransportParams("tcp")
	if transportParams["trtype"] != "TCP" || transportParams["max_io_size"] != 131072 {
		t.Fatalf("unexpected transport params: %+v", transportParams)
	}
}

func TestGetExport(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	rpc := &stubCaller{
		getSubsystems: []subsystemInfo{{
			NQN: "nqn.2026-04.io.fastblock:fbvol-cluster-a-1-7",
			Namespaces: []subsystemNS{{
				NSID: 11,
			}},
			ListenAddresses: []subsystemAddress{{
				Traddr:  "10.0.0.10",
				Trsvcid: "4420",
			}},
		}},
	}
	manager := newLocalManagerWithRPC(cfg, rpc)

	export, err := manager.GetExport(context.Background(), "fbvol-cluster-a-1-7")
	if err != nil {
		t.Fatalf("get export failed: %v", err)
	}
	if export.ID != "fbvol-cluster-a-1-7" || export.NSID != 11 {
		t.Fatalf("unexpected export: %+v", export)
	}
}

func TestListExports(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	rpc := &stubCaller{
		getSubsystems: []subsystemInfo{
			{
				NQN: "nqn.2026-04.io.fastblock:fbvol-cluster-a-1-7",
				Namespaces: []subsystemNS{{NSID: 11}},
				ListenAddresses: []subsystemAddress{{Traddr: "10.0.0.10", Trsvcid: "4420"}},
			},
			{
				NQN: "nqn.other:test",
				Namespaces: []subsystemNS{{NSID: 1}},
				ListenAddresses: []subsystemAddress{{Traddr: "127.0.0.1", Trsvcid: "4420"}},
			},
		},
	}
	manager := newLocalManagerWithRPC(cfg, rpc)

	exports, err := manager.ListExports(context.Background())
	if err != nil {
		t.Fatalf("list exports failed: %v", err)
	}
	if len(exports) != 1 || exports[0].ID != "fbvol-cluster-a-1-7" {
		t.Fatalf("unexpected exports: %+v", exports)
	}
}

func TestGetExportTreatsSPDKNotFoundAsErrExportNotFound(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	rpc := &stubCaller{
		fail: map[string]error{
			"nvmf_get_subsystems": &spdkrpc.ResponseError{Code: -19, Message: "No such device"},
		},
	}
	manager := newLocalManagerWithRPC(cfg, rpc)

	_, err := manager.GetExport(context.Background(), "fbvol-cluster-a-1-7")
	if !errors.Is(err, ErrExportNotFound) {
		t.Fatalf("expected ErrExportNotFound, got %v", err)
	}
}

func TestCreateExportRecreatesIncompleteSubsystem(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	rpc := &stubCaller{
		getSubsystems: []subsystemInfo{{
			NQN:        "nqn.2026-04.io.fastblock:fbvol-cluster-a-1-7",
			Namespaces: nil,
			ListenAddresses: []subsystemAddress{{
				Traddr:  "10.0.0.10",
				Trsvcid: "4420",
			}},
		}},
	}
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
	if export.ID == "" || export.NSID != 11 {
		t.Fatalf("unexpected export: %+v", export)
	}
	if len(rpc.calls) < 8 {
		t.Fatalf("expected cleanup and recreate calls, got %d", len(rpc.calls))
	}
	if rpc.calls[1].method != "nvmf_delete_subsystem" || rpc.calls[2].method != "bdev_fastblock_delete" {
		t.Fatalf("expected stale export cleanup before recreate, got %+v", rpc.calls)
	}
}

func TestEnsureTransportIsIdempotent(t *testing.T) {
	cfg := config.Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.TargetAddress = "10.0.0.10"
	cfg.NodeName = "node-a"
	rpc := &stubCaller{
		fail: map[string]error{
			"nvmf_create_transport": &spdkrpc.ResponseError{Code: -17, Message: "transport already exists"},
		},
	}
	manager := newLocalManagerWithRPC(cfg, rpc)

	if err := manager.ensureTransport(context.Background(), "rdma"); err != nil {
		t.Fatalf("ensure transport should be idempotent: %v", err)
	}
}
