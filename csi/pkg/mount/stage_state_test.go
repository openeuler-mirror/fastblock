package mount

import "testing"

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
