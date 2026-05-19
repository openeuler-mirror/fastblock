package spdkrpc

import (
	"context"
	"encoding/json"
	"net"
	"path/filepath"
	"testing"
)

func TestCall(t *testing.T) {
	socketPath := filepath.Join(t.TempDir(), "spdk.sock")
	ln, err := net.Listen("unix", socketPath)
	if err != nil {
		t.Fatalf("listen failed: %v", err)
	}
	defer ln.Close()

	go func() {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		_ = json.NewEncoder(conn).Encode(map[string]any{
			"jsonrpc": "2.0",
			"id":      1,
			"result":  map[string]any{"name": "ok"},
		})
	}()

	client := New(socketPath)
	var result struct {
		Name string `json:"name"`
	}
	if err := client.Call(context.Background(), "test_method", map[string]string{"k": "v"}, &result); err != nil {
		t.Fatalf("call failed: %v", err)
	}
	if result.Name != "ok" {
		t.Fatalf("unexpected result: %+v", result)
	}
}

func TestCallReturnsRPCError(t *testing.T) {
	socketPath := filepath.Join(t.TempDir(), "spdk.sock")
	ln, err := net.Listen("unix", socketPath)
	if err != nil {
		t.Fatalf("listen failed: %v", err)
	}
	defer ln.Close()

	go func() {
		conn, err := ln.Accept()
		if err != nil {
			return
		}
		defer conn.Close()
		_ = json.NewEncoder(conn).Encode(map[string]any{
			"jsonrpc": "2.0",
			"id":      1,
			"error": map[string]any{
				"code":    -1,
				"message": "boom",
			},
		})
	}()

	client := New(socketPath)
	if err := client.Call(context.Background(), "test_method", nil, nil); err == nil {
		t.Fatal("expected rpc error")
	}
}
