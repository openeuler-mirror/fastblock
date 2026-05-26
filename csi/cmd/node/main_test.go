package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestResolveNodeIDPrefersExplicitValue(t *testing.T) {
	nodeID, err := resolveNodeID("node-a", "/does/not/matter")
	if err != nil {
		t.Fatalf("resolve node id failed: %v", err)
	}
	if nodeID != "node-a" {
		t.Fatalf("unexpected node id %q", nodeID)
	}
}

func TestResolveNodeIDFallsBackToFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "hostnqn")
	if err := os.WriteFile(path, []byte("nqn.2014-08.org.nvmexpress:uuid:test\n"), 0o644); err != nil {
		t.Fatalf("write hostnqn failed: %v", err)
	}
	nodeID, err := resolveNodeID("", path)
	if err != nil {
		t.Fatalf("resolve node id failed: %v", err)
	}
	if nodeID != "nqn.2014-08.org.nvmexpress:uuid:test" {
		t.Fatalf("unexpected node id %q", nodeID)
	}
}

func TestResolveNodeIDRejectsEmptyFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "hostnqn")
	if err := os.WriteFile(path, []byte(" \n"), 0o644); err != nil {
		t.Fatalf("write hostnqn failed: %v", err)
	}
	if _, err := resolveNodeID("", path); err == nil {
		t.Fatal("expected empty hostnqn file to fail")
	}
}
