package config

import "testing"

func TestDefaultConfigNeedsNodeNameOnly(t *testing.T) {
	cfg := Default()
	cfg.NodeName = "node-a"
	if err := cfg.Validate(); err != nil {
		t.Fatalf("validate failed: %v", err)
	}
}
