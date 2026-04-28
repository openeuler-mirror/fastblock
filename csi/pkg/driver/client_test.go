package driver

import (
	"context"
	"path/filepath"
	"testing"

	csi "github.com/container-storage-interface/spec/lib/go/csi"
)

func TestDialContextUnixAndTCP(t *testing.T) {
	testCases := []struct {
		name     string
		endpoint string
	}{
		{
			name:     "tcp",
			endpoint: "tcp://127.0.0.1:0",
		},
		{
			name:     "unix",
			endpoint: "unix://" + filepath.Join(t.TempDir(), "csi.sock"),
		},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			server, err := ListenEndpoint(tc.endpoint)
			if err != nil {
				t.Fatalf("listen endpoint failed: %v", err)
			}
			server.RegisterIdentity(NewIdentityService(Options{DriverName: "csi.fastblock.io"}))

			ctx, cancel := context.WithCancel(context.Background())
			defer cancel()
			go func() { _ = server.Serve(ctx) }()

			addr := tc.endpoint
			if tc.name == "tcp" {
				addr = "tcp://" + server.Address()
			}

			conn, err := DialContext(context.Background(), addr)
			if err != nil {
				t.Fatalf("dial context failed: %v", err)
			}
			defer conn.Close()

			client := csi.NewIdentityClient(conn)
			resp, err := client.GetPluginInfo(context.Background(), &csi.GetPluginInfoRequest{})
			if err != nil {
				t.Fatalf("GetPluginInfo failed: %v", err)
			}
			if resp.GetName() != "csi.fastblock.io" {
				t.Fatalf("unexpected plugin info: %+v", resp)
			}
			cancel()
		})
	}
}
