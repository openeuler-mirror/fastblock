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
	if r.Method == http.MethodGet {
		exports, err := s.manager.ListExports(r.Context())
		if err != nil {
			writeError(w, http.StatusInternalServerError, err.Error())
			return
		}
		writeJSON(w, http.StatusOK, exports)
		return
	}
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
	parts, ok := parseExportRouteParts(r.URL.Path)
	if !ok || len(parts) == 0 {
		writeError(w, http.StatusNotFound, "route not found")
		return
	}
	exportID := parts[0]
	if len(parts) == 1 && r.Method == http.MethodDelete {
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
	if len(parts) == 1 && r.Method == http.MethodGet {
		if exportID == "" {
			writeError(w, http.StatusBadRequest, "export id is required")
			return
		}
		export, err := s.manager.GetExport(r.Context(), exportID)
		if err != nil {
			if errors.Is(err, nvmf.ErrExportNotFound) {
				writeError(w, http.StatusNotFound, err.Error())
				return
			}
			writeError(w, http.StatusInternalServerError, err.Error())
			return
		}
		writeJSON(w, http.StatusOK, export)
		return
	}
	if len(parts) == 2 && parts[1] == "flatten" && r.Method == http.MethodPost {
		if err := s.manager.FlattenExport(r.Context(), exportID); err != nil {
			writeError(w, http.StatusInternalServerError, err.Error())
			return
		}
		w.WriteHeader(http.StatusNoContent)
		return
	}
	if len(parts) >= 2 && parts[1] == "snapshots" {
		s.handleSnapshotAction(w, r, exportID, parts[2:])
		return
	}
	if len(parts) == 2 && r.Method == http.MethodPost {
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
		switch parts[1] {
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
		return
	}
	writeError(w, http.StatusNotFound, "route not found")
}

func (s *Server) handleSnapshotAction(w http.ResponseWriter, r *http.Request, exportID string, parts []string) {
	if len(parts) == 0 {
		switch r.Method {
		case http.MethodGet:
			items, err := s.manager.ListSnapshots(r.Context(), exportID)
			if err != nil {
				writeError(w, http.StatusInternalServerError, err.Error())
				return
			}
			writeJSON(w, http.StatusOK, items)
			return
		case http.MethodPost:
			var req api.SnapshotRequest
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
				writeError(w, http.StatusBadRequest, "invalid json body")
				return
			}
			if err := req.Validate(); err != nil {
				writeError(w, http.StatusBadRequest, err.Error())
				return
			}
			if err := s.manager.CreateSnapshot(r.Context(), exportID, req.SnapshotName); err != nil {
				writeError(w, http.StatusInternalServerError, err.Error())
				return
			}
			w.WriteHeader(http.StatusNoContent)
			return
		default:
			writeError(w, http.StatusMethodNotAllowed, "method not allowed")
			return
		}
	}

	snapshotName := parts[0]
	if len(parts) == 1 {
		switch r.Method {
		case http.MethodGet:
			item, err := s.manager.GetSnapshot(r.Context(), exportID, snapshotName)
			if err != nil {
				writeError(w, http.StatusInternalServerError, err.Error())
				return
			}
			writeJSON(w, http.StatusOK, item)
			return
		case http.MethodDelete:
			if err := s.manager.DeleteSnapshot(r.Context(), exportID, snapshotName); err != nil {
				writeError(w, http.StatusInternalServerError, err.Error())
				return
			}
			w.WriteHeader(http.StatusNoContent)
			return
		default:
			writeError(w, http.StatusMethodNotAllowed, "method not allowed")
			return
		}
	}

	if len(parts) == 2 && r.Method == http.MethodPost {
		switch parts[1] {
		case "protect":
			if err := s.manager.ProtectSnapshot(r.Context(), exportID, snapshotName); err != nil {
				writeError(w, http.StatusInternalServerError, err.Error())
				return
			}
		case "unprotect":
			if err := s.manager.UnprotectSnapshot(r.Context(), exportID, snapshotName); err != nil {
				writeError(w, http.StatusInternalServerError, err.Error())
				return
			}
		case "clone":
			var req api.CloneFromSnapshotRequest
			if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
				writeError(w, http.StatusBadRequest, "invalid json body")
				return
			}
			req.SnapshotName = snapshotName
			if err := req.Validate(); err != nil {
				writeError(w, http.StatusBadRequest, err.Error())
				return
			}
			if err := s.manager.CreateCloneFromSnapshot(r.Context(), exportID, snapshotName, req.CloneImageName); err != nil {
				writeError(w, http.StatusInternalServerError, err.Error())
				return
			}
		default:
			writeError(w, http.StatusNotFound, "route not found")
			return
		}
		w.WriteHeader(http.StatusNoContent)
		return
	}
	writeError(w, http.StatusNotFound, "route not found")
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
	parts, ok := parseExportRouteParts(path)
	if !ok || len(parts) == 0 {
		return "", "", false
	}
	if len(parts) == 1 {
		return parts[0], "", true
	}
	if len(parts) == 2 {
		return parts[0], parts[1], true
	}
	return "", "", false
}

func parseExportRouteParts(path string) (parts []string, ok bool) {
	trimmed := strings.TrimPrefix(path, "/v1/exports/")
	if trimmed == path || trimmed == "" {
		return nil, false
	}
	parts = strings.Split(trimmed, "/")
	for _, part := range parts {
		if part == "" {
			return nil, false
		}
	}
	return parts, true
}
