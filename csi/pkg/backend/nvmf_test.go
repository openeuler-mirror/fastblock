package backend

import (
	"context"
	"os"
	"path/filepath"
	"reflect"
	"testing"
	"time"
)

type recordedCall struct {
	name string
	args []string
}

type stubRunner struct {
	calls []recordedCall
	run   func()
}

func (r *stubRunner) Run(_ context.Context, name string, args ...string) error {
	r.calls = append(r.calls, recordedCall{name: name, args: append([]string{}, args...)})
	if r.run != nil {
		r.run()
	}
	return nil
}

func TestStageSkipsConnectWhenDeviceIsReady(t *testing.T) {
	root := t.TempDir()
	prepareFakeDevice(t, root, "nqn.test", "3")
	runner := &stubRunner{}
	backend := newTestBackend(t, root, runner)

	device, err := backend.Stage(context.Background(), "vol-1", VolumeContext{
		Transport: "rdma",
		NQN:       "nqn.test",
		Traddr:    "10.0.0.10",
		Trsvcid:   "4420",
		NSID:      3,
	})
	if err != nil {
		t.Fatalf("stage failed: %v", err)
	}
	if device != filepath.Join(root, "dev", "nvme0n1") {
		t.Fatalf("unexpected device path: %s", device)
	}
	if len(runner.calls) != 0 {
		t.Fatalf("expected no connect call, got %d", len(runner.calls))
	}
}

func TestStageConnectsAndFindsDevice(t *testing.T) {
	root := t.TempDir()
	runner := &stubRunner{run: func() { prepareFakeDevice(t, root, "nqn.test", "3") }}
	backend := newTestBackend(t, root, runner)

	device, err := backend.Stage(context.Background(), "vol-1", VolumeContext{
		Transport: "rdma",
		NQN:       "nqn.test",
		Traddr:    "10.0.0.10",
		Trsvcid:   "4420",
		NSID:      3,
	})
	if err != nil {
		t.Fatalf("stage failed: %v", err)
	}
	if device != filepath.Join(root, "dev", "nvme0n1") {
		t.Fatalf("unexpected device path: %s", device)
	}
	if len(runner.calls) != 1 {
		t.Fatalf("unexpected command count: %d", len(runner.calls))
	}
	want := []string{"connect", "-t", "rdma", "-n", "nqn.test", "-a", "10.0.0.10", "-s", "4420"}
	if !reflect.DeepEqual(runner.calls[0].args, want) {
		t.Fatalf("unexpected connect args: %#v", runner.calls[0].args)
	}
}

func prepareFakeDevice(t *testing.T, root, nqn, nsid string) {
	t.Helper()
	sysNVMe := filepath.Join(root, "sys", "class", "nvme", "nvme0")
	sysBlock := filepath.Join(root, "sys", "class", "block", "nvme0n1")
	if err := os.MkdirAll(sysNVMe, 0o755); err != nil {
		t.Fatalf("mkdir nvme failed: %v", err)
	}
	if err := os.MkdirAll(sysBlock, 0o755); err != nil {
		t.Fatalf("mkdir block failed: %v", err)
	}
	if err := os.WriteFile(filepath.Join(sysNVMe, "subsysnqn"), []byte(nqn+"\n"), 0o644); err != nil {
		t.Fatalf("write subsysnqn failed: %v", err)
	}
	if err := os.WriteFile(filepath.Join(sysBlock, "nsid"), []byte(nsid+"\n"), 0o644); err != nil {
		t.Fatalf("write nsid failed: %v", err)
	}
}

func newTestBackend(t *testing.T, root string, runner *stubRunner) *NVMFBackend {
	t.Helper()
	backend := NewNVMF()
	backend.runner = runner
	backend.DeviceRoot = filepath.Join(root, "dev")
	backend.SysClassNVMe = filepath.Join(root, "sys", "class", "nvme")
	backend.SysClassBlock = filepath.Join(root, "sys", "class", "block")
	backend.SysModuleRoot = filepath.Join(root, "sys", "module")
	backend.HostNQNPath = filepath.Join(root, "etc", "nvme", "hostnqn")
	backend.lookPath = func(name string) (string, error) { return "/usr/sbin/" + name, nil }
	backend.ConnectTimeout = time.Second
	backend.PollInterval = 10 * time.Millisecond
	prepareHostEnvironment(t, root, "nqn.2014-08.org.nvmexpress:uuid:test")
	return backend
}

