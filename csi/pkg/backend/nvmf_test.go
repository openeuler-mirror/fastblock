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
	backend := newTestBackend(root, runner)

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
	backend := newTestBackend(root, runner)

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

func newTestBackend(root string, runner *stubRunner) *NVMFBackend {
	backend := NewNVMF()
	backend.runner = runner
	backend.DeviceRoot = filepath.Join(root, "dev")
	backend.SysClassNVMe = filepath.Join(root, "sys", "class", "nvme")
	backend.SysClassBlock = filepath.Join(root, "sys", "class", "block")
	backend.ConnectTimeout = time.Second
	backend.PollInterval = 10 * time.Millisecond
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
