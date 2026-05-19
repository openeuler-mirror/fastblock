# fastblock-csi

This module contains the Kubernetes CSI control-plane implementation for fastblock.

Planned binaries:
- `cmd/controller`: CSI controller service
- `cmd/node`: CSI node service

Current status:
- repository skeleton only
- no Kubernetes or CSI dependencies added yet
- scoped to an NVMe-oF-only node path
