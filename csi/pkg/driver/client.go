package driver

import (
	"context"
	"net"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

func DialContext(ctx context.Context, rawEndpoint string) (*grpc.ClientConn, error) {
	ep, err := ParseEndpoint(rawEndpoint)
	if err != nil {
		return nil, err
	}
	if ep.Network == "unix" {
		return grpc.DialContext(
			ctx,
			"unix://"+ep.Address,
			grpc.WithTransportCredentials(insecure.NewCredentials()),
			grpc.WithContextDialer(func(ctx context.Context, _ string) (net.Conn, error) {
				var d net.Dialer
				return d.DialContext(ctx, "unix", ep.Address)
			}),
		)
	}
	return grpc.DialContext(
		ctx,
		ep.Address,
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
}
