package main

import (
	"context"
	"flag"
	"fmt"
	"log"
	"path/filepath"
	"time"

	"fastblock-csi/pkg/driver"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
)

func main() {
	var controllerEndpoint string
	var nodeEndpoint string
	var nodeID string
	var hostNQN string
	var volumeName string
	var pool string
	var transport string
	var stagePath string
	var targetPath string
	var sizeBytes int64
	var objectSize int64
	var blockSize int64
	var cleanup bool

	flag.StringVar(&controllerEndpoint, "controller-endpoint", "unix:///tmp/fastblock-csi-controller.sock", "CSI controller endpoint")
	flag.StringVar(&nodeEndpoint, "node-endpoint", "unix:///tmp/fastblock-csi-node.sock", "CSI node endpoint")
	flag.StringVar(&nodeID, "node-id", "node-a", "CSI node id")
	flag.StringVar(&hostNQN, "host-nqn", "", "host NQN used for controller publish")
	flag.StringVar(&volumeName, "volume-name", "smoke-vol", "volume name")
	flag.StringVar(&pool, "pool", "fb", "fastblock pool")
	flag.StringVar(&transport, "transport", "rdma", "transport type: rdma or tcp")
	flag.StringVar(&stagePath, "stage-path", "", "staging target path")
	flag.StringVar(&targetPath, "target-path", "", "publish target path")
	flag.Int64Var(&sizeBytes, "size-bytes", 1<<20, "volume size in bytes")
	flag.Int64Var(&objectSize, "object-size", 4<<20, "object size in bytes")
	flag.Int64Var(&blockSize, "block-size", 4096, "block size in bytes")
	flag.BoolVar(&cleanup, "cleanup", true, "cleanup resources after smoke flow")
	flag.Parse()

	baseDir := filepath.Join("/tmp", "fastblock-csi-smoke", volumeName)
	if stagePath == "" {
		stagePath = filepath.Join(baseDir, "stage")
	}
	if targetPath == "" {
		targetPath = filepath.Join(baseDir, "publish", "device")
	}

	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Minute)
	defer cancel()

	controllerConn, err := driver.DialContext(ctx, controllerEndpoint)
	if err != nil {
		log.Fatalf("dial controller failed: %v", err)
	}
	defer controllerConn.Close()

	nodeConn, err := driver.DialContext(ctx, nodeEndpoint)
	if err != nil {
		log.Fatalf("dial node failed: %v", err)
	}
	defer nodeConn.Close()

	controllerClient := csi.NewControllerClient(controllerConn)
	nodeClient := csi.NewNodeClient(nodeConn)
	controllerIdentity := csi.NewIdentityClient(controllerConn)
	nodeIdentity := csi.NewIdentityClient(nodeConn)
	volumeCapability := driver.SingleNodeWriterBlockVolumeCapability()

	runPreflight(ctx, controllerIdentity, nodeIdentity, controllerClient, nodeClient)

	log.Printf("CreateVolume name=%s pool=%s transport=%s", volumeName, pool, transport)
	createResp, err := controllerClient.CreateVolume(ctx, &csi.CreateVolumeRequest{
		Name: volumeName,
		CapacityRange: &csi.CapacityRange{
			RequiredBytes: sizeBytes,
		},
		Parameters: map[string]string{
			"pool":       pool,
			"objectSize": fmt.Sprintf("%d", objectSize),
			"blockSize":  fmt.Sprintf("%d", blockSize),
			"transport":  transport,
		},
		VolumeCapabilities: []*csi.VolumeCapability{volumeCapability},
	})
	if err != nil {
		log.Fatalf("CreateVolume failed: %v", err)
	}
	volume := createResp.GetVolume()
	if volume == nil {
		log.Fatalf("CreateVolume returned nil volume")
	}

	cleanupState := &smokeState{
		controllerClient: controllerClient,
		nodeClient:       nodeClient,
		volumeID:         volume.GetVolumeId(),
		nodeID:           nodeID,
		stagePath:        stagePath,
		targetPath:       targetPath,
	}
	if cleanup {
		defer cleanupState.cleanup(context.Background())
	}

	log.Printf("ControllerPublishVolume volumeID=%s nodeID=%s", volume.GetVolumeId(), nodeID)
	publishReq := &csi.ControllerPublishVolumeRequest{
		VolumeId:         volume.GetVolumeId(),
		NodeId:           nodeID,
		VolumeCapability: volumeCapability,
		VolumeContext:    volume.GetVolumeContext(),
	}
	if hostNQN != "" {
		publishReq.Secrets = map[string]string{"hostNQN": hostNQN}
	}
	publishResp, err := controllerClient.ControllerPublishVolume(ctx, publishReq)
	if err != nil {
		log.Fatalf("ControllerPublishVolume failed: %v", err)
	}
	cleanupState.controllerPublished = true
	cleanupState.hostNQN = hostNQN
	if cleanupState.hostNQN == "" {
		cleanupState.hostNQN = nodeID
	}

	log.Printf("NodeStageVolume stagePath=%s", stagePath)
	if _, err := nodeClient.NodeStageVolume(ctx, &csi.NodeStageVolumeRequest{
		VolumeId:          volume.GetVolumeId(),
		PublishContext:    publishResp.GetPublishContext(),
		StagingTargetPath: stagePath,
		VolumeCapability:  volumeCapability,
		VolumeContext:     volume.GetVolumeContext(),
	}); err != nil {
		log.Fatalf("NodeStageVolume failed: %v", err)
	}
	cleanupState.nodeStaged = true

	log.Printf("NodePublishVolume targetPath=%s", targetPath)
	if _, err := nodeClient.NodePublishVolume(ctx, &csi.NodePublishVolumeRequest{
		VolumeId:          volume.GetVolumeId(),
		PublishContext:    publishResp.GetPublishContext(),
		StagingTargetPath: stagePath,
		TargetPath:        targetPath,
		VolumeCapability:  volumeCapability,
		VolumeContext:     volume.GetVolumeContext(),
	}); err != nil {
		log.Fatalf("NodePublishVolume failed: %v", err)
	}
	cleanupState.nodePublished = true

	log.Printf("Smoke flow succeeded volumeID=%s", volume.GetVolumeId())
}

