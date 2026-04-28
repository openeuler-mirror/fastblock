package config

import "testing"

func TestDefaultConfigNeedsNodeNameOnly(t *testing.T) {
	cfg := Default()
	cfg.NodeName = "node-a"
	cfg.TargetAddress = "10.0.0.10"
	if err := cfg.Validate(); err != nil {
		t.Fatalf("validate failed: %v", err)
	}
}
