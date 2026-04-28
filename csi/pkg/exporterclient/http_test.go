package exporterclient

import (
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestCreateExport(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost || r.URL.Path != "/v1/exports" {
			t.Fatalf("unexpected request: %s %s", r.Method, r.URL.Path)
		}
		w.WriteHeader(http.StatusCreated)
		_ = json.NewEncoder(w).Encode(Export{ID: "exp-1", NQN: "nqn.1", NSID: 1, Traddr: "10.0.0.1", Trsvcid: "4420"})
	}))
	defer server.Close()

	client := NewHTTP(server.URL)
	export, err := client.CreateExport(context.Background(), CreateExportRequest{
		VolumeID:      "fbvol:cluster:1:2",
		PoolName:      "fb",
		ImageName:     "img-2",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
		BlockSize:     4096,
		Transport:     "rdma",
	})
	if err != nil {
		t.Fatalf("create export failed: %v", err)
	}
	if export.ID != "exp-1" {
		t.Fatalf("unexpected export: %+v", export)
	}
}

func TestDeleteAndHostACL(t *testing.T) {
	var paths []string
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		paths = append(paths, r.Method+" "+r.URL.Path)
		w.WriteHeader(http.StatusNoContent)
	}))
	defer server.Close()

	client := NewHTTP(server.URL)
	if err := client.DeleteExport(context.Background(), "exp-1"); err != nil {
		t.Fatalf("delete export failed: %v", err)
	}
	if err := client.AllowHost(context.Background(), "exp-1", "nqn.host.1"); err != nil {
		t.Fatalf("allow host failed: %v", err)
	}
	if err := client.DenyHost(context.Background(), "exp-1", "nqn.host.1"); err != nil {
		t.Fatalf("deny host failed: %v", err)
	}
	if len(paths) != 3 {
		t.Fatalf("unexpected request count: %d", len(paths))
	}
}

func TestHTTPErrorResponse(t *testing.T) {
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusBadRequest)
		_ = json.NewEncoder(w).Encode(map[string]string{"error": "bad request"})
	}))
	defer server.Close()

	client := NewHTTP(server.URL)
	_, err := client.CreateExport(context.Background(), CreateExportRequest{
		VolumeID:      "fbvol:cluster:1:2",
		PoolName:      "fb",
		ImageName:     "img-2",
		CapacityBytes: 1 << 20,
		ObjectSize:    4 << 20,
		BlockSize:     4096,
		Transport:     "rdma",
	})
	if err == nil || !strings.Contains(err.Error(), "bad request") {
		t.Fatalf("unexpected error: %v", err)
	}
}
