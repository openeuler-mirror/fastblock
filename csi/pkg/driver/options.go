package driver

import (
	"errors"
	"fmt"
	"strings"
)

const (
	DefaultDriverName = "csi.fastblock.io"
	ModeController    = "controller"
	ModeNode          = "node"
)

type Options struct {
	DriverName string
	Endpoint   string
	NodeID     string
	Mode       string
}

func (o Options) Validate() error {
	if strings.TrimSpace(o.DriverName) == "" {
		return errors.New("driver name is required")
	}
	if strings.TrimSpace(o.Endpoint) == "" {
		return errors.New("endpoint is required")
	}
	if o.Mode != ModeController && o.Mode != ModeNode {
		return fmt.Errorf("unsupported mode %q", o.Mode)
	}
	if o.Mode == ModeNode && strings.TrimSpace(o.NodeID) == "" {
		return errors.New("node id is required in node mode")
	}
	return nil
}
