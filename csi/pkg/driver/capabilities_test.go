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
	caps := ControllerServiceCapabilities(false)
	if len(caps) != 3 {
		t.Fatalf("unexpected controller capability count: %d", len(caps))
	}
	if caps[0].GetRpc().GetType() != csi.ControllerServiceCapability_RPC_CREATE_DELETE_VOLUME {
		t.Fatalf("unexpected first controller capability: %+v", caps[0])
	}
	if caps[1].GetRpc().GetType() != csi.ControllerServiceCapability_RPC_PUBLISH_UNPUBLISH_VOLUME {
		t.Fatalf("unexpected second controller capability: %+v", caps[1])
	}
	if caps[2].GetRpc().GetType() != csi.ControllerServiceCapability_RPC_EXPAND_VOLUME {
		t.Fatalf("unexpected third controller capability: %+v", caps[2])
	}

	withSnapshots := ControllerServiceCapabilities(true)
	if len(withSnapshots) != 5 {
		t.Fatalf("unexpected snapshot controller capability count: %d", len(withSnapshots))
	}
	if withSnapshots[3].GetRpc().GetType() != csi.ControllerServiceCapability_RPC_CREATE_DELETE_SNAPSHOT {
		t.Fatalf("unexpected snapshot create/delete capability: %+v", withSnapshots[3])
	}
	if withSnapshots[4].GetRpc().GetType() != csi.ControllerServiceCapability_RPC_LIST_SNAPSHOTS {
		t.Fatalf("unexpected snapshot list capability: %+v", withSnapshots[4])
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
