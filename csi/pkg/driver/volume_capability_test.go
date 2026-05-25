package driver

import (
	"testing"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
)

func TestIsSupportedVolumeCapability(t *testing.T) {
	supported := &csi.VolumeCapability{
		AccessType: &csi.VolumeCapability_Block{Block: &csi.VolumeCapability_BlockVolume{}},
		AccessMode: &csi.VolumeCapability_AccessMode{Mode: csi.VolumeCapability_AccessMode_SINGLE_NODE_WRITER},
	}
	if !IsSupportedVolumeCapability(supported) {
		t.Fatal("expected block single-node-writer to be supported")
	}

	unsupported := &csi.VolumeCapability{
		AccessType: &csi.VolumeCapability_Mount{Mount: &csi.VolumeCapability_MountVolume{}},
		AccessMode: &csi.VolumeCapability_AccessMode{Mode: csi.VolumeCapability_AccessMode_SINGLE_NODE_WRITER},
	}
	if IsSupportedVolumeCapability(unsupported) {
		t.Fatal("expected mount capability to be unsupported")
	}
}

func TestAreSupportedVolumeCapabilities(t *testing.T) {
	if AreSupportedVolumeCapabilities(nil) {
		t.Fatal("expected empty capabilities to be unsupported")
	}
}
