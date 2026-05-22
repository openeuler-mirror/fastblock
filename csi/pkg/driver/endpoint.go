package driver

import (
	"fmt"
	"net/url"
	"strings"
)

type Endpoint struct {
	Network string
	Address string
}

func ParseEndpoint(raw string) (Endpoint, error) {
	if strings.TrimSpace(raw) == "" {
		return Endpoint{}, fmt.Errorf("endpoint is required")
	}
	u, err := url.Parse(raw)
	if err != nil {
		return Endpoint{}, err
	}
	switch u.Scheme {
	case "unix":
		if strings.TrimSpace(u.Path) == "" {
			return Endpoint{}, fmt.Errorf("unix endpoint path is required")
		}
		return Endpoint{Network: "unix", Address: u.Path}, nil
	case "tcp":
		if strings.TrimSpace(u.Host) == "" {
			return Endpoint{}, fmt.Errorf("tcp endpoint host is required")
		}
		return Endpoint{Network: "tcp", Address: u.Host}, nil
	default:
		return Endpoint{}, fmt.Errorf("unsupported endpoint scheme %q", u.Scheme)
	}
}
