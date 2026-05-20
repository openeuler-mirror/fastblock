package nvmf

import (
	"context"
	"errors"
	"fmt"
	"net"
	"strings"

	"fastblock-exporter/pkg/api"
	"fastblock-exporter/pkg/config"
	"fastblock-exporter/pkg/spdkrpc"
)

type Manager interface {
	CreateExport(ctx context.Context, req api.CreateExportRequest) (api.Export, error)
	DeleteExport(ctx context.Context, exportID string) error
	AllowHost(ctx context.Context, exportID, hostNQN string) error
	DenyHost(ctx context.Context, exportID, hostNQN string) error
}

type LocalManager struct {
	rpc             spdkrpc.Caller
	monitorAddress  string
	targetAddress   string
	targetServiceID string
	nqnPrefix       string
}

func NewLocalManager(cfg config.Config) *LocalManager {
	return newLocalManagerWithRPC(cfg, spdkrpc.New(cfg.RPCSocketPath))
}

func newLocalManagerWithRPC(cfg config.Config, rpc spdkrpc.Caller) *LocalManager {
	return &LocalManager{
		rpc:             rpc,
		monitorAddress:  cfg.MonitorAddress,
		targetAddress:   cfg.TargetAddress,
		targetServiceID: cfg.TargetServiceID,
		nqnPrefix:       strings.TrimRight(cfg.SubsystemNQNPrefix, ":"),
	}
}

func exportID(volumeID string) string {
	replacer := strings.NewReplacer(":", "-", "/", "-", " ", "-", ".", "-")
	return replacer.Replace(volumeID)
}

func bdevName(exportID string) string {
	return fmt.Sprintf("fbdev_%s", exportID)
}

func subsystemNQN(prefix, exportID string) string {
	return fmt.Sprintf("%s:%s", strings.TrimRight(prefix, ":"), exportID)
}

func addressFamily(traddr string) string {
	ip := net.ParseIP(traddr)
	if ip != nil && ip.To4() == nil {
		return "IPv6"
	}
	return "IPv4"
}

func subsystemSerial(exportID string) string {
	serial := strings.ToUpper(strings.ReplaceAll(exportID, "-", ""))
	serial = "FB" + serial
	if len(serial) > 20 {
		return serial[:20]
	}
	return serial
}

func (m *LocalManager) CreateExport(ctx context.Context, req api.CreateExportRequest) (api.Export, error) {
	if err := req.Validate(); err != nil {
		return api.Export{}, err
	}

	id := exportID(req.VolumeID)
	bdev := bdevName(id)
	nqn := subsystemNQN(m.nqnPrefix, id)

	var createdBdev string
	if err := m.rpc.Call(ctx, "bdev_fastblock_create", map[string]any{
		"name":            bdev,
		"pool_name":       req.PoolName,
		"image_name":      req.ImageName,
		"image_size":      req.CapacityBytes,
		"object_size":     req.ObjectSize,
		"block_size":      req.BlockSize,
		"monitor_address": m.monitorAddress,
	}, &createdBdev); err != nil {
		return api.Export{}, err
	}

	if err := m.rpc.Call(ctx, "nvmf_create_subsystem", map[string]any{
		"nqn":            nqn,
		"serial_number":  subsystemSerial(id),
		"model_number":   "FASTBLOCK",
		"allow_any_host": false,
	}, nil); err != nil {
		return api.Export{}, m.cleanupCreateFailure(ctx, "", createdBdev, err)
	}

	var nsid int
	if err := m.rpc.Call(ctx, "nvmf_subsystem_add_ns", map[string]any{
		"nqn": nqn,
		"namespace": map[string]any{
			"bdev_name": createdBdev,
		},
	}, &nsid); err != nil {
		return api.Export{}, m.cleanupCreateFailure(ctx, nqn, createdBdev, err)
	}

	if err := m.rpc.Call(ctx, "nvmf_subsystem_add_listener", map[string]any{
		"nqn": nqn,
		"listen_address": map[string]any{
			"trtype":  strings.ToUpper(req.Transport),
			"adrfam":  addressFamily(m.targetAddress),
			"traddr":  m.targetAddress,
			"trsvcid": m.targetServiceID,
		},
	}, nil); err != nil {
		return api.Export{}, m.cleanupCreateFailure(ctx, nqn, createdBdev, err)
	}

	return api.Export{
		ID:      id,
		NQN:     nqn,
		NSID:    nsid,
		Traddr:  m.targetAddress,
		Trsvcid: m.targetServiceID,
	}, nil
}

func (m *LocalManager) cleanupCreateFailure(ctx context.Context, nqn, bdev string, createErr error) error {
	var cleanupErrors []string
	if nqn != "" {
		if err := m.rpc.Call(ctx, "nvmf_delete_subsystem", map[string]any{"nqn": nqn}, nil); err != nil {
			cleanupErrors = append(cleanupErrors, "delete subsystem: "+err.Error())
		}
	}
	if bdev != "" {
		if err := m.rpc.Call(ctx, "bdev_fastblock_delete", map[string]any{"name": bdev}, nil); err != nil {
			cleanupErrors = append(cleanupErrors, "delete bdev: "+err.Error())
		}
	}
	if len(cleanupErrors) == 0 {
		return createErr
	}
	return fmt.Errorf("%w; cleanup failed: %s", createErr, strings.Join(cleanupErrors, ", "))
}

func (m *LocalManager) DeleteExport(ctx context.Context, exportID string) error {
	if strings.TrimSpace(exportID) == "" {
		return errors.New("export id is required")
	}
	if err := m.rpc.Call(ctx, "nvmf_delete_subsystem", map[string]any{
		"nqn": subsystemNQN(m.nqnPrefix, exportID),
	}, nil); err != nil {
		return err
	}
	return m.rpc.Call(ctx, "bdev_fastblock_delete", map[string]any{
		"name": bdevName(exportID),
	}, nil)
}

func (m *LocalManager) AllowHost(ctx context.Context, exportID, hostNQN string) error {
	if strings.TrimSpace(exportID) == "" {
		return errors.New("export id is required")
	}
	if strings.TrimSpace(hostNQN) == "" {
		return errors.New("host nqn is required")
	}
	return m.rpc.Call(ctx, "nvmf_subsystem_add_host", map[string]any{
		"nqn":  subsystemNQN(m.nqnPrefix, exportID),
		"host": hostNQN,
	}, nil)
}

func (m *LocalManager) DenyHost(ctx context.Context, exportID, hostNQN string) error {
	if strings.TrimSpace(exportID) == "" {
		return errors.New("export id is required")
	}
	if strings.TrimSpace(hostNQN) == "" {
		return errors.New("host nqn is required")
	}
	return m.rpc.Call(ctx, "nvmf_subsystem_remove_host", map[string]any{
		"nqn":  subsystemNQN(m.nqnPrefix, exportID),
		"host": hostNQN,
	}, nil)
}
