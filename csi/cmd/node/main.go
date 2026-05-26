package main

import (
	"context"
	"errors"
	"flag"
	"fmt"
	"log"
	"os"
	"os/signal"
	"strings"
	"syscall"

	"fastblock-csi/pkg/backend"
	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/node"
)

func main() {
	var endpoint string
	var driverName string
	var nodeID string
	var nodeIDFile string

	flag.StringVar(&endpoint, "endpoint", "unix:///var/lib/kubelet/plugins/csi.fastblock.io/node.sock", "CSI node endpoint")
	flag.StringVar(&driverName, "driver-name", driver.DefaultDriverName, "CSI driver name")
	flag.StringVar(&nodeID, "node-id", "", "Kubernetes node id")
	flag.StringVar(&nodeIDFile, "node-id-file", "/etc/nvme/hostnqn", "file used to auto-discover node id when -node-id is empty")
	flag.Parse()

	resolvedNodeID, err := resolveNodeID(nodeID, nodeIDFile)
	if err != nil {
		fmt.Fprintf(os.Stderr, "resolve node id failed: %v\n", err)
		os.Exit(2)
	}

	opts := driver.Options{
		DriverName: driverName,
		Endpoint:   endpoint,
		NodeID:     resolvedNodeID,
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

func resolveNodeID(explicitNodeID, nodeIDFile string) (string, error) {
	if nodeID := strings.TrimSpace(explicitNodeID); nodeID != "" {
		return nodeID, nil
	}
	path := strings.TrimSpace(nodeIDFile)
	if path == "" {
		return "", errors.New("node id is required")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return "", fmt.Errorf("read node id file %s: %w", path, err)
	}
	nodeID := strings.TrimSpace(string(data))
	if nodeID == "" {
		return "", fmt.Errorf("node id file %s is empty", path)
	}
	return nodeID, nil
}