type smokeState struct {
	controllerClient    csi.ControllerClient
	nodeClient          csi.NodeClient
	volumeID            string
	nodeID              string
	stagePath           string
	targetPath          string
	hostNQN             string
	controllerPublished bool
	nodeStaged          bool
	nodePublished       bool
}

func (s *smokeState) cleanup(ctx context.Context) {
	if s.nodePublished {
		if err := runCleanupStep(ctx, 15*time.Second, func(stepCtx context.Context) error {
			_, err := s.nodeClient.NodeUnpublishVolume(stepCtx, &csi.NodeUnpublishVolumeRequest{
				VolumeId:   s.volumeID,
				TargetPath: s.targetPath,
			})
			return err
		}); err != nil {
			log.Printf("cleanup NodeUnpublishVolume failed: %v", err)
		}
	}
	if s.nodeStaged {
		if err := runCleanupStep(ctx, 30*time.Second, func(stepCtx context.Context) error {
			_, err := s.nodeClient.NodeUnstageVolume(stepCtx, &csi.NodeUnstageVolumeRequest{
				VolumeId:          s.volumeID,
				StagingTargetPath: s.stagePath,
			})
			return err
		}); err != nil {
			log.Printf("cleanup NodeUnstageVolume failed: %v", err)
		}
	}
	if s.controllerPublished {
		if err := runCleanupStep(ctx, 60*time.Second, func(stepCtx context.Context) error {
			req := &csi.ControllerUnpublishVolumeRequest{
				VolumeId: s.volumeID,
				NodeId:   s.nodeID,
			}
			if s.hostNQN != "" {
				req.Secrets = map[string]string{"hostNQN": s.hostNQN}
			}
			_, err := s.controllerClient.ControllerUnpublishVolume(stepCtx, req)
			return err
		}); err != nil {
			log.Printf("cleanup ControllerUnpublishVolume failed: %v", err)
		}
	}
	if s.volumeID != "" {
		if err := runCleanupStep(ctx, 30*time.Second, func(stepCtx context.Context) error {
			_, err := s.controllerClient.DeleteVolume(stepCtx, &csi.DeleteVolumeRequest{VolumeId: s.volumeID})
			return err
		}); err != nil {
			log.Printf("cleanup DeleteVolume failed: %v", err)
		}
	}
}

func runCleanupStep(parent context.Context, timeout time.Duration, fn func(context.Context) error) error {
	stepCtx, cancel := context.WithTimeout(parent, timeout)
	defer cancel()
	return fn(stepCtx)
}

func runPreflight(
	ctx context.Context,
	controllerIdentity csi.IdentityClient,
	nodeIdentity csi.IdentityClient,
	controllerClient csi.ControllerClient,
	nodeClient csi.NodeClient,
) {
	controllerInfo, err := controllerIdentity.GetPluginInfo(ctx, &csi.GetPluginInfoRequest{})
	if err != nil {
		log.Fatalf("controller GetPluginInfo failed: %v", err)
	}
	nodeInfo, err := nodeIdentity.GetPluginInfo(ctx, &csi.GetPluginInfoRequest{})
	if err != nil {
		log.Fatalf("node GetPluginInfo failed: %v", err)
	}
	log.Printf("controller plugin=%s version=%s", controllerInfo.GetName(), controllerInfo.GetVendorVersion())
	log.Printf("node plugin=%s version=%s", nodeInfo.GetName(), nodeInfo.GetVendorVersion())

	if _, err := controllerIdentity.Probe(ctx, &csi.ProbeRequest{}); err != nil {
		log.Fatalf("controller Probe failed: %v", err)
	}
	if _, err := nodeIdentity.Probe(ctx, &csi.ProbeRequest{}); err != nil {
		log.Fatalf("node Probe failed: %v", err)
	}

	controllerCaps, err := controllerClient.ControllerGetCapabilities(ctx, &csi.ControllerGetCapabilitiesRequest{})
	if err != nil {
		log.Fatalf("ControllerGetCapabilities failed: %v", err)
	}
	nodeCaps, err := nodeClient.NodeGetCapabilities(ctx, &csi.NodeGetCapabilitiesRequest{})
	if err != nil {
		log.Fatalf("NodeGetCapabilities failed: %v", err)
	}
	log.Printf("controller capabilities=%d node capabilities=%d", len(controllerCaps.GetCapabilities()), len(nodeCaps.GetCapabilities()))
}
