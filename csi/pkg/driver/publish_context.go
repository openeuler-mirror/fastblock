package driver

import (
	"errors"
	"fmt"
	"strconv"
	"strings"

	"fastblock-csi/pkg/backend"
	"fastblock-csi/pkg/exporterclient"
)

const (
	PublishContextTransport = "transport"
	PublishContextNQN       = "nqn"
	PublishContextTraddr    = "traddr"
	PublishContextTrsvcid   = "trsvcid"
	PublishContextNSID      = "nsid"
)

func BuildPublishContext(export exporterclient.Export, transport string) (map[string]string, error) {
	if strings.TrimSpace(export.ID) == "" {
		return nil, errors.New("export id is required")
	}
	if strings.TrimSpace(export.NQN) == "" {
		return nil, errors.New("export nqn is required")
	}
	if strings.TrimSpace(export.Traddr) == "" {
		return nil, errors.New("export traddr is required")
	}
	if strings.TrimSpace(export.Trsvcid) == "" {
		return nil, errors.New("export trsvcid is required")
	}
	if export.NSID <= 0 {
		return nil, fmt.Errorf("invalid export nsid %d", export.NSID)
	}
	if transport != "rdma" && transport != "tcp" {
		return nil, fmt.Errorf("unsupported transport %q", transport)
	}
	return map[string]string{
		PublishContextTransport: transport,
		PublishContextNQN:       export.NQN,
		PublishContextTraddr:    export.Traddr,
		PublishContextTrsvcid:   export.Trsvcid,
		PublishContextNSID:      strconv.Itoa(export.NSID),
	}, nil
}

func ParsePublishContext(publishContext map[string]string) (backend.VolumeContext, error) {
	nsid, err := strconv.Atoi(strings.TrimSpace(publishContext[PublishContextNSID]))
	if err != nil {
		return backend.VolumeContext{}, fmt.Errorf("invalid publish context nsid: %w", err)
	}
	volumeCtx := backend.VolumeContext{
		Transport: strings.TrimSpace(publishContext[PublishContextTransport]),
		NQN:       strings.TrimSpace(publishContext[PublishContextNQN]),
		Traddr:    strings.TrimSpace(publishContext[PublishContextTraddr]),
		Trsvcid:   strings.TrimSpace(publishContext[PublishContextTrsvcid]),
		NSID:      nsid,
	}
	if err := backend.ValidateVolumeContext(volumeCtx); err != nil {
		return backend.VolumeContext{}, err
	}
	return volumeCtx, nil
}
