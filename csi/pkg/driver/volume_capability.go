package driver

import csi "github.com/container-storage-interface/spec/lib/go/csi"

func IsSupportedVolumeCapability(capability *csi.VolumeCapability) bool {
	if capability == nil {
		return false
	}
	if capability.GetBlock() == nil {
		return false
	}
	mode := capability.GetAccessMode()
	if mode == nil {
		return false
	}
	return mode.GetMode() == csi.VolumeCapability_AccessMode_SINGLE_NODE_WRITER
}

func AreSupportedVolumeCapabilities(capabilities []*csi.VolumeCapability) bool {
	if len(capabilities) == 0 {
		return false
	}
	for _, capability := range capabilities {
		if !IsSupportedVolumeCapability(capability) {
			return false
		}
	}
	return true
}
