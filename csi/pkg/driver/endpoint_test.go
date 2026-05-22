package driver

import "testing"

func TestParseUnixEndpoint(t *testing.T) {
	ep, err := ParseEndpoint("unix:///var/lib/kubelet/plugins/csi.fastblock.io/controller.sock")
	if err != nil {
		t.Fatalf("parse endpoint failed: %v", err)
	}
	if ep.Network != "unix" || ep.Address != "/var/lib/kubelet/plugins/csi.fastblock.io/controller.sock" {
		t.Fatalf("unexpected endpoint: %+v", ep)
	}
}

func TestParseTCPEndpoint(t *testing.T) {
	ep, err := ParseEndpoint("tcp://127.0.0.1:9900")
	if err != nil {
		t.Fatalf("parse endpoint failed: %v", err)
	}
	if ep.Network != "tcp" || ep.Address != "127.0.0.1:9900" {
		t.Fatalf("unexpected endpoint: %+v", ep)
	}
}

func TestParseEndpointRejectsInvalidInput(t *testing.T) {
	if _, err := ParseEndpoint(""); err == nil {
		t.Fatal("expected empty endpoint error")
	}
	if _, err := ParseEndpoint("http://127.0.0.1"); err == nil {
		t.Fatal("expected unsupported scheme error")
	}
}
