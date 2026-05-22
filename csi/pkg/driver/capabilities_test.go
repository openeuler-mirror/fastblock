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
	if len(caps) != 3 {
		t.Fatalf("unexpected controller capability count: %d", len(caps))
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
