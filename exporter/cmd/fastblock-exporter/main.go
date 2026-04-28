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
	flag.StringVar(&cfg.RPCSocketPath, "spdk-rpc-sock", cfg.RPCSocketPath, "SPDK RPC socket path")
	flag.StringVar(&cfg.NodeName, "node-name", "", "exporter node name")
	flag.Parse()

	if err := cfg.Validate(); err != nil {
		fmt.Fprintf(os.Stderr, "invalid exporter config: %v\n", err)
		os.Exit(2)
	}

	srv := server.New(cfg, nvmf.NewLocalManager(cfg.RPCSocketPath))
	if err := srv.Start(context.Background()); err != nil {
		fmt.Fprintf(os.Stderr, "exporter start failed: %v\n", err)
		os.Exit(1)
	}
}
