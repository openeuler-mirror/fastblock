package controller

import (
	"context"
	"fmt"
	"sort"
	"strconv"
	"strings"

	"fastblock-csi/pkg/driver"
	"fastblock-csi/pkg/monitorclient"
	"fastblock-csi/pkg/volumeid"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/types/known/timestamppb"
)

type GRPCService struct {
	csi.UnimplementedControllerServer

	service *Service
}

type publishVolumeContext struct {
	ref           monitorclient.VolumeRef
	transport     string
	blockSize     int64
	objectSize    int64
	capacityBytes int64
}

func NewGRPCService(service *Service) *GRPCService {
	return &GRPCService{service: service}
}

func (s *GRPCService) ControllerGetCapabilities(context.Context, *csi.ControllerGetCapabilitiesRequest) (*csi.ControllerGetCapabilitiesResponse, error) {
	return &csi.ControllerGetCapabilitiesResponse{
		Capabilities: driver.ControllerServiceCapabilities(s.service.SupportsSnapshots()),
	}, nil
}

func (s *GRPCService) ValidateVolumeCapabilities(_ context.Context, req *csi.ValidateVolumeCapabilitiesRequest) (*csi.ValidateVolumeCapabilitiesResponse, error) {
	if !driver.AreSupportedVolumeCapabilities(req.GetVolumeCapabilities()) {
		return &csi.ValidateVolumeCapabilitiesResponse{}, nil
	}
	return &csi.ValidateVolumeCapabilitiesResponse{
		Confirmed: &csi.ValidateVolumeCapabilitiesResponse_Confirmed{
			VolumeCapabilities: req.GetVolumeCapabilities(),
			Parameters:         req.GetParameters(),
			VolumeContext:      req.GetVolumeContext(),
		},
	}, nil
}

func (s *GRPCService) CreateVolume(ctx context.Context, req *csi.CreateVolumeRequest) (*csi.CreateVolumeResponse, error) {
	if req.GetName() == "" {
		return nil, fmt.Errorf("volume name is required")
	}
	if req.GetParameters()["pool"] == "" {
		return nil, fmt.Errorf("pool parameter is required")
	}
	source := req.GetVolumeContentSource()
	snapshotID := ""
	if source != nil {
		if source.GetVolume() != nil {
			return nil, status.Error(codes.Unimplemented, "volume clone is not supported")
		}
		if source.GetSnapshot() == nil || strings.TrimSpace(source.GetSnapshot().GetSnapshotId()) == "" {
			return nil, status.Error(codes.InvalidArgument, "snapshot content source requires snapshot id")
		}
		if !s.service.SupportsSnapshots() {
			return nil, status.Error(codes.Unimplemented, ErrSnapshotNotSupported.Error())
		}
		snapshotID = strings.TrimSpace(source.GetSnapshot().GetSnapshotId())
	}
	required := req.GetCapacityRange().GetRequiredBytes()
	if required <= 0 {
		return nil, fmt.Errorf("required bytes must be greater than zero")
	}
	objectSize, err := strconv.ParseInt(req.GetParameters()["objectSize"], 10, 64)
	if err != nil {
		return nil, fmt.Errorf("invalid objectSize: %w", err)
	}
	blockSize, err := strconv.ParseInt(req.GetParameters()["blockSize"], 10, 64)
	if err != nil {
		return nil, fmt.Errorf("invalid blockSize: %w", err)
	}
	transport := req.GetParameters()["transport"]
	volume, err := s.service.CreateVolume(ctx, CreateVolumeRequest{
		Name:          req.GetName(),
		Pool:          req.GetParameters()["pool"],
		CapacityBytes: required,
		ObjectSize:    objectSize,
		BlockSize:     blockSize,
		Transport:     transport,
		SnapshotID:    snapshotID,
	})
	if err != nil {
		return nil, toGRPCError(err)
	}
	var contentSource *csi.VolumeContentSource
	if snapshotID != "" {
		contentSource = &csi.VolumeContentSource{
			Type: &csi.VolumeContentSource_Snapshot{
				Snapshot: &csi.VolumeContentSource_SnapshotSource{
					SnapshotId: snapshotID,
				},
			},
		}
	}
	return &csi.CreateVolumeResponse{
		Volume: &csi.Volume{
			VolumeId:      volume.ID,
			CapacityBytes: volume.CapacityBytes,
			ContentSource: contentSource,
			VolumeContext: map[string]string{
				"pool":          volume.Pool,
				"name":          volume.Name,
				"transport":     transport,
				"blockSize":     req.GetParameters()["blockSize"],
				"objectSize":    req.GetParameters()["objectSize"],
				"capacityBytes": strconv.FormatInt(volume.CapacityBytes, 10),
			},
		},
	}, nil
}

