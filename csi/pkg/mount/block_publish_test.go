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
	err   error
}

func (r *stubCommandRunner) Run(_ context.Context, name string, args ...string) error {
	r.calls = append(r.calls, recordedCommand{name: name, args: append([]string{}, args...)})
	return r.err
}

func TestPublishAndUnpublishBlockDevice(t *testing.T) {
	runner := &stubCommandRunner{}
	targetPath := filepath.Join(t.TempDir(), "publish", "device")
	publisher := &BlockPublisher{
		runner: runner,
		readMountInfo: func() ([]byte, error) {
			return []byte("36 25 0:32 / " + targetPath + " rw,relatime - tmpfs tmpfs rw\n"), nil
		},
	}
	stagePath := t.TempDir()

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

func TestUnpublishUnmountedTargetRemovesFileWithoutUmount(t *testing.T) {
	runner := &stubCommandRunner{}
	targetPath := filepath.Join(t.TempDir(), "publish", "device")
	if err := os.MkdirAll(filepath.Dir(targetPath), 0o755); err != nil {
		t.Fatalf("mkdir publish dir failed: %v", err)
	}
	file, err := os.Create(targetPath)
	if err != nil {
		t.Fatalf("create target file failed: %v", err)
	}
	_ = file.Close()

	publisher := &BlockPublisher{
		runner: runner,
		readMountInfo: func() ([]byte, error) {
			return []byte(""), nil
		},
	}
	if err := publisher.UnpublishBlockDevice(context.Background(), targetPath); err != nil {
		t.Fatalf("unpublish unmounted target failed: %v", err)
	}
	if len(runner.calls) != 0 {
		t.Fatalf("expected no umount call for unmounted target, got %d", len(runner.calls))
	}
	if _, err := os.Stat(targetPath); !os.IsNotExist(err) {
		t.Fatalf("expected target path removed, stat err=%v", err)
	}
}

func TestUnpublishMountedTargetRunsUmount(t *testing.T) {
	runner := &stubCommandRunner{}
	targetPath := filepath.Join(t.TempDir(), "publish", "device")
	if err := os.MkdirAll(filepath.Dir(targetPath), 0o755); err != nil {
		t.Fatalf("mkdir publish dir failed: %v", err)
	}
	file, err := os.Create(targetPath)
	if err != nil {
		t.Fatalf("create target file failed: %v", err)
	}
	_ = file.Close()

	publisher := &BlockPublisher{
		runner: runner,
		readMountInfo: func() ([]byte, error) {
			return []byte("36 25 0:32 / " + targetPath + " rw,relatime - tmpfs tmpfs rw\n"), nil
		},
	}
	if err := publisher.UnpublishBlockDevice(context.Background(), targetPath); err != nil {
		t.Fatalf("unpublish mounted target failed: %v", err)
	}
	if len(runner.calls) != 1 {
		t.Fatalf("expected one umount call, got %d", len(runner.calls))
	}
	if runner.calls[0].name != "umount" || !reflect.DeepEqual(runner.calls[0].args, []string{targetPath}) {
		t.Fatalf("unexpected umount call: %#v", runner.calls[0])
	}
	if _, err := os.Stat(targetPath); !os.IsNotExist(err) {
		t.Fatalf("expected target path removed, stat err=%v", err)
	}
}

func TestIsMountedTargetUnescapesMountinfoPath(t *testing.T) {
	mounted, err := isMountedTarget("/var/lib/kubelet/pods/pod with space/device", func() ([]byte, error) {
		return []byte("36 25 0:32 / /var/lib/kubelet/pods/pod\\040with\\040space/device rw,relatime - tmpfs tmpfs rw\n"), nil
	})
	if err != nil {
		t.Fatalf("isMountedTarget failed: %v", err)
	}
	if !mounted {
		t.Fatal("expected escaped mountinfo path to match target")
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
