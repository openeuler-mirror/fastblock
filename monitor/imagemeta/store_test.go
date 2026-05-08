package imagemeta

import (
	"context"
	"errors"
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
	gotByName, err := GetImageByName(ctx, client, "fb", "volume-a")
	if err != nil {
		t.Fatalf("GetImageByName failed: %v", err)
	}
	if gotByName.ImageID != "img-1" {
		t.Fatalf("unexpected image metadata by name: %+v", gotByName)
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

func TestImageAttachmentLifecycle(t *testing.T) {
	client := newTestClient(t)
	ctx := context.Background()

	image := &ImageMetadata{
		ImageID:    "img-attach-1",
		PoolID:     17,
		PoolName:   "fb",
		ImageName:  "volume-attach-a",
		Size:       1 << 20,
		ObjectSize: 4 << 20,
		Status:     ImageStatusReady,
	}
	if err := PutImage(ctx, client, image); err != nil {
		t.Fatalf("PutImage failed: %v", err)
	}

	attached, err := AttachImage(ctx, client, image.ImageID, "client-a", "controller", time.Minute)
	if err != nil {
		t.Fatalf("AttachImage failed: %v", err)
	}
	if len(attached.Attachments) != 1 {
		t.Fatalf("expected one attachment, got %+v", attached.Attachments)
	}
	if attached.Attachments[0].ClientID != "client-a" || attached.Attachments[0].ClientType != "controller" {
		t.Fatalf("unexpected attachment payload: %+v", attached.Attachments[0])
	}

	initialLease := attached.Attachments[0].LeaseExpiresAt
	reattached, err := AttachImage(ctx, client, image.ImageID, "client-a", "node", 2*time.Minute)
	if err != nil {
		t.Fatalf("reattach failed: %v", err)
	}
	if len(reattached.Attachments) != 1 {
		t.Fatalf("expected reattach to update in place, got %+v", reattached.Attachments)
	}
	if reattached.Attachments[0].ClientType != "node" {
		t.Fatalf("expected client type update on reattach, got %+v", reattached.Attachments[0])
	}
	if !reattached.Attachments[0].LeaseExpiresAt.After(initialLease) {
		t.Fatalf("expected renewed lease expiration after %v, got %v", initialLease, reattached.Attachments[0].LeaseExpiresAt)
	}

	renewed, err := RenewImageLease(ctx, client, image.ImageID, "client-a", 3*time.Minute)
	if err != nil {
		t.Fatalf("RenewImageLease failed: %v", err)
	}
	if len(renewed.Attachments) != 1 {
		t.Fatalf("expected one attachment after renew, got %+v", renewed.Attachments)
	}
	if !renewed.Attachments[0].LeaseExpiresAt.After(reattached.Attachments[0].LeaseExpiresAt) {
		t.Fatalf("expected lease expiration to advance after renew")
	}

	detached, err := DetachImage(ctx, client, image.ImageID, "client-a")
	if err != nil {
		t.Fatalf("DetachImage failed: %v", err)
	}
	if len(detached.Attachments) != 0 {
		t.Fatalf("expected no attachments after detach, got %+v", detached.Attachments)
	}
}

func TestDeleteImageRejectsActiveAttachments(t *testing.T) {
	client := newTestClient(t)
	ctx := context.Background()

	active := &ImageMetadata{
		ImageID:    "img-attach-active",
		PoolID:     18,
		PoolName:   "fb",
		ImageName:  "volume-attach-active",
		Size:       1 << 20,
		ObjectSize: 4 << 20,
		Status:     ImageStatusReady,
	}
	if err := PutImage(ctx, client, active); err != nil {
		t.Fatalf("PutImage failed: %v", err)
	}
	if _, err := AttachImage(ctx, client, active.ImageID, "client-a", "controller", time.Minute); err != nil {
		t.Fatalf("AttachImage failed: %v", err)
	}
	if err := DeleteImage(ctx, client, active.ImageID); !errors.Is(err, ErrImageAttached) {
		t.Fatalf("expected ErrImageAttached, got %v", err)
	}

	expired := &ImageMetadata{
		ImageID:    "img-attach-expired",
		PoolID:     19,
		PoolName:   "fb",
		ImageName:  "volume-attach-expired",
		Size:       1 << 20,
		ObjectSize: 4 << 20,
		Status:     ImageStatusReady,
		Attachments: []ImageAttachment{
			{
				ClientID:       "client-b",
				ClientType:     "controller",
				AttachedAt:     time.Now().UTC().Add(-2 * time.Minute),
				LeaseExpiresAt: time.Now().UTC().Add(-1 * time.Minute),
			},
		},
	}
	if err := PutImage(ctx, client, expired); err != nil {
		t.Fatalf("PutImage failed: %v", err)
	}
	if err := DeleteImage(ctx, client, expired.ImageID); err != nil {
		t.Fatalf("DeleteImage should ignore expired attachments, got %v", err)
	}
}

func TestRenewImageLeaseRequiresExistingAttachment(t *testing.T) {
	client := newTestClient(t)
	ctx := context.Background()

	image := &ImageMetadata{
		ImageID:    "img-attach-renew",
		PoolID:     20,
		PoolName:   "fb",
		ImageName:  "volume-attach-renew",
		Size:       1 << 20,
		ObjectSize: 4 << 20,
		Status:     ImageStatusReady,
	}
	if err := PutImage(ctx, client, image); err != nil {
		t.Fatalf("PutImage failed: %v", err)
	}

	if _, err := RenewImageLease(ctx, client, image.ImageID, "missing-client", time.Minute); !errors.Is(err, ErrAttachmentNotFound) {
		t.Fatalf("expected ErrAttachmentNotFound, got %v", err)
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
	gotByID, err := GetSnapshotByID(ctx, client, "snap-1")
	if err != nil {
		t.Fatalf("GetSnapshotByID failed: %v", err)
	}
	if gotByID.SourceImageID != "img-2" || gotByID.SnapshotName != "daily-1" {
		t.Fatalf("unexpected snapshot metadata by id: %+v", gotByID)
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

func TestCreateSnapshotByNameAdvancesCurrentSnapSeq(t *testing.T) {
	client := newTestClient(t)
	ctx := context.Background()

	image := &ImageMetadata{
		ImageID:        "img-3",
		PoolID:         11,
		PoolName:       "fb",
		ImageName:      "volume-c",
		Size:           8 << 20,
		ObjectSize:     4 << 20,
		CurrentSnapSeq: 2,
		Status:         ImageStatusReady,
		Generation:     7,
	}
	if err := PutImage(ctx, client, image); err != nil {
		t.Fatalf("PutImage failed: %v", err)
	}

	snapshot, err := CreateSnapshotByName(ctx, client, "fb", "volume-c", "snap-a")
	if err != nil {
		t.Fatalf("CreateSnapshotByName failed: %v", err)
	}
	if snapshot.SourceImageID != "img-3" || snapshot.SnapSeq != 3 || snapshot.OperationID == "" {
		t.Fatalf("unexpected snapshot returned: %+v", snapshot)
	}

	gotImage, err := GetImage(ctx, client, "img-3")
	if err != nil {
		t.Fatalf("GetImage failed: %v", err)
	}
	if gotImage.CurrentSnapSeq != 3 {
		t.Fatalf("expected current snap seq 3, got %+v", gotImage)
	}
	if gotImage.Generation != 8 {
		t.Fatalf("expected generation increment, got %+v", gotImage)
	}

	gotSnap, err := GetSnapshotByID(ctx, client, snapshot.SnapshotID)
	if err != nil {
		t.Fatalf("GetSnapshotByID failed: %v", err)
	}
	if gotSnap.SnapshotName != "snap-a" || gotSnap.SnapSeq != 3 {
		t.Fatalf("unexpected stored snapshot: %+v", gotSnap)
	}

	gotOp, err := GetOperation(ctx, client, snapshot.OperationID)
	if err != nil {
		t.Fatalf("GetOperation failed: %v", err)
	}
	if gotOp.Type != OperationCreateSnapshot || gotOp.TargetID != snapshot.SnapshotID || gotOp.Status != OperationStatusDone {
		t.Fatalf("unexpected operation: %+v", gotOp)
	}

	if _, err := CreateSnapshotByName(ctx, client, "fb", "volume-c", "snap-a"); !errors.Is(err, ErrSnapshotExists) {
		t.Fatalf("expected ErrSnapshotExists, got %v", err)
	}
}

func TestCreateCloneFromSnapshot(t *testing.T) {
	client := newTestClient(t)
	ctx := context.Background()

	image := &ImageMetadata{
		ImageID:        "img-4",
		PoolID:         13,
		PoolName:       "fb",
		ImageName:      "volume-d",
		Size:           16 << 20,
		ObjectSize:     4 << 20,
		CurrentSnapSeq: 5,
		Status:         ImageStatusReady,
		Generation:     2,
	}
	if err := PutImage(ctx, client, image); err != nil {
		t.Fatalf("PutImage failed: %v", err)
	}

	snapshot := &SnapshotMetadata{
		SnapshotID:      "snap-4",
		SnapshotName:    "base",
		SourceImageID:   "img-4",
		SourcePoolID:    13,
		SourcePoolName:  "fb",
		SourceImageName: "volume-d",
		SnapSeq:         5,
		Status:          SnapshotStatusReady,
		Protected:       true,
	}
	if err := PutSnapshot(ctx, client, snapshot); err != nil {
		t.Fatalf("PutSnapshot failed: %v", err)
	}

	clone, err := CreateCloneFromSnapshot(ctx, client, "snap-4", "clone-a")
	if err != nil {
		t.Fatalf("CreateCloneFromSnapshot failed: %v", err)
	}
	if clone.ParentSnapshotID != "snap-4" || clone.PoolName != "fb" || clone.ImageName != "clone-a" {
		t.Fatalf("unexpected clone metadata: %+v", clone)
	}
	if clone.CurrentSnapSeq != 0 {
		t.Fatalf("expected clone current snap seq 0, got %+v", clone)
	}

	gotClone, err := GetImage(ctx, client, clone.ImageID)
	if err != nil {
		t.Fatalf("GetImage clone failed: %v", err)
	}
	if gotClone.ParentSnapshotID != "snap-4" || gotClone.Depth != image.Depth+1 {
		t.Fatalf("unexpected stored clone metadata: %+v", gotClone)
	}

	children, err := ListChildImageIDs(ctx, client, "snap-4")
	if err != nil {
		t.Fatalf("ListChildImageIDs failed: %v", err)
	}
	if len(children) != 1 || children[0] != clone.ImageID {
		t.Fatalf("unexpected child links: %+v", children)
	}

	gotSnapshot, err := GetSnapshotByID(ctx, client, "snap-4")
	if err != nil {
		t.Fatalf("GetSnapshotByID failed: %v", err)
	}
	if gotSnapshot.ChildCount != 1 {
		t.Fatalf("expected child_count=1, got %+v", gotSnapshot)
	}

	notProtected := &SnapshotMetadata{
		SnapshotID:      "snap-5",
		SnapshotName:    "unprotected",
		SourceImageID:   "img-4",
		SourcePoolID:    13,
		SourcePoolName:  "fb",
		SourceImageName: "volume-d",
		SnapSeq:         5,
		Status:          SnapshotStatusReady,
		Protected:       false,
	}
	if err := PutSnapshot(ctx, client, notProtected); err != nil {
		t.Fatalf("PutSnapshot failed: %v", err)
	}
	if _, err := CreateCloneFromSnapshot(ctx, client, "snap-5", "clone-b"); !errors.Is(err, ErrSnapshotNotProtected) {
		t.Fatalf("expected ErrSnapshotNotProtected, got %v", err)
	}
}

func TestProtectAndUnprotectSnapshotByID(t *testing.T) {
	client := newTestClient(t)
	ctx := context.Background()

	image := &ImageMetadata{
		ImageID:    "img-5",
		PoolID:     15,
		PoolName:   "fb",
		ImageName:  "volume-e",
		Size:       4 << 20,
		ObjectSize: 4 << 20,
		Status:     ImageStatusReady,
	}
	if err := PutImage(ctx, client, image); err != nil {
		t.Fatalf("PutImage failed: %v", err)
	}

	snapshot := &SnapshotMetadata{
		SnapshotID:      "snap-6",
		SnapshotName:    "manual",
		SourceImageID:   "img-5",
		SourcePoolID:    15,
		SourcePoolName:  "fb",
		SourceImageName: "volume-e",
		SnapSeq:         1,
		Status:          SnapshotStatusReady,
		Protected:       false,
	}
	if err := PutSnapshot(ctx, client, snapshot); err != nil {
		t.Fatalf("PutSnapshot failed: %v", err)
	}

	protected, err := ProtectSnapshotByID(ctx, client, "snap-6")
	if err != nil {
		t.Fatalf("ProtectSnapshotByID failed: %v", err)
	}
	if !protected.Protected {
		t.Fatalf("expected protected snapshot, got %+v", protected)
	}

	unprotected, err := UnprotectSnapshotByID(ctx, client, "snap-6")
	if err != nil {
		t.Fatalf("UnprotectSnapshotByID failed: %v", err)
	}
	if unprotected.Protected {
		t.Fatalf("expected unprotected snapshot, got %+v", unprotected)
	}

	withChild := &SnapshotMetadata{
		SnapshotID:      "snap-7",
		SnapshotName:    "with-child",
		SourceImageID:   "img-5",
		SourcePoolID:    15,
		SourcePoolName:  "fb",
		SourceImageName: "volume-e",
		SnapSeq:         2,
		Status:          SnapshotStatusReady,
		Protected:       true,
		ChildCount:      1,
	}
	if err := PutSnapshot(ctx, client, withChild); err != nil {
		t.Fatalf("PutSnapshot failed: %v", err)
	}
	if _, err := UnprotectSnapshotByID(ctx, client, "snap-7"); !errors.Is(err, ErrSnapshotHasChildren) {
		t.Fatalf("expected ErrSnapshotHasChildren, got %v", err)
	}
}

func TestDeleteSnapshotByIDMarksDeletedPendingGC(t *testing.T) {
	client := newTestClient(t)
	ctx := context.Background()

	image := &ImageMetadata{
		ImageID:    "img-6",
		PoolID:     17,
		PoolName:   "fb",
		ImageName:  "volume-f",
		Size:       4 << 20,
		ObjectSize: 4 << 20,
		Status:     ImageStatusReady,
	}
	if err := PutImage(ctx, client, image); err != nil {
		t.Fatalf("PutImage failed: %v", err)
	}

	protected := &SnapshotMetadata{
		SnapshotID:      "snap-8",
		SnapshotName:    "protected",
		SourceImageID:   "img-6",
		SourcePoolID:    17,
		SourcePoolName:  "fb",
		SourceImageName: "volume-f",
		SnapSeq:         1,
		Status:          SnapshotStatusReady,
		Protected:       true,
	}
	if err := PutSnapshot(ctx, client, protected); err != nil {
		t.Fatalf("PutSnapshot failed: %v", err)
	}
	if _, err := DeleteSnapshotByID(ctx, client, "snap-8"); !errors.Is(err, ErrSnapshotProtected) {
		t.Fatalf("expected ErrSnapshotProtected, got %v", err)
	}

	withChild := &SnapshotMetadata{
		SnapshotID:      "snap-9",
		SnapshotName:    "child",
		SourceImageID:   "img-6",
		SourcePoolID:    17,
		SourcePoolName:  "fb",
		SourceImageName: "volume-f",
		SnapSeq:         2,
		Status:          SnapshotStatusReady,
		Protected:       false,
		ChildCount:      1,
	}
	if err := PutSnapshot(ctx, client, withChild); err != nil {
		t.Fatalf("PutSnapshot failed: %v", err)
	}
	if _, err := DeleteSnapshotByID(ctx, client, "snap-9"); !errors.Is(err, ErrSnapshotHasChildren) {
		t.Fatalf("expected ErrSnapshotHasChildren, got %v", err)
	}

	deletable := &SnapshotMetadata{
		SnapshotID:      "snap-10",
		SnapshotName:    "old",
		SourceImageID:   "img-6",
		SourcePoolID:    17,
		SourcePoolName:  "fb",
		SourceImageName: "volume-f",
		SnapSeq:         3,
		Status:          SnapshotStatusReady,
		Protected:       false,
	}
	if err := PutSnapshot(ctx, client, deletable); err != nil {
		t.Fatalf("PutSnapshot failed: %v", err)
	}
	deleted, err := DeleteSnapshotByID(ctx, client, "snap-10")
	if err != nil {
		t.Fatalf("DeleteSnapshotByID failed: %v", err)
	}
	if deleted.Status != SnapshotStatusDeletedPendingGC {
		t.Fatalf("expected deleted_pending_gc, got %+v", deleted)
	}
	gotByID, err := GetSnapshotByID(ctx, client, "snap-10")
	if err != nil {
		t.Fatalf("GetSnapshotByID failed: %v", err)
	}
	if gotByID.Status != SnapshotStatusDeletedPendingGC {
		t.Fatalf("expected stored deleted_pending_gc, got %+v", gotByID)
	}
	if _, err := GetSnapshotIDByName(ctx, client, "img-6", "old"); !errors.Is(err, ErrSnapshotNotFound) {
		t.Fatalf("expected snapshot name index removed, got %v", err)
	}
}

func TestFinalizeFlattenImageByID(t *testing.T) {
	client := newTestClient(t)
	ctx := context.Background()

	image := &ImageMetadata{
		ImageID:    "img-7",
		PoolID:     19,
		PoolName:   "fb",
		ImageName:  "volume-g",
		Size:       8 << 20,
		ObjectSize: 4 << 20,
		Status:     ImageStatusReady,
	}
	if err := PutImage(ctx, client, image); err != nil {
		t.Fatalf("PutImage failed: %v", err)
	}

	snapshot := &SnapshotMetadata{
		SnapshotID:      "snap-11",
		SnapshotName:    "base",
		SourceImageID:   "img-7",
		SourcePoolID:    19,
		SourcePoolName:  "fb",
		SourceImageName: "volume-g",
		SnapSeq:         1,
		Status:          SnapshotStatusReady,
		Protected:       true,
	}
	if err := PutSnapshot(ctx, client, snapshot); err != nil {
		t.Fatalf("PutSnapshot failed: %v", err)
	}

	clone, err := CreateCloneFromSnapshot(ctx, client, "snap-11", "clone-c")
	if err != nil {
		t.Fatalf("CreateCloneFromSnapshot failed: %v", err)
	}

	flattened, err := FinalizeFlattenImageByID(ctx, client, clone.ImageID)
	if err != nil {
		t.Fatalf("FinalizeFlattenImageByID failed: %v", err)
	}
	if flattened.ParentSnapshotID != "" || flattened.Depth != 0 {
		t.Fatalf("unexpected flattened image: %+v", flattened)
	}

	gotImage, err := GetImage(ctx, client, clone.ImageID)
	if err != nil {
		t.Fatalf("GetImage failed: %v", err)
	}
	if gotImage.ParentSnapshotID != "" || gotImage.Depth != 0 {
		t.Fatalf("unexpected stored flattened image: %+v", gotImage)
	}

	gotSnapshot, err := GetSnapshotByID(ctx, client, "snap-11")
	if err != nil {
		t.Fatalf("GetSnapshotByID failed: %v", err)
	}
	if gotSnapshot.ChildCount != 0 {
		t.Fatalf("expected child_count=0 after flatten, got %+v", gotSnapshot)
	}

	children, err := ListChildImageIDs(ctx, client, "snap-11")
	if err != nil {
		t.Fatalf("ListChildImageIDs failed: %v", err)
	}
	if len(children) != 0 {
		t.Fatalf("expected child links removed, got %+v", children)
	}

	plain := &ImageMetadata{
		ImageID:    "img-8",
		PoolID:     19,
		PoolName:   "fb",
		ImageName:  "plain",
		Size:       4 << 20,
		ObjectSize: 4 << 20,
		Status:     ImageStatusReady,
	}
	if err := PutImage(ctx, client, plain); err != nil {
		t.Fatalf("PutImage failed: %v", err)
	}
	if _, err := FinalizeFlattenImageByID(ctx, client, "img-8"); !errors.Is(err, ErrImageNotClone) {
		t.Fatalf("expected ErrImageNotClone, got %v", err)
	}

	withSnapshots := &ImageMetadata{
		ImageID:          "img-9",
		PoolID:           19,
		PoolName:         "fb",
		ImageName:        "clone-has-snaps",
		Size:             4 << 20,
		ObjectSize:       4 << 20,
		CurrentSnapSeq:   1,
		Status:           ImageStatusReady,
		ParentSnapshotID: "snap-11",
		Depth:            1,
	}
	if err := PutImage(ctx, client, withSnapshots); err != nil {
		t.Fatalf("PutImage failed: %v", err)
	}
	if _, err := FinalizeFlattenImageByID(ctx, client, "img-9"); !errors.Is(err, ErrImageHasSnapshots) {
		t.Fatalf("expected ErrImageHasSnapshots, got %v", err)
	}
}

func TestDeleteImageConstraints(t *testing.T) {
	client := newTestClient(t)
	ctx := context.Background()

	// Create base image
	image := &ImageMetadata{
		ImageID:    "img-del-1",
		PoolID:     20,
		PoolName:   "fb",
		ImageName:  "delete-test",
		Size:       8 << 20,
		ObjectSize: 4 << 20,
		Status:     ImageStatusReady,
	}
	if err := PutImage(ctx, client, image); err != nil {
		t.Fatalf("PutImage failed: %v", err)
	}

	// Create snapshot
	snapshot, err := CreateSnapshotByName(ctx, client, "fb", "delete-test", "snap-del")
	if err != nil {
		t.Fatalf("CreateSnapshotByName failed: %v", err)
	}

	// Should not be able to delete image with active snapshots
	if err := DeleteImage(ctx, client, "img-del-1"); !errors.Is(err, ErrImageHasSnapshots) {
		t.Fatalf("expected ErrImageHasSnapshots when deleting image with snapshots, got %v", err)
	}

	// Mark snapshot as deleted
	deletedSnapshot, err := DeleteSnapshotByID(ctx, client, snapshot.SnapshotID)
	if err != nil {
		t.Fatalf("DeleteSnapshotByID failed: %v", err)
	}
	if deletedSnapshot.Status != SnapshotStatusDeletedPendingGC {
		t.Fatalf("expected snapshot status DeletedPendingGC, got %v", deletedSnapshot.Status)
	}

	// Now should be able to delete image (snapshot is marked as deleted)
	if err := DeleteImage(ctx, client, "img-del-1"); err != nil {
		t.Fatalf("DeleteImage should succeed after snapshot deleted, got %v", err)
	}

	// Verify image is deleted
	if _, err := GetImage(ctx, client, "img-del-1"); !errors.Is(err, ErrImageNotFound) {
		t.Fatalf("expected ErrImageNotFound after deletion, got %v", err)
	}
}

func TestDeleteSnapshotConstraints(t *testing.T) {
	client := newTestClient(t)
	ctx := context.Background()

	// Create base image
	image := &ImageMetadata{
		ImageID:    "img-snap-del-1",
		PoolID:     21,
		PoolName:   "fb",
		ImageName:  "snap-delete-test",
		Size:       8 << 20,
		ObjectSize: 4 << 20,
		Status:     ImageStatusReady,
	}
	if err := PutImage(ctx, client, image); err != nil {
		t.Fatalf("PutImage failed: %v", err)
	}

	// Create and protect snapshot
	snapshot, err := CreateSnapshotByName(ctx, client, "fb", "snap-delete-test", "snap-protected")
	if err != nil {
		t.Fatalf("CreateSnapshotByName failed: %v", err)
	}
	protectedSnapshot, err := ProtectSnapshotByID(ctx, client, snapshot.SnapshotID)
	if err != nil {
		t.Fatalf("ProtectSnapshotByID failed: %v", err)
	}

	// Should not be able to delete protected snapshot
	if _, err := DeleteSnapshotByID(ctx, client, protectedSnapshot.SnapshotID); !errors.Is(err, ErrSnapshotProtected) {
		t.Fatalf("expected ErrSnapshotProtected when deleting protected snapshot, got %v", err)
	}

	// Create clone from snapshot
	clone, err := CreateCloneFromSnapshot(ctx, client, protectedSnapshot.SnapshotID, "clone-del-test")
	if err != nil {
		t.Fatalf("CreateCloneFromSnapshot failed: %v", err)
	}

	// Verify child_count increased
	updatedSnapshot, err := GetSnapshotByID(ctx, client, protectedSnapshot.SnapshotID)
	if err != nil {
		t.Fatalf("GetSnapshotByID failed: %v", err)
	}
	if updatedSnapshot.ChildCount != 1 {
		t.Fatalf("expected child_count=1 after clone creation, got %d", updatedSnapshot.ChildCount)
	}

	// Should not be able to unprotect snapshot with children
	if _, err := UnprotectSnapshotByID(ctx, client, protectedSnapshot.SnapshotID); !errors.Is(err, ErrSnapshotHasChildren) {
		t.Fatalf("expected ErrSnapshotHasChildren when unprotecting snapshot with children, got %v", err)
	}

	// Delete clone image
	if err := DeleteImage(ctx, client, clone.ImageID); err != nil {
		t.Fatalf("DeleteImage failed: %v", err)
	}

	// Verify child_count decreased
	afterDeleteSnapshot, err := GetSnapshotByID(ctx, client, protectedSnapshot.SnapshotID)
	if err != nil {
		t.Fatalf("GetSnapshotByID failed: %v", err)
	}
	if afterDeleteSnapshot.ChildCount != 0 {
		t.Fatalf("expected child_count=0 after clone deletion, got %d", afterDeleteSnapshot.ChildCount)
	}

	// Now should be able to unprotect
	unprotectedSnapshot, err := UnprotectSnapshotByID(ctx, client, protectedSnapshot.SnapshotID)
	if err != nil {
		t.Fatalf("UnprotectSnapshotByID should succeed after children deleted, got %v", err)
	}
	if unprotectedSnapshot.Protected {
		t.Fatalf("expected snapshot to be unprotected")
	}

	// Now should be able to delete
	deletedSnapshot, err := DeleteSnapshotByID(ctx, client, protectedSnapshot.SnapshotID)
	if err != nil {
		t.Fatalf("DeleteSnapshotByID should succeed after unprotect, got %v", err)
	}
	if deletedSnapshot.Status != SnapshotStatusDeletedPendingGC {
		t.Fatalf("expected snapshot status DeletedPendingGC, got %v", deletedSnapshot.Status)
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
