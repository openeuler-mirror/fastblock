package driver

import (
	"testing"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
)

func TestSingleNodeWriterBlockVolumeCapability(t *testing.T) {
	capability := SingleNodeWriterBlockVolumeCapability()
	if capability.GetBlock() == nil {
		t.Fatalf("expected block capability")
	}
	if capability.GetAccessMode().GetMode() != csi.VolumeCapability_AccessMode_SINGLE_NODE_WRITER {
		t.Fatalf("unexpected access mode: %+v", capability.GetAccessMode())
	}
}
