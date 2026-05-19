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

	flag.StringVar(&endpoint, "endpoint", "unix:///var/lib/kubelet/plugins/csi.fastblock.io/controller.sock", "CSI controller endpoint")
	flag.StringVar(&driverName, "driver-name", driver.DefaultDriverName, "CSI driver name")
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

	svc := controller.New(opts, monitorclient.NewNoop(), exporterclient.NewNoop())
	log.Printf("fastblock CSI controller skeleton starting, driver=%s endpoint=%s", svc.DriverName(), svc.Endpoint())
}
