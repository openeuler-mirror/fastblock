package driver

import (
	"testing"

	"fastblock-csi/pkg/exporterclient"
)

func TestBuildAndParsePublishContext(t *testing.T) {
	publishContext, err := BuildPublishContext(exporterclient.Export{
		ID:      "exp-1",
		NQN:     "nqn.test",
		NSID:    7,
		Traddr:  "10.0.0.10",
		Trsvcid: "4420",
	}, "rdma")
	if err != nil {
		t.Fatalf("build publish context failed: %v", err)
	}
	volumeCtx, err := ParsePublishContext(publishContext)
	if err != nil {
		t.Fatalf("parse publish context failed: %v", err)
	}
	if volumeCtx.Transport != "rdma" || volumeCtx.NSID != 7 {
		t.Fatalf("unexpected volume context: %+v", volumeCtx)
	}
}

func TestParsePublishContextRejectsBadNSID(t *testing.T) {
	_, err := ParsePublishContext(map[string]string{
		PublishContextTransport: "rdma",
		PublishContextNQN:       "nqn.test",
		PublishContextTraddr:    "10.0.0.10",
		PublishContextTrsvcid:   "4420",
		PublishContextNSID:      "bad",
	})
	if err == nil {
		t.Fatal("expected parse error")
	}
}
