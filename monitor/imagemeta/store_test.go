package imagemeta

import (
	"context"
	"fmt"
	"net"
	"net/url"
	"reflect"
	"testing"
	"time"

	"monitor/etcdapi"

	"github.com/coreos/etcd/embed"
)

func TestImageMetadataLifecycle(t *testing.T) {
	client := newTestClient(t)
	ctx := context.Background()

	image := &ImageMetadata{
		ImageID:    "img-1",
		PoolID:     7,
		PoolName:   "fb",
		ImageName:  "volume-a",
		Size:       1 << 20,
		ObjectSize: 4 << 20,
		Status:     ImageStatusReady,
		Generation: 1,
	}
	if err := PutImage(ctx, client, image); err != nil {
		t.Fatalf("PutImage failed: %v", err)
	}

	got, err := GetImage(ctx, client, "img-1")
	if err != nil {
		t.Fatalf("GetImage failed: %v", err)
	}
	if got.ImageName != "volume-a" || got.PoolName != "fb" || got.Generation != 1 {
		t.Fatalf("unexpected image metadata: %+v", got)
	}
	if got.CreatedAt.IsZero() || got.UpdatedAt.IsZero() {
		t.Fatalf("expected timestamps to be populated: %+v", got)
	}

	imageID, err := GetImageIDByName(ctx, client, "fb", "volume-a")
	if err != nil {
		t.Fatalf("GetImageIDByName failed: %v", err)
	}
	if imageID != "img-1" {
		t.Fatalf("unexpected image id by name: %q", imageID)
	}

	items, err := ListImages(ctx, client)
	if err != nil {
		t.Fatalf("ListImages failed: %v", err)
	}
	if len(items) != 1 || items[0].ImageID != "img-1" {
		t.Fatalf("unexpected list result: %+v", items)
	}

	if err := DeleteImage(ctx, client, "img-1"); err != nil {
		t.Fatalf("DeleteImage failed: %v", err)
	}
	if _, err := GetImage(ctx, client, "img-1"); err != ErrImageNotFound {
		t.Fatalf("expected ErrImageNotFound after delete, got %v", err)
	}
}

