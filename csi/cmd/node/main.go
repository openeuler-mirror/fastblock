package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"syscall"

	"fastblock-csi/pkg/backend"
	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/node"
)

func main() {
	var endpoint string
	var driverName string
	var nodeID string

	flag.StringVar(&endpoint, "endpoint", "unix:///var/lib/kubelet/plugins/csi.fastblock.io/node.sock", "CSI node endpoint")
	flag.StringVar(&driverName, "driver-name", driver.DefaultDriverName, "CSI driver name")
	flag.StringVar(&nodeID, "node-id", "", "Kubernetes node id")
	flag.Parse()

	opts := driver.Options{
		DriverName: driverName,
		Endpoint:   endpoint,
		NodeID:     nodeID,
		Mode:       driver.ModeNode,
	}
	if err := opts.Validate(); err != nil {
		fmt.Fprintf(os.Stderr, "invalid node options: %v\n", err)
		os.Exit(2)
	}

	svc := node.New(opts, backend.NewNVMF())
	server, err := driver.ListenEndpoint(opts.Endpoint)
	if err != nil {
		fmt.Fprintf(os.Stderr, "listen endpoint failed: %v\n", err)
		os.Exit(1)
	}
	server.RegisterIdentity(driver.NewIdentityService(opts))
	server.RegisterNode(node.NewGRPCService(svc))

	ctx, stop := signal.NotifyContext(context.Background(), syscall.SIGINT, syscall.SIGTERM)
	defer stop()

	log.Printf("fastblock CSI node serving, driver=%s nodeID=%s endpoint=%s", svc.DriverName(), svc.NodeID(), opts.Endpoint)
	if err := server.Serve(ctx); err != nil {
		fmt.Fprintf(os.Stderr, "node server failed: %v\n", err)
		os.Exit(1)
	}
}