func TestUnstage(t *testing.T) {
	runner := &stubRunner{}
	backend := NewNVMF()
	backend.runner = runner

	err := backend.Unstage(context.Background(), "vol-1", VolumeContext{
		Transport: "tcp",
		NQN:       "nqn.test",
		Traddr:    "10.0.0.10",
		Trsvcid:   "4420",
		NSID:      1,
	})
	if err != nil {
		t.Fatalf("unstage failed: %v", err)
	}
	if len(runner.calls) != 1 {
		t.Fatalf("unexpected command count: %d", len(runner.calls))
	}
	want := []string{"disconnect", "-n", "nqn.test"}
	if !reflect.DeepEqual(runner.calls[0].args, want) {
		t.Fatalf("unexpected disconnect args: %#v", runner.calls[0].args)
	}
}

func TestStageFailsWhenHostEnvironmentIsMissing(t *testing.T) {
	root := t.TempDir()
	backend := newTestBackend(t, root, &stubRunner{})
	if err := os.Remove(backend.HostNQNPath); err != nil {
		t.Fatalf("remove hostnqn failed: %v", err)
	}
	if _, err := backend.Stage(context.Background(), "vol-1", VolumeContext{
		Transport: "rdma",
		NQN:       "nqn.test",
		Traddr:    "10.0.0.10",
		Trsvcid:   "4420",
		NSID:      3,
	}); err == nil {
		t.Fatal("expected missing hostnqn to fail preflight")
	}
}

func TestTransportModuleHelper(t *testing.T) {
	if transportModule("rdma") != "nvme_rdma" {
		t.Fatalf("unexpected rdma module: %q", transportModule("rdma"))
	}
	if transportModule("tcp") != "nvme_tcp" {
		t.Fatalf("unexpected tcp module: %q", transportModule("tcp"))
	}
	if transportModule("bad") != "" {
		t.Fatalf("unexpected module for bad transport: %q", transportModule("bad"))
	}
}

func prepareHostEnvironment(t *testing.T, root, hostNQN string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Join(root, "etc", "nvme"), 0o755); err != nil {
		t.Fatalf("mkdir hostnqn dir failed: %v", err)
	}
	if err := os.WriteFile(filepath.Join(root, "etc", "nvme", "hostnqn"), []byte(hostNQN+"\n"), 0o644); err != nil {
		t.Fatalf("write hostnqn failed: %v", err)
	}
	if err := os.MkdirAll(filepath.Join(root, "sys", "module", "nvme_rdma"), 0o755); err != nil {
		t.Fatalf("mkdir nvme_rdma failed: %v", err)
	}
	if err := os.MkdirAll(filepath.Join(root, "sys", "module", "nvme_tcp"), 0o755); err != nil {
		t.Fatalf("mkdir nvme_tcp failed: %v", err)
	}
}

func TestCommandArgHelpers(t *testing.T) {
	connectArgs := buildConnectArgs(VolumeContext{
		Transport: "tcp",
		NQN:       "nqn.test",
		Traddr:    "10.0.0.10",
		Trsvcid:   "4420",
	})
	if !reflect.DeepEqual(connectArgs, []string{"connect", "-t", "tcp", "-n", "nqn.test", "-a", "10.0.0.10", "-s", "4420"}) {
		t.Fatalf("unexpected connect args: %#v", connectArgs)
	}
	disconnectArgs := buildDisconnectArgs("nqn.test")
	if !reflect.DeepEqual(disconnectArgs, []string{"disconnect", "-n", "nqn.test"}) {
		t.Fatalf("unexpected disconnect args: %#v", disconnectArgs)
	}
}
