package server

import (
	"context"
	"encoding/json"
	"errors"
	"log"
	"net/http"
	"strings"
	"time"

	"fastblock-exporter/pkg/api"
	"fastblock-exporter/pkg/config"
	"fastblock-exporter/pkg/nvmf"
)

type Server struct {
	cfg     config.Config
	manager nvmf.Manager
}

func New(cfg config.Config, manager nvmf.Manager) *Server {
	return &Server{cfg: cfg, manager: manager}
}

func (s *Server) Handler() http.Handler {
	mux := http.NewServeMux()
	mux.HandleFunc("/healthz", s.handleHealthz)
	mux.HandleFunc("/v1/exports", s.handleExports)
	mux.HandleFunc("/v1/exports/", s.handleExportAction)
	return mux
}

func (s *Server) Start(ctx context.Context) error {
	httpServer := &http.Server{
		Addr:    s.cfg.ListenAddress,
		Handler: s.Handler(),
	}
	go func() {
		<-ctx.Done()
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		_ = httpServer.Shutdown(shutdownCtx)
	}()
	log.Printf("fastblock exporter listening on %s with spdk socket %s", s.cfg.ListenAddress, s.cfg.RPCSocketPath)
	err := httpServer.ListenAndServe()
	if errors.Is(err, http.ErrServerClosed) {
		return nil
	}
	return err
}

func (s *Server) handleHealthz(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	writeJSON(w, http.StatusOK, map[string]string{"status": "ok", "node_name": s.cfg.NodeName})
}

func (s *Server) handleExports(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	var req api.CreateExportRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, http.StatusBadRequest, "invalid json body")
		return
	}
	if err := req.Validate(); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	export, err := s.manager.CreateExport(r.Context(), req)
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	writeJSON(w, http.StatusCreated, export)
}

func (s *Server) handleExportAction(w http.ResponseWriter, r *http.Request) {
	exportID, action, ok := parseExportRoute(r.URL.Path)
	if !ok {
		writeError(w, http.StatusNotFound, "route not found")
		return
	}
	if action == "" && r.Method == http.MethodDelete {
		if exportID == "" {
			writeError(w, http.StatusBadRequest, "export id is required")
			return
		}
		if err := s.manager.DeleteExport(r.Context(), exportID); err != nil {
			writeError(w, http.StatusInternalServerError, err.Error())
			return
		}
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if action == "" || r.Method != http.MethodPost {
		writeError(w, http.StatusNotFound, "route not found")
		return
	}
	var req api.HostAccessRequest
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeError(w, http.StatusBadRequest, "invalid json body")
		return
	}
	if err := req.Validate(); err != nil {
		writeError(w, http.StatusBadRequest, err.Error())
		return
	}
	var err error
	switch action {
	case "allow-host":
		err = s.manager.AllowHost(r.Context(), exportID, req.HostNQN)
	case "deny-host":
		err = s.manager.DenyHost(r.Context(), exportID, req.HostNQN)
	default:
		writeError(w, http.StatusNotFound, "route not found")
		return
	}
	if err != nil {
		writeError(w, http.StatusInternalServerError, err.Error())
		return
	}
	w.WriteHeader(http.StatusNoContent)
}

func writeJSON(w http.ResponseWriter, status int, v any) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(status)
	_ = json.NewEncoder(w).Encode(v)
}

func writeError(w http.ResponseWriter, status int, message string) {
	writeJSON(w, status, map[string]string{"error": message})
}

func parseExportRoute(path string) (exportID string, action string, ok bool) {
	trimmed := strings.TrimPrefix(path, "/v1/exports/")
	parts := strings.Split(trimmed, "/")
	switch len(parts) {
	case 1:
		return parts[0], "", true
	case 2:
		return parts[0], parts[1], true
	default:
		return "", "", false
	}
}
