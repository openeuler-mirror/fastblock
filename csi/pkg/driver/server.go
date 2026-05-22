package driver

import (
	"context"
	"fmt"
	"net"
	"os"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
	"google.golang.org/grpc"
)

type Server struct {
	grpcServer *grpc.Server
	listener   net.Listener
	endpoint   Endpoint
}

func NewServer(endpoint Endpoint) (*Server, error) {
	if endpoint.Network == "unix" {
		if err := os.Remove(endpoint.Address); err != nil && !os.IsNotExist(err) {
			return nil, err
		}
	}
	listener, err := net.Listen(endpoint.Network, endpoint.Address)
	if err != nil {
		return nil, err
	}
	return &Server{
		grpcServer: grpc.NewServer(),
		listener:   listener,
		endpoint:   endpoint,
	}, nil
}

func (s *Server) RegisterIdentity(identity csi.IdentityServer) {
	csi.RegisterIdentityServer(s.grpcServer, identity)
}

func (s *Server) RegisterController(controller csi.ControllerServer) {
	csi.RegisterControllerServer(s.grpcServer, controller)
}

func (s *Server) RegisterNode(node csi.NodeServer) {
	csi.RegisterNodeServer(s.grpcServer, node)
}

func (s *Server) Serve(ctx context.Context) error {
	errCh := make(chan error, 1)
	go func() {
		errCh <- s.grpcServer.Serve(s.listener)
	}()

	select {
	case <-ctx.Done():
		s.grpcServer.GracefulStop()
		_ = s.listener.Close()
		return nil
	case err := <-errCh:
		return err
	}
}

func (s *Server) Address() string {
	return s.listener.Addr().String()
}

func ListenEndpoint(raw string) (*Server, error) {
	ep, err := ParseEndpoint(raw)
	if err != nil {
		return nil, fmt.Errorf("parse endpoint: %w", err)
	}
	return NewServer(ep)
}
