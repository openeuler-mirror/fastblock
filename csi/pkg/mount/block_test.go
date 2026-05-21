package mount

import "testing"

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
