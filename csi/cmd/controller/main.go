package main

import (
	"flag"
	"fmt"
	"log"
	"os"

	"fastblock-csi/pkg/controller"
	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/exporterclient"
	"fastblock-csi/pkg/monitorclient"
)

func main() {
	var endpoint string
	var driverName string
	var exporterEndpoint string
	var monitorAddress string

	flag.StringVar(&endpoint, "endpoint", "unix:///var/lib/kubelet/plugins/csi.fastblock.io/controller.sock", "CSI controller endpoint")
	flag.StringVar(&driverName, "driver-name", driver.DefaultDriverName, "CSI driver name")
	flag.StringVar(&monitorAddress, "monitor-address", "", "fastblock monitor address")
	flag.StringVar(&exporterEndpoint, "exporter-endpoint", "", "fastblock exporter base url")
	flag.Parse()

	opts := driver.Options{
		DriverName: driverName,
		Endpoint:   endpoint,
		Mode:       driver.ModeController,
	}
	if err := opts.Validate(); err != nil {
		fmt.Fprintf(os.Stderr, "invalid controller options: %v\n", err)
		os.Exit(2)
	}

	monitor := monitorclient.Client(monitorclient.NewNoop())
	if monitorAddress != "" {
		monitor = monitorclient.NewTCP(monitorAddress)
	}
	exporter := exporterclient.Client(exporterclient.NewNoop())
	if exporterEndpoint != "" {
		exporter = exporterclient.NewHTTP(exporterEndpoint)
	}

	svc := controller.New(opts, monitor, exporter)
	log.Printf("fastblock CSI controller skeleton starting, driver=%s endpoint=%s", svc.DriverName(), svc.Endpoint())
}
