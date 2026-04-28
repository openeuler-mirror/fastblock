package config

import (
	"errors"
	"fmt"
	"strings"
)

type Config struct {
	ListenAddress string
	RPCSocketPath string
	NodeName      string
}

func Default() Config {
	return Config{
		ListenAddress: ":9500",
		RPCSocketPath: "/var/tmp/fastblock_nvmf_tgt.sock",
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
	return nil
}
