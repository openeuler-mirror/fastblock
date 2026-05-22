package driver

import csi "github.com/container-storage-interface/spec/lib/go/csi"

func PluginCapabilities() []*csi.PluginCapability {
	return []*csi.PluginCapability{
		{
			Type: &csi.PluginCapability_Service_{
				Service: &csi.PluginCapability_Service{
					Type: csi.PluginCapability_Service_CONTROLLER_SERVICE,
				},
			},
		},
	}
}

func ControllerServiceCapabilities() []*csi.ControllerServiceCapability {
	rpcs := []csi.ControllerServiceCapability_RPC_Type{
		csi.ControllerServiceCapability_RPC_CREATE_DELETE_VOLUME,
		csi.ControllerServiceCapability_RPC_PUBLISH_UNPUBLISH_VOLUME,
		csi.ControllerServiceCapability_RPC_EXPAND_VOLUME,
	}

	caps := make([]*csi.ControllerServiceCapability, 0, len(rpcs))
	for _, rpc := range rpcs {
		caps = append(caps, &csi.ControllerServiceCapability{
			Type: &csi.ControllerServiceCapability_Rpc{
				Rpc: &csi.ControllerServiceCapability_RPC{
					Type: rpc,
				},
			},
		})
	}
	return caps
}

func NodeServiceCapabilities() []*csi.NodeServiceCapability {
	rpcs := []csi.NodeServiceCapability_RPC_Type{
		csi.NodeServiceCapability_RPC_STAGE_UNSTAGE_VOLUME,
	}

	caps := make([]*csi.NodeServiceCapability, 0, len(rpcs))
	for _, rpc := range rpcs {
		caps = append(caps, &csi.NodeServiceCapability{
			Type: &csi.NodeServiceCapability_Rpc{
				Rpc: &csi.NodeServiceCapability_RPC{
					Type: rpc,
				},
			},
		})
	}
	return caps
}
