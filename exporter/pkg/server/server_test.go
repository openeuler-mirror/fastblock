package server

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"

	"fastblock-exporter/pkg/api"
	"fastblock-exporter/pkg/config"
)

type stubManager struct {
	createReq api.CreateExportRequest
	deleteID  string
	allowID   string
	allowNQN  string
}

func (m *stubManager) CreateExport(_ context.Context, req api.CreateExportRequest) (api.Export, error) {
	m.createReq = req
	return api.Export{ID: "exp-1", NQN: "nqn.1", NSID: 1, Traddr: "10.0.0.1", Trsvcid: "4420"}, nil
}

func (m *stubManager) DeleteExport(_ context.Context, exportID string) error {
	m.deleteID = exportID
	return nil
}

func (m *stubManager) AllowHost(_ context.Context, exportID, hostNQN string) error {
	m.allowID = exportID
	m.allowNQN = hostNQN
	return nil
}

func (m *stubManager) DenyHost(context.Context, string, string) error {
	return nil
}

func TestHealthz(t *testing.T) {
	srv := New(config.Config{NodeName: "node-a"}, &stubManager{})
	req := httptest.NewRequest(http.MethodGet, "/healthz", nil)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("unexpected status: %d", rec.Code)
	}
}

func TestCreateExport(t *testing.T) {
	manager := &stubManager{}
	srv := New(config.Config{NodeName: "node-a"}, manager)
	body, _ := json.Marshal(api.CreateExportRequest{
		VolumeID:      "fbvol:cluster:1:2",
		PoolName:      "fb",
		ImageName:     "img-1",
		CapacityBytes: 1 << 20,
		BlockSize:     4096,
		Transport:     "rdma",
	})
	req := httptest.NewRequest(http.MethodPost, "/v1/exports", bytes.NewReader(body))
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusCreated {
		t.Fatalf("unexpected status: %d", rec.Code)
	}
	if manager.createReq.VolumeID != "fbvol:cluster:1:2" {
		t.Fatalf("unexpected create request: %+v", manager.createReq)
	}
}

func TestDeleteExport(t *testing.T) {
	manager := &stubManager{}
	srv := New(config.Config{NodeName: "node-a"}, manager)
	req := httptest.NewRequest(http.MethodDelete, "/v1/exports/exp-9", nil)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusNoContent {
		t.Fatalf("unexpected status: %d", rec.Code)
	}
	if manager.deleteID != "exp-9" {
		t.Fatalf("unexpected delete id: %q", manager.deleteID)
	}
}

func TestAllowHost(t *testing.T) {
	manager := &stubManager{}
	srv := New(config.Config{NodeName: "node-a"}, manager)
	body, _ := json.Marshal(api.HostAccessRequest{HostNQN: "nqn.2014-08.org.nvmexpress:uuid:test"})
	req := httptest.NewRequest(http.MethodPost, "/v1/exports/exp-7/allow-host", bytes.NewReader(body))
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusNoContent {
		t.Fatalf("unexpected status: %d", rec.Code)
	}
	if manager.allowID != "exp-7" || manager.allowNQN == "" {
		t.Fatalf("unexpected allow-host call: id=%q nqn=%q", manager.allowID, manager.allowNQN)
	}
}
