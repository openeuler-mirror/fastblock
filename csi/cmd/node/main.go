package main

import (
	"flag"
	"fmt"
	"log"
	"os"

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
	log.Printf("fastblock CSI node skeleton starting, driver=%s nodeID=%s", svc.DriverName(), svc.NodeID())
}