func TestSnapshotMetadataChildLinksAndOperations(t *testing.T) {
	client := newTestClient(t)
	ctx := context.Background()

	image := &ImageMetadata{
		ImageID:    "img-2",
		PoolID:     9,
		PoolName:   "fb",
		ImageName:  "volume-b",
		Size:       2 << 20,
		ObjectSize: 4 << 20,
	}
	if err := PutImage(ctx, client, image); err != nil {
		t.Fatalf("PutImage failed: %v", err)
	}

	snap := &SnapshotMetadata{
		SnapshotID:      "snap-1",
		SnapshotName:    "daily-1",
		SourceImageID:   "img-2",
		SourcePoolID:    9,
		SourcePoolName:  "fb",
		SourceImageName: "volume-b",
		SnapSeq:         10,
		Status:          SnapshotStatusReady,
		Protected:       true,
		OperationID:     "op-1",
	}
	if err := PutSnapshot(ctx, client, snap); err != nil {
		t.Fatalf("PutSnapshot failed: %v", err)
	}

	gotSnap, err := GetSnapshot(ctx, client, "img-2", "snap-1")
	if err != nil {
		t.Fatalf("GetSnapshot failed: %v", err)
	}
	if gotSnap.SnapshotName != "daily-1" || !gotSnap.Protected || gotSnap.SnapSeq != 10 {
		t.Fatalf("unexpected snapshot metadata: %+v", gotSnap)
	}

	snapshotID, err := GetSnapshotIDByName(ctx, client, "img-2", "daily-1")
	if err != nil {
		t.Fatalf("GetSnapshotIDByName failed: %v", err)
	}
	if snapshotID != "snap-1" {
		t.Fatalf("unexpected snapshot id by name: %q", snapshotID)
	}

	snapshots, err := ListSnapshots(ctx, client, "img-2")
	if err != nil {
		t.Fatalf("ListSnapshots failed: %v", err)
	}
	if len(snapshots) != 1 || snapshots[0].SnapshotID != "snap-1" {
		t.Fatalf("unexpected snapshots: %+v", snapshots)
	}

	if err := PutChildLink(ctx, client, "snap-1", "child-b"); err != nil {
		t.Fatalf("PutChildLink failed: %v", err)
	}
	if err := PutChildLink(ctx, client, "snap-1", "child-a"); err != nil {
		t.Fatalf("PutChildLink failed: %v", err)
	}
	children, err := ListChildImageIDs(ctx, client, "snap-1")
	if err != nil {
		t.Fatalf("ListChildImageIDs failed: %v", err)
	}
	expectedChildren := []string{"child-a", "child-b"}
	if !reflect.DeepEqual(children, expectedChildren) {
		t.Fatalf("unexpected child image ids: %+v", children)
	}
	if err := DeleteChildLink(ctx, client, "snap-1", "child-a"); err != nil {
		t.Fatalf("DeleteChildLink failed: %v", err)
	}

	record := &ImageOperationRecord{
		OperationID: "op-1",
		Type:        OperationCreateSnapshot,
		TargetID:    "snap-1",
		Status:      OperationStatusDone,
		Error:       "",
	}
	if err := PutOperation(ctx, client, record); err != nil {
		t.Fatalf("PutOperation failed: %v", err)
	}
	gotOp, err := GetOperation(ctx, client, "op-1")
	if err != nil {
		t.Fatalf("GetOperation failed: %v", err)
	}
	if gotOp.Type != OperationCreateSnapshot || gotOp.Status != OperationStatusDone {
		t.Fatalf("unexpected operation record: %+v", gotOp)
	}
	ops, err := ListOperations(ctx, client)
	if err != nil {
		t.Fatalf("ListOperations failed: %v", err)
	}
	if len(ops) != 1 || ops[0].OperationID != "op-1" {
		t.Fatalf("unexpected operations: %+v", ops)
	}

	if err := DeleteSnapshot(ctx, client, "img-2", "snap-1"); err != nil {
		t.Fatalf("DeleteSnapshot failed: %v", err)
	}
	if _, err := GetSnapshot(ctx, client, "img-2", "snap-1"); err != ErrSnapshotNotFound {
		t.Fatalf("expected ErrSnapshotNotFound after delete, got %v", err)
	}
	if err := DeleteOperation(ctx, client, "op-1"); err != nil {
		t.Fatalf("DeleteOperation failed: %v", err)
	}
	if _, err := GetOperation(ctx, client, "op-1"); err != ErrOperationNotFound {
		t.Fatalf("expected ErrOperationNotFound after delete, got %v", err)
	}
}

func newTestClient(t *testing.T) *etcdapi.EtcdClient {
	t.Helper()

	clientURL := mustURL(t, nextLocalURL(t))
	peerURL := mustURL(t, nextLocalURL(t))
	cfg := embed.NewConfig()
	cfg.Dir = t.TempDir()
	cfg.Name = "test-etcd"
	cfg.LCUrls = []url.URL{*clientURL}
	cfg.ACUrls = []url.URL{*clientURL}
	cfg.LPUrls = []url.URL{*peerURL}
	cfg.APUrls = []url.URL{*peerURL}
	cfg.InitialCluster = fmt.Sprintf("%s=%s", cfg.Name, peerURL.String())

	server, err := embed.StartEtcd(cfg)
	if err != nil {
		t.Fatalf("StartEtcd failed: %v", err)
	}
	t.Cleanup(server.Close)

	select {
	case <-server.Server.ReadyNotify():
	case <-time.After(10 * time.Second):
		t.Fatal("embedded etcd did not become ready")
	}

	client, err := etcdapi.NewEtcdClient([]string{clientURL.Host})
	if err != nil {
		t.Fatalf("NewEtcdClient failed: %v", err)
	}
	return client
}

func nextLocalURL(t *testing.T) string {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("net.Listen failed: %v", err)
	}
	defer listener.Close()
	return "http://" + listener.Addr().String()
}

func mustURL(t *testing.T, raw string) *url.URL {
	t.Helper()
	parsed, err := url.Parse(raw)
	if err != nil {
		t.Fatalf("url.Parse(%q) failed: %v", raw, err)
	}
	return parsed
}
