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
	"fastblock-exporter/pkg/nvmf"
)

type stubManager struct {
	createReq api.CreateExportRequest
	listResp   []api.Export
	deleteID  string
	allowID   string
	allowNQN  string
	denyID    string
	denyNQN   string
	getID     string
	getErr    error
}

func (m *stubManager) CreateExport(_ context.Context, req api.CreateExportRequest) (api.Export, error) {
	m.createReq = req
	return api.Export{ID: "exp-1", NQN: "nqn.1", NSID: 1, Traddr: "10.0.0.1", Trsvcid: "4420"}, nil
}

func (m *stubManager) ListExports(_ context.Context) ([]api.Export, error) {
	return m.listResp, nil
}

func (m *stubManager) DeleteExport(_ context.Context, exportID string) error {
	m.deleteID = exportID
	return nil
}

func (m *stubManager) GetExport(_ context.Context, exportID string) (api.Export, error) {
	m.getID = exportID
	if m.getErr != nil {
		return api.Export{}, m.getErr
	}
	return api.Export{ID: exportID, NQN: "nqn.1", NSID: 1, Traddr: "10.0.0.1", Trsvcid: "4420"}, nil
}

func (m *stubManager) AllowHost(_ context.Context, exportID, hostNQN string) error {
	m.allowID = exportID
	m.allowNQN = hostNQN
	return nil
}

func (m *stubManager) DenyHost(_ context.Context, exportID, hostNQN string) error {
	m.denyID = exportID
	m.denyNQN = hostNQN
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
		ObjectSize:    4 << 20,
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

func TestListExports(t *testing.T) {
	manager := &stubManager{
		listResp: []api.Export{{ID: "exp-1", NQN: "nqn.1", NSID: 1, Traddr: "10.0.0.1", Trsvcid: "4420"}},
	}
	srv := New(config.Config{NodeName: "node-a"}, manager)
	req := httptest.NewRequest(http.MethodGet, "/v1/exports", nil)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("unexpected status: %d", rec.Code)
	}
	var exports []api.Export
	if err := json.Unmarshal(rec.Body.Bytes(), &exports); err != nil {
		t.Fatalf("decode response failed: %v", err)
	}
	if len(exports) != 1 || exports[0].ID != "exp-1" {
		t.Fatalf("unexpected exports: %+v", exports)
	}
}

func TestCreateExportWithoutSizeHints(t *testing.T) {
	manager := &stubManager{}
	srv := New(config.Config{NodeName: "node-a"}, manager)
	body, _ := json.Marshal(api.CreateExportRequest{
		VolumeID:  "fbvol:cluster:1:3",
		PoolName:  "fb",
		ImageName: "img-2",
		BlockSize: 4096,
		Transport: "tcp",
	})
	req := httptest.NewRequest(http.MethodPost, "/v1/exports", bytes.NewReader(body))
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusCreated {
		t.Fatalf("unexpected status: %d", rec.Code)
	}
	if manager.createReq.ImageName != "img-2" || manager.createReq.BlockSize != 4096 {
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

func TestGetExport(t *testing.T) {
	manager := &stubManager{}
	srv := New(config.Config{NodeName: "node-a"}, manager)
	req := httptest.NewRequest(http.MethodGet, "/v1/exports/exp-5", nil)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("unexpected status: %d", rec.Code)
	}
	if manager.getID != "exp-5" {
		t.Fatalf("unexpected get id: %q", manager.getID)
	}
}

func TestGetExportReturnsNotFound(t *testing.T) {
	manager := &stubManager{getErr: nvmf.ErrExportNotFound}
	srv := New(config.Config{NodeName: "node-a"}, manager)
	req := httptest.NewRequest(http.MethodGet, "/v1/exports/exp-missing", nil)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusNotFound {
		t.Fatalf("unexpected status: %d", rec.Code)
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

func TestDenyHost(t *testing.T) {
	manager := &stubManager{}
	srv := New(config.Config{NodeName: "node-a"}, manager)
	body, _ := json.Marshal(api.HostAccessRequest{HostNQN: "nqn.2014-08.org.nvmexpress:uuid:test"})
	req := httptest.NewRequest(http.MethodPost, "/v1/exports/exp-8/deny-host", bytes.NewReader(body))
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusNoContent {
		t.Fatalf("unexpected status: %d", rec.Code)
	}
	if manager.denyID != "exp-8" || manager.denyNQN == "" {
		t.Fatalf("unexpected deny-host call: id=%q nqn=%q", manager.denyID, manager.denyNQN)
	}
}

func TestRejectInvalidCreateExportRequest(t *testing.T) {
	manager := &stubManager{}
	srv := New(config.Config{NodeName: "node-a"}, manager)
	req := httptest.NewRequest(http.MethodPost, "/v1/exports", bytes.NewReader([]byte(`{}`)))
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusBadRequest {
		t.Fatalf("unexpected status: %d", rec.Code)
	}
}

func TestRejectWrongMethodOnHealthz(t *testing.T) {
	srv := New(config.Config{NodeName: "node-a"}, &stubManager{})
	req := httptest.NewRequest(http.MethodPost, "/healthz", nil)
	rec := httptest.NewRecorder()
	srv.Handler().ServeHTTP(rec, req)
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("unexpected status: %d", rec.Code)
	}
}

func TestParseExportRoute(t *testing.T) {
	exportID, action, ok := parseExportRoute("/v1/exports/exp-1")
	if !ok || exportID != "exp-1" || action != "" {
		t.Fatalf("unexpected delete route parse: %v %q %q", ok, exportID, action)
	}
	exportID, action, ok = parseExportRoute("/v1/exports/exp-1/allow-host")
	if !ok || exportID != "exp-1" || action != "allow-host" {
		t.Fatalf("unexpected action route parse: %v %q %q", ok, exportID, action)
	}
	_, _, ok = parseExportRoute("/v1/exports/exp-1/allow-host/extra")
	if ok {
		t.Fatal("expected invalid route parse failure")
	}
}
