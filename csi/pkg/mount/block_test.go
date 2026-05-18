package mount

import (
	"os"
	"path/filepath"
	"testing"
)

func TestValidateBlockPublishTarget(t *testing.T) {
	if err := ValidateBlockPublishTarget("/dev/nvme0n1", "/var/lib/kubelet/plugins/kubernetes.io/csi/pv/stage", "/var/lib/kubelet/pods/pod/volumeDevices/publish"); err != nil {
		t.Fatalf("unexpected validation error: %v", err)
	}
	if err := ValidateBlockPublishTarget("/dev/nvme0n1", "/stage", "target"); err == nil {
		t.Fatal("expected target path validation error")
	}
	if err := ValidateBlockPublishTarget("/dev/nvme0n1", "/same", "/same"); err == nil {
		t.Fatal("expected same path validation error")
	}
}

func TestCanonicalStageDevicePath(t *testing.T) {
	path, err := CanonicalStageDevicePath("/var/lib/kubelet/plugins/kubernetes.io/csi/pv/stage")
	if err != nil {
		t.Fatalf("unexpected canonical path error: %v", err)
	}
	if path != "/var/lib/kubelet/plugins/kubernetes.io/csi/pv/stage/device" {
		t.Fatalf("unexpected canonical path: %s", path)
	}
	if _, err := CanonicalStageDevicePath("stage"); err == nil {
		t.Fatal("expected relative path error")
	}
}

func TestCanonicalPublishDevicePath(t *testing.T) {
	path, err := CanonicalPublishDevicePath("/var/lib/kubelet/pods/pod/volumeDevices/publish")
	if err != nil {
		t.Fatalf("unexpected canonical publish path error: %v", err)
	}
	if path != "/var/lib/kubelet/pods/pod/volumeDevices/publish/device" {
		t.Fatalf("unexpected canonical publish path: %s", path)
	}
	if _, err := CanonicalPublishDevicePath("publish"); err == nil {
		t.Fatal("expected relative publish path error")
	}
}

func TestWriteAndRemoveStageDeviceLink(t *testing.T) {
	stagePath := t.TempDir()
	devicePath := filepath.Join(t.TempDir(), "nvme0n1")
	file, err := os.Create(devicePath)
	if err != nil {
		t.Fatalf("create fake device path failed: %v", err)
	}
	_ = file.Close()
	if err := WriteStageDeviceLink(stagePath, devicePath); err != nil {
		t.Fatalf("write stage device link failed: %v", err)
	}
	path, _ := CanonicalStageDevicePath(stagePath)
	info, err := os.Lstat(path)
	if err != nil {
		t.Fatalf("lstat stage device link failed: %v", err)
	}
	if info.Mode()&os.ModeSymlink == 0 {
		t.Fatalf("expected stage device path to be a symlink")
	}
	if err := RemoveStageDeviceLink(stagePath); err != nil {
		t.Fatalf("remove stage device link failed: %v", err)
	}
	if _, err := os.Lstat(path); !os.IsNotExist(err) {
		t.Fatalf("expected stage device link removal, err=%v", err)
	}
}

func TestWriteStageDeviceLinkReplacesStaleFilePath(t *testing.T) {
	stageRoot := t.TempDir()
	stagePath := filepath.Join(stageRoot, "stage")
	devicePath := filepath.Join(t.TempDir(), "nvme0n1")
	file, err := os.Create(devicePath)
	if err != nil {
		t.Fatalf("create fake device path failed: %v", err)
	}
	_ = file.Close()
	if err := os.WriteFile(stagePath, []byte("stale"), 0o644); err != nil {
		t.Fatalf("write stale stage file failed: %v", err)
	}
	if err := WriteStageDeviceLink(stagePath, devicePath); err != nil {
		t.Fatalf("write stage device link failed: %v", err)
	}
	info, err := os.Stat(stagePath)
	if err != nil {
		t.Fatalf("stat repaired stage path failed: %v", err)
	}
	if !info.IsDir() {
		t.Fatalf("expected repaired stage path to be directory, got mode %v", info.Mode())
	}
}

func TestWriteStageDeviceLinkFallsBackToBlockDeviceNode(t *testing.T) {
	stagePath := t.TempDir()
	sysClassBlockRoot := filepath.Join(t.TempDir(), "sys", "class", "block")
	deviceDir := filepath.Join(sysClassBlockRoot, "nvme0n1")
	if err := os.MkdirAll(deviceDir, 0o755); err != nil {
		t.Fatalf("mkdir fake sysfs failed: %v", err)
	}
	if err := os.WriteFile(filepath.Join(deviceDir, "dev"), []byte("259:1\n"), 0o644); err != nil {
		t.Fatalf("write fake major/minor failed: %v", err)
	}
	if err := writeStageDeviceLinkWithSysfsRoot(stagePath, "/dev/nvme0n1", sysClassBlockRoot); err != nil {
		t.Fatalf("write stage device node failed: %v", err)
	}
	path, _ := CanonicalStageDevicePath(stagePath)
	info, err := os.Lstat(path)
	if err != nil {
		t.Fatalf("lstat stage device path failed: %v", err)
	}
	if info.Mode()&os.ModeDevice == 0 {
		t.Fatalf("expected block device node, got mode %v", info.Mode())
	}
}
