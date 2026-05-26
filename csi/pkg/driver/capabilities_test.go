package driver

import (
	"testing"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
)

func TestPluginCapabilities(t *testing.T) {
	caps := PluginCapabilities()
	if len(caps) != 1 {
		t.Fatalf("unexpected plugin capability count: %d", len(caps))
	}
	service := caps[0].GetService()
	if service == nil || service.GetType() != csi.PluginCapability_Service_CONTROLLER_SERVICE {
		t.Fatalf("unexpected plugin capability: %+v", caps[0])
	}
}

func TestControllerServiceCapabilities(t *testing.T) {
	caps := ControllerServiceCapabilities()
	if len(caps) != 2 {
		t.Fatalf("unexpected controller capability count: %d", len(caps))
	}
	if caps[0].GetRpc().GetType() != csi.ControllerServiceCapability_RPC_CREATE_DELETE_VOLUME {
		t.Fatalf("unexpected first controller capability: %+v", caps[0])
	}
	if caps[1].GetRpc().GetType() != csi.ControllerServiceCapability_RPC_PUBLISH_UNPUBLISH_VOLUME {
		t.Fatalf("unexpected second controller capability: %+v", caps[1])
	}
}

func TestNodeServiceCapabilities(t *testing.T) {
	caps := NodeServiceCapabilities()
	if len(caps) != 1 {
		t.Fatalf("unexpected node capability count: %d", len(caps))
	}
	if caps[0].GetRpc().GetType() != csi.NodeServiceCapability_RPC_STAGE_UNSTAGE_VOLUME {
		t.Fatalf("unexpected node capability: %+v", caps[0])
	}
}
