package main

import (
	"context"
	"flag"
	"fmt"
	"os"

	"fastblock-exporter/pkg/config"
	"fastblock-exporter/pkg/nvmf"
	"fastblock-exporter/pkg/server"
)

func main() {
	cfg := config.Default()
	flag.StringVar(&cfg.ListenAddress, "listen", cfg.ListenAddress, "exporter listen address")
	flag.StringVar(&cfg.MonitorAddress, "monitor-address", "", "fastblock monitor address")
	flag.StringVar(&cfg.RPCSocketPath, "spdk-rpc-sock", cfg.RPCSocketPath, "SPDK RPC socket path")
	flag.StringVar(&cfg.NodeName, "node-name", "", "exporter node name")
	flag.StringVar(&cfg.TargetAddress, "target-address", "", "NVMe-oF target transport address")
	flag.StringVar(&cfg.TargetServiceID, "target-service-id", cfg.TargetServiceID, "NVMe-oF target service id")
	flag.StringVar(&cfg.SubsystemNQNPrefix, "nqn-prefix", cfg.SubsystemNQNPrefix, "NVMe-oF subsystem NQN prefix")
	flag.Parse()

	if err := cfg.Validate(); err != nil {
		fmt.Fprintf(os.Stderr, "invalid exporter config: %v\n", err)
		os.Exit(2)
	}

	srv := server.New(cfg, nvmf.NewLocalManager(cfg))
	if err := srv.Start(context.Background()); err != nil {
		fmt.Fprintf(os.Stderr, "exporter start failed: %v\n", err)
		os.Exit(1)
	}
}