func (s *GRPCService) CreateSnapshot(ctx context.Context, req *csi.CreateSnapshotRequest) (*csi.CreateSnapshotResponse, error) {
	if !s.service.SupportsSnapshots() {
		return nil, status.Error(codes.Unimplemented, ErrSnapshotNotSupported.Error())
	}
	if strings.TrimSpace(req.GetName()) == "" {
		return nil, status.Error(codes.InvalidArgument, "snapshot name is required")
	}
	if strings.TrimSpace(req.GetSourceVolumeId()) == "" {
		return nil, status.Error(codes.InvalidArgument, "source volume id is required")
	}
	sourceVolume, err := s.resolveVolumeRef(ctx, req.GetSourceVolumeId())
	if err != nil {
		return nil, toGRPCError(err)
	}
	snapshot, err := s.service.CreateSnapshot(ctx, CreateSnapshotRequest{
		Name:         req.GetName(),
		SourceVolume: sourceVolume,
	})
	if err != nil {
		return nil, toGRPCError(err)
	}
	return &csi.CreateSnapshotResponse{
		Snapshot: toCSISnapshot(snapshot),
	}, nil
}

func (s *GRPCService) DeleteSnapshot(ctx context.Context, req *csi.DeleteSnapshotRequest) (*csi.DeleteSnapshotResponse, error) {
	if !s.service.SupportsSnapshots() {
		return nil, status.Error(codes.Unimplemented, ErrSnapshotNotSupported.Error())
	}
	if strings.TrimSpace(req.GetSnapshotId()) == "" {
		return nil, status.Error(codes.InvalidArgument, "snapshot id is required")
	}
	if err := s.service.DeleteSnapshot(ctx, DeleteSnapshotRequest{SnapshotID: req.GetSnapshotId()}); err != nil {
		return nil, toGRPCError(err)
	}
	return &csi.DeleteSnapshotResponse{}, nil
}

func (s *GRPCService) ListSnapshots(ctx context.Context, req *csi.ListSnapshotsRequest) (*csi.ListSnapshotsResponse, error) {
	if !s.service.SupportsSnapshots() {
		return nil, status.Error(codes.Unimplemented, ErrSnapshotNotSupported.Error())
	}
	if req.GetMaxEntries() < 0 {
		return nil, status.Error(codes.InvalidArgument, "max entries must not be negative")
	}
	listReq := ListSnapshotsRequest{
		SnapshotID:     req.GetSnapshotId(),
		SourceVolumeID: req.GetSourceVolumeId(),
	}
	if err := listReq.Validate(); err != nil {
		return nil, status.Error(codes.InvalidArgument, err.Error())
	}
	snapshots, err := s.service.ListSnapshots(ctx, listReq)
	if err != nil {
		return nil, toGRPCError(err)
	}
	sort.Slice(snapshots, func(i, j int) bool {
		return snapshots[i].ID < snapshots[j].ID
	})
	start, err := parseSnapshotStartingToken(req.GetStartingToken(), len(snapshots))
	if err != nil {
		return nil, err
	}
	entries, nextToken := paginateSnapshots(snapshots, start, req.GetMaxEntries())
	resp := &csi.ListSnapshotsResponse{
		Entries: make([]*csi.ListSnapshotsResponse_Entry, 0, len(entries)),
	}
	if nextToken != "" {
		resp.NextToken = nextToken
	}
	for _, snapshot := range entries {
		resp.Entries = append(resp.Entries, &csi.ListSnapshotsResponse_Entry{
			Snapshot: toCSISnapshot(snapshot),
		})
	}
	return resp, nil
}

