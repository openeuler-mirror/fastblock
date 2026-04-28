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
	volumeCapability := driver.SingleNodeWriterBlockVolumeCapability()

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
	cleanupState.publishContext = publishResp.GetPublishContext()
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
	publishContext      map[string]string
	controllerPublished bool
	nodeStaged          bool
	nodePublished       bool
}

func (s *smokeState) cleanup(ctx context.Context) {
	ctx, cancel := context.WithTimeout(ctx, 30*time.Second)
	defer cancel()

	if s.nodePublished {
		if _, err := s.nodeClient.NodeUnpublishVolume(ctx, &csi.NodeUnpublishVolumeRequest{
			VolumeId:   s.volumeID,
			TargetPath: s.targetPath,
		}); err != nil {
			log.Printf("cleanup NodeUnpublishVolume failed: %v", err)
		}
	}
	if s.nodeStaged {
		if _, err := s.nodeClient.NodeUnstageVolume(ctx, &csi.NodeUnstageVolumeRequest{
			VolumeId:          s.volumeID,
			StagingTargetPath: s.stagePath,
		}); err != nil {
			log.Printf("cleanup NodeUnstageVolume failed: %v", err)
		}
	}
	if s.controllerPublished {
		req := &csi.ControllerUnpublishVolumeRequest{
			VolumeId: s.publishContext[driver.PublishContextExportID],
			NodeId:   s.nodeID,
		}
		if s.hostNQN != "" {
			req.Secrets = map[string]string{"hostNQN": s.hostNQN}
		}
		if _, err := s.controllerClient.ControllerUnpublishVolume(ctx, req); err != nil {
			log.Printf("cleanup ControllerUnpublishVolume failed: %v", err)
		}
	}
	if s.volumeID != "" {
		if _, err := s.controllerClient.DeleteVolume(ctx, &csi.DeleteVolumeRequest{VolumeId: s.volumeID}); err != nil {
			log.Printf("cleanup DeleteVolume failed: %v", err)
		}
	}
}
