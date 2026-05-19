package config

import (
	"errors"
	"fmt"
	"strings"
)

type Config struct {
	ListenAddress     string
	RPCSocketPath     string
	NodeName          string
	TargetAddress     string
	TargetServiceID   string
	SubsystemNQNPrefix string
}

func Default() Config {
	return Config{
		ListenAddress:      ":9500",
		RPCSocketPath:      "/var/tmp/fastblock_nvmf_tgt.sock",
		TargetServiceID:    "4420",
		SubsystemNQNPrefix: "nqn.2026-04.io.fastblock",
	}
}

func (c Config) Validate() error {
	if strings.TrimSpace(c.ListenAddress) == "" {
		return errors.New("listen address is required")
	}
	if strings.TrimSpace(c.RPCSocketPath) == "" {
		return errors.New("rpc socket path is required")
	}
	if strings.TrimSpace(c.NodeName) == "" {
		return fmt.Errorf("node name is required")
	}
	if strings.TrimSpace(c.TargetAddress) == "" {
		return errors.New("target address is required")
	}
	if strings.TrimSpace(c.TargetServiceID) == "" {
		return errors.New("target service id is required")
	}
	if strings.TrimSpace(c.SubsystemNQNPrefix) == "" {
		return errors.New("subsystem nqn prefix is required")
	}
	return nil
}
