package config

import "testing"

func TestDefaultConfigNeedsNodeNameOnly(t *testing.T) {
	cfg := Default()
	cfg.MonitorAddress = "10.0.0.20:3333"
	cfg.NodeName = "node-a"
	cfg.TargetAddress = "10.0.0.10"
	if err := cfg.Validate(); err != nil {
		t.Fatalf("validate failed: %v", err)
	}
}