func (s *GRPCService) DeleteVolume(ctx context.Context, req *csi.DeleteVolumeRequest) (*csi.DeleteVolumeResponse, error) {
	if req.GetVolumeId() == "" {
		return nil, fmt.Errorf("volume id is required")
	}
	ref, err := s.resolveVolumeRef(ctx, req.GetVolumeId())
	if err != nil {
		return nil, err
	}
	if err := s.service.DeleteVolume(ctx, NewDeleteVolumeRequest(ref)); err != nil {
		return nil, toGRPCError(err)
	}
	return &csi.DeleteVolumeResponse{}, nil
}

func (s *GRPCService) ControllerPublishVolume(ctx context.Context, req *csi.ControllerPublishVolumeRequest) (*csi.ControllerPublishVolumeResponse, error) {
	if req.GetVolumeId() == "" {
		return nil, fmt.Errorf("volume id is required")
	}
	if req.GetNodeId() == "" {
		return nil, fmt.Errorf("node id is required")
	}
	if !driver.IsSupportedVolumeCapability(req.GetVolumeCapability()) {
		return nil, fmt.Errorf("unsupported volume capability")
	}
	volumeCtx, err := s.resolvePublishVolumeContext(ctx, req.GetVolumeId(), req.GetVolumeContext())
	if err != nil {
		return nil, err
	}
	volume, err := s.service.GetVolume(ctx, NewGetVolumeRequest(volumeCtx.ref))
	if err != nil {
		return nil, err
	}
	volume.ID = req.GetVolumeId()
	if volume.CapacityBytes == 0 {
		volume.CapacityBytes = volumeCtx.capacityBytes
	}
	if volume.ObjectSize == 0 {
		volume.ObjectSize = volumeCtx.objectSize
	}
	result, err := s.service.ControllerPublishVolume(ctx, ControllerPublishRequest{
		Volume:    volume,
		BlockSize: volumeCtx.blockSize,
		Transport: volumeCtx.transport,
		NodeID:    req.GetNodeId(),
		Secrets:   req.GetSecrets(),
	})
	if err != nil {
		return nil, toGRPCError(err)
	}
	return &csi.ControllerPublishVolumeResponse{
		PublishContext: result.PublishContext,
	}, nil
}

func (s *GRPCService) ControllerUnpublishVolume(ctx context.Context, req *csi.ControllerUnpublishVolumeRequest) (*csi.ControllerUnpublishVolumeResponse, error) {
	if req.GetVolumeId() == "" {
		return nil, fmt.Errorf("volume id is required")
	}
	if err := s.service.ControllerUnpublishVolume(ctx, ControllerUnpublishRequest{
		VolumeID: req.GetVolumeId(),
		NodeID:   req.GetNodeId(),
		Secrets:  req.GetSecrets(),
	}); err != nil {
		return nil, toGRPCError(err)
	}
	return &csi.ControllerUnpublishVolumeResponse{}, nil
}

func (s *GRPCService) resolveVolumeRef(ctx context.Context, volumeID string) (monitorclient.VolumeRef, error) {
	if metadata, ok, err := s.service.volumes.Get(ctx, volumeID); err != nil {
		return monitorclient.VolumeRef{}, err
	} else if ok {
		return metadata.Volume.Ref(), nil
	}
	nameRef, err := volumeid.DecodeNameRef(volumeID)
	if err != nil {
		return monitorclient.VolumeRef{}, err
	}
	return monitorclient.VolumeRef{
		ID:   volumeID,
		Pool: nameRef.Pool,
		Name: nameRef.Name,
	}, nil
}

func (s *GRPCService) resolvePublishVolumeContext(ctx context.Context, volumeID string, volumeContext map[string]string) (publishVolumeContext, error) {
	if metadata, ok, err := s.service.volumes.Get(ctx, volumeID); err != nil {
		return publishVolumeContext{}, err
	} else if ok {
		if metadata.BlockSize > 0 &&
			(metadata.Transport == "rdma" || metadata.Transport == "tcp") &&
			metadata.Volume.ObjectSize > 0 &&
			metadata.Volume.CapacityBytes > 0 &&
			strings.TrimSpace(metadata.Volume.Name) != "" &&
			strings.TrimSpace(metadata.Volume.Pool) != "" {
			if err := validatePublishVolumeContextAgainstMetadata(metadata, volumeContext); err != nil {
				return publishVolumeContext{}, err
			}
			return publishVolumeContext{
				ref:           metadata.Volume.Ref(),
				transport:     metadata.Transport,
				blockSize:     metadata.BlockSize,
				objectSize:    metadata.Volume.ObjectSize,
				capacityBytes: metadata.Volume.CapacityBytes,
			}, nil
		}
	}
	return parsePublishVolumeContext(volumeID, volumeContext)
}

