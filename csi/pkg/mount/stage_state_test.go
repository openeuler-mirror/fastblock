package mount

import (
	"os"
	"path/filepath"
	"testing"
)

func TestWriteAndReadStageState(t *testing.T) {
	stagePath := t.TempDir()
	state := StageState{
		VolumeID:   "fbvolname:fb:img-a",
		DevicePath: "/dev/nvme0n1",
		Transport:  "rdma",
		NQN:        "nqn.test",
		Traddr:     "10.0.0.10",
		Trsvcid:    "4420",
		NSID:       1,
	}
	if err := WriteStageState(stagePath, state); err != nil {
		t.Fatalf("write stage state failed: %v", err)
	}
	readState, err := ReadStageState(stagePath)
	if err != nil {
		t.Fatalf("read stage state failed: %v", err)
	}
	if readState.DevicePath != "/dev/nvme0n1" || readState.NQN != "nqn.test" {
		t.Fatalf("unexpected stage state: %+v", readState)
	}
}

func TestRemoveStageState(t *testing.T) {
	stagePath := t.TempDir()
	if err := WriteStageState(stagePath, StageState{VolumeID: "fbvolname:fb:img-a"}); err != nil {
		t.Fatalf("write stage state failed: %v", err)
	}
	if err := RemoveStageState(stagePath); err != nil {
		t.Fatalf("remove stage state failed: %v", err)
	}
	if _, err := ReadStageState(stagePath); err == nil {
		t.Fatal("expected removed stage state to be unreadable")
	}
}

func TestWriteStageStateReplacesStaleFilePath(t *testing.T) {
	stageRoot := t.TempDir()
	stagePath := filepath.Join(stageRoot, "stage")
	if err := os.WriteFile(stagePath, []byte("stale"), 0o644); err != nil {
		t.Fatalf("write stale stage file failed: %v", err)
	}
	if err := WriteStageState(stagePath, StageState{VolumeID: "fbvolname:fb:img-a"}); err != nil {
		t.Fatalf("write stage state failed: %v", err)
	}
	info, err := os.Stat(stagePath)
	if err != nil {
		t.Fatalf("stat repaired stage path failed: %v", err)
	}
	if !info.IsDir() {
		t.Fatalf("expected repaired stage path to be directory, got mode %v", info.Mode())
	}
}
