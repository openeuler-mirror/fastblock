package server

import (
	"context"
	"log"

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

func (s *Server) Start(context.Context) error {
	log.Printf("fastblock exporter skeleton listening on %s with spdk socket %s", s.cfg.ListenAddress, s.cfg.RPCSocketPath)
	return nil
}