func validatePublishVolumeContextAgainstMetadata(metadata VolumeMetadata, ctx map[string]string) error {
	if len(ctx) == 0 {
		return nil
	}
	if pool := strings.TrimSpace(ctx["pool"]); pool != "" && pool != metadata.Volume.Pool {
		return fmt.Errorf("volume_context pool mismatch with stored metadata")
	}
	if name := strings.TrimSpace(ctx["name"]); name != "" && name != metadata.Volume.Name {
		return fmt.Errorf("volume_context name mismatch with stored metadata")
	}
	return nil
}

func parsePublishVolumeContext(volumeID string, ctx map[string]string) (publishVolumeContext, error) {
	pool := strings.TrimSpace(ctx["pool"])
	if pool == "" {
		return publishVolumeContext{}, fmt.Errorf("volume_context.pool is required")
	}
	name := strings.TrimSpace(ctx["name"])
	if name == "" {
		return publishVolumeContext{}, fmt.Errorf("volume_context.name is required")
	}
	transport := strings.TrimSpace(ctx["transport"])
	if transport != "rdma" && transport != "tcp" {
		return publishVolumeContext{}, fmt.Errorf("invalid volume_context.transport %q", transport)
	}
	blockSize, err := strconv.ParseInt(ctx["blockSize"], 10, 64)
	if err != nil || blockSize <= 0 {
		return publishVolumeContext{}, fmt.Errorf("invalid volume_context.blockSize")
	}
	objectSize, err := strconv.ParseInt(ctx["objectSize"], 10, 64)
	if err != nil || objectSize <= 0 {
		return publishVolumeContext{}, fmt.Errorf("invalid volume_context.objectSize")
	}
	capacityBytes, err := strconv.ParseInt(ctx["capacityBytes"], 10, 64)
	if err != nil || capacityBytes <= 0 {
		return publishVolumeContext{}, fmt.Errorf("invalid volume_context.capacityBytes")
	}
	return publishVolumeContext{
		ref: monitorclient.VolumeRef{
			ID:   volumeID,
			Pool: pool,
			Name: name,
		},
		transport:     transport,
		blockSize:     blockSize,
		objectSize:    objectSize,
		capacityBytes: capacityBytes,
	}, nil
}

func toCSISnapshot(snapshot monitorclient.Snapshot) *csi.Snapshot {
	if snapshot.ID == "" {
		return nil
	}
	item := &csi.Snapshot{
		SizeBytes:      snapshot.SizeBytes,
		SnapshotId:     snapshot.ID,
		SourceVolumeId: snapshot.SourceVolume.ID,
		ReadyToUse:     snapshot.ReadyToUse,
	}
	if !snapshot.CreationTime.IsZero() {
		item.CreationTime = timestamppb.New(snapshot.CreationTime)
	}
	return item
}

func parseSnapshotStartingToken(token string, total int) (int, error) {
	if strings.TrimSpace(token) == "" {
		return 0, nil
	}
	start, err := strconv.Atoi(token)
	if err != nil || start < 0 || start > total {
		return 0, status.Error(codes.Aborted, "invalid starting token")
	}
	return start, nil
}

func paginateSnapshots(snapshots []monitorclient.Snapshot, start int, maxEntries int32) ([]monitorclient.Snapshot, string) {
	if start >= len(snapshots) {
		return nil, ""
	}
	if maxEntries <= 0 {
		return snapshots[start:], ""
	}
	end := start + int(maxEntries)
	if end >= len(snapshots) {
		return snapshots[start:], ""
	}
	return snapshots[start:end], strconv.Itoa(end)
}
