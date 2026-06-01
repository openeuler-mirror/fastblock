package mount

import (
	"context"
	"os"
	"path/filepath"
	"reflect"
	"testing"
)

type recordedCommand struct {
	name string
	args []string
}

type stubCommandRunner struct {
	calls []recordedCommand
}

func (r *stubCommandRunner) Run(_ context.Context, name string, args ...string) error {
	r.calls = append(r.calls, recordedCommand{name: name, args: append([]string{}, args...)})
	return nil
}

func TestPublishAndUnpublishBlockDevice(t *testing.T) {
	runner := &stubCommandRunner{}
	publisher := &BlockPublisher{runner: runner}
	stagePath := t.TempDir()
	targetPath := filepath.Join(t.TempDir(), "publish", "device")

	if err := publisher.PublishBlockDevice(context.Background(), "/dev/nvme0n1", stagePath, targetPath); err != nil {
		t.Fatalf("publish block device failed: %v", err)
	}
	if _, err := os.Stat(targetPath); err != nil {
		t.Fatalf("expected target file to exist: %v", err)
	}
	if len(runner.calls) != 1 {
		t.Fatalf("unexpected publish call count: %d", len(runner.calls))
	}
	if !reflect.DeepEqual(runner.calls[0].args, []string{"--bind", "/dev/nvme0n1", targetPath}) {
		t.Fatalf("unexpected publish args: %#v", runner.calls[0].args)
	}

	if err := publisher.UnpublishBlockDevice(context.Background(), targetPath); err != nil {
		t.Fatalf("unpublish block device failed: %v", err)
	}
	if len(runner.calls) != 2 {
		t.Fatalf("unexpected total call count: %d", len(runner.calls))
	}
	if runner.calls[1].name != "umount" || !reflect.DeepEqual(runner.calls[1].args, []string{targetPath}) {
		t.Fatalf("unexpected unpublish args: %#v", runner.calls[1])
	}
	if _, err := os.Stat(targetPath); !os.IsNotExist(err) {
		t.Fatalf("expected target path to be removed, stat err=%v", err)
	}
}

func TestUnpublishMissingTargetIsNoop(t *testing.T) {
	publisher := &BlockPublisher{runner: &stubCommandRunner{}}
	targetPath := filepath.Join(t.TempDir(), "missing")
	if err := publisher.UnpublishBlockDevice(context.Background(), targetPath); err != nil {
		t.Fatalf("unexpected error for missing target: %v", err)
	}
}

func TestPublishBlockDeviceAlreadyPublishedIsNoop(t *testing.T) {
	runner := &stubCommandRunner{}
	publisher := &BlockPublisher{runner: runner}
	stagePath := t.TempDir()
	devicePath := filepath.Join(t.TempDir(), "device")
	targetPath := filepath.Join(t.TempDir(), "publish", "device")

	file, err := os.Create(devicePath)
	if err != nil {
		t.Fatalf("create device file failed: %v", err)
	}
	_ = file.Close()
	if err := os.MkdirAll(filepath.Dir(targetPath), 0o755); err != nil {
		t.Fatalf("mkdir publish dir failed: %v", err)
	}
	if err := os.Link(devicePath, targetPath); err != nil {
		t.Fatalf("link target to device failed: %v", err)
	}

	if err := publisher.PublishBlockDevice(context.Background(), devicePath, stagePath, targetPath); err != nil {
		t.Fatalf("expected already published target to be a noop, got %v", err)
	}
	if len(runner.calls) != 0 {
		t.Fatalf("expected no mount call for already published target, got %d", len(runner.calls))
	}
}
