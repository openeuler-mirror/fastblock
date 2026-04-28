package volumeid

import (
	"errors"
	"fmt"
	"strconv"
	"strings"
)

const (
	prefix        = "fbvol"
	nameRefPrefix = "fbvolname"
)

type ID struct {
	ClusterID string
	PoolID    int64
	ImageID   int64
}

func Encode(id ID) (string, error) {
	if strings.TrimSpace(id.ClusterID) == "" {
		return "", errors.New("cluster id is required")
	}
	if id.PoolID < 0 {
		return "", errors.New("pool id must be non-negative")
	}
	if id.ImageID < 0 {
		return "", errors.New("image id must be non-negative")
	}
	return fmt.Sprintf("%s:%s:%d:%d", prefix, id.ClusterID, id.PoolID, id.ImageID), nil
}

func Decode(raw string) (ID, error) {
	parts := strings.Split(raw, ":")
	if len(parts) != 4 {
		return ID{}, fmt.Errorf("invalid volume id %q", raw)
	}
	if parts[0] != prefix {
		return ID{}, fmt.Errorf("invalid volume id prefix %q", parts[0])
	}
	poolID, err := strconv.ParseInt(parts[2], 10, 64)
	if err != nil {
		return ID{}, fmt.Errorf("invalid pool id %q: %w", parts[2], err)
	}
	imageID, err := strconv.ParseInt(parts[3], 10, 64)
	if err != nil {
		return ID{}, fmt.Errorf("invalid image id %q: %w", parts[3], err)
	}
	return ID{ClusterID: parts[1], PoolID: poolID, ImageID: imageID}, nil
}

type NameRef struct {
	Pool string
	Name string
}

func EncodeNameRef(ref NameRef) (string, error) {
	if strings.TrimSpace(ref.Pool) == "" {
		return "", errors.New("pool is required")
	}
	if strings.TrimSpace(ref.Name) == "" {
		return "", errors.New("name is required")
	}
	return fmt.Sprintf("%s:%s:%s", nameRefPrefix, ref.Pool, ref.Name), nil
}

func DecodeNameRef(raw string) (NameRef, error) {
	parts := strings.Split(raw, ":")
	if len(parts) != 3 {
		return NameRef{}, fmt.Errorf("invalid name ref %q", raw)
	}
	if parts[0] != nameRefPrefix {
		return NameRef{}, fmt.Errorf("invalid name ref prefix %q", parts[0])
	}
	if strings.TrimSpace(parts[1]) == "" || strings.TrimSpace(parts[2]) == "" {
		return NameRef{}, fmt.Errorf("invalid name ref %q", raw)
	}
	return NameRef{Pool: parts[1], Name: parts[2]}, nil
}
