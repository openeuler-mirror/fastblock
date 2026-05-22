package driver

import (
	"context"
	"testing"
	"time"
)

func TestListenEndpointAndServe(t *testing.T) {
	server, err := ListenEndpoint("tcp://127.0.0.1:0")
	if err != nil {
		t.Fatalf("listen endpoint failed: %v", err)
	}
	server.RegisterIdentity(NewIdentityService(Options{DriverName: "csi.fastblock.io"}))

	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() {
		done <- server.Serve(ctx)
	}()

	time.Sleep(50 * time.Millisecond)
	cancel()

	select {
	case err := <-done:
		if err != nil {
			t.Fatalf("serve returned error: %v", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("server did not stop after cancel")
	}
}
