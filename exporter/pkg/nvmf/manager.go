package nvmf

import (
	"context"
	"errors"
	"fmt"
	"net"
	"strings"
	"time"

	"fastblock-exporter/pkg/api"
	"fastblock-exporter/pkg/config"
	"fastblock-exporter/pkg/spdkrpc"
)

var ErrExportNotFound = errors.New("export not found")

type Manager interface {
	CreateExport(ctx context.Context, req api.CreateExportRequest) (api.Export, error)
	ListExports(ctx context.Context) ([]api.Export, error)
	GetExport(ctx context.Context, exportID string) (api.Export, error)
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

func exportIDFromNQN(prefix, nqn string) (string, bool) {
	prefix = strings.TrimRight(prefix, ":") + ":"
	if !strings.HasPrefix(nqn, prefix) {
		return "", false
	}
	return strings.TrimPrefix(nqn, prefix), true
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
	if export, err := m.GetExport(ctx, id); err == nil {
		return export, nil
	} else if isIncompleteExport(err) {
		if cleanupErr := m.DeleteExport(ctx, id); cleanupErr != nil {
			return api.Export{}, cleanupErr
		}
	} else if !errors.Is(err, ErrExportNotFound) {
		return api.Export{}, err
	}

	if err := m.ensureTransport(ctx, req.Transport); err != nil {
		return api.Export{}, err
	}
	bdev := bdevName(id)
	nqn := subsystemNQN(m.nqnPrefix, id)

	var createdBdev string
	registerParams := m.buildRegisterExistingBdevParams(req, bdev)
	if err := m.rpc.Call(ctx, "bdev_fastblock_register_existing", registerParams, &createdBdev); err != nil {
		if export, reused := m.reuseExistingExport(ctx, id, err); reused {
			return export, nil
		}
		retry, recoverErr := m.cleanupStaleBdev(ctx, id, bdev, err)
		if recoverErr != nil {
			return api.Export{}, recoverErr
		}
		if !retry {
			return api.Export{}, err
		}
		if retryErr := m.rpc.Call(ctx, "bdev_fastblock_register_existing", registerParams, &createdBdev); retryErr != nil {
			if export, reused := m.reuseExistingExport(ctx, id, retryErr); reused {
				return export, nil
			}
			return api.Export{}, retryErr
		}
	}

	if err := m.rpc.Call(ctx, "nvmf_create_subsystem", buildCreateSubsystemParams(nqn, subsystemSerial(id)), nil); err != nil {
		if export, reused := m.reuseExistingExport(ctx, id, err); reused {
			return export, nil
		}
		return api.Export{}, m.cleanupCreateFailure(ctx, "", createdBdev, err)
	}

	var nsid int
	if err := m.rpc.Call(ctx, "nvmf_subsystem_add_ns", map[string]any{
		"nqn": nqn,
		"namespace": map[string]any{
			"bdev_name": createdBdev,
		},
	}, &nsid); err != nil {
		if export, reused := m.reuseExistingExport(ctx, id, err); reused {
			return export, nil
		}
		return api.Export{}, m.cleanupCreateFailure(ctx, nqn, createdBdev, err)
	}

	if err := m.rpc.Call(ctx, "nvmf_subsystem_add_listener", m.buildListenerParams(req.Transport, nqn), nil); err != nil {
		if export, reused := m.reuseExistingExport(ctx, id, err); reused {
			return export, nil
		}
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

func (m *LocalManager) cleanupStaleBdev(ctx context.Context, exportID, bdev string, createErr error) (bool, error) {
	if !isSPDKAlreadyExists(createErr) {
		return false, nil
	}
	if err := m.rpc.Call(ctx, "bdev_fastblock_delete", map[string]any{"name": bdev}, nil); err != nil && !isSPDKNotFound(err) {
		return false, err
	}
	if _, err := m.GetExport(ctx, exportID); err == nil {
		return false, nil
	} else if !errors.Is(err, ErrExportNotFound) {
		return false, err
	}
	return true, nil
}

func (m *LocalManager) buildRegisterExistingBdevParams(req api.CreateExportRequest, bdev string) map[string]any {
	return map[string]any{
		"name":            bdev,
		"pool_name":       req.PoolName,
		"image_name":      req.ImageName,
		"block_size":      req.BlockSize,
		"monitor_address": m.monitorAddress,
	}
}

func buildCreateSubsystemParams(nqn, serial string) map[string]any {
	return map[string]any{
		"nqn":            nqn,
		"serial_number":  serial,
		"model_number":   "FASTBLOCK",
		"allow_any_host": false,
	}
}

func buildTransportParams(transport string) map[string]any {
	switch strings.ToUpper(transport) {
	case "RDMA":
		return map[string]any{
			"trtype":                  "RDMA",
			"max_queue_depth":         128,
			"max_io_qpairs_per_ctrlr": 8,
			"max_io_size":             131072,
			"in_capsule_data_size":    8192,
		}
	default:
		return map[string]any{
			"trtype":                  "TCP",
			"max_io_qpairs_per_ctrlr": 8,
			"max_io_size":             131072,
			"in_capsule_data_size":    8192,
		}
	}
}

func (m *LocalManager) ensureTransport(ctx context.Context, transport string) error {
	err := m.rpc.Call(ctx, "nvmf_create_transport", buildTransportParams(transport), nil)
	if err != nil && isSPDKAlreadyExists(err) {
		return nil
	}
	return err
}

func (m *LocalManager) buildListenerParams(transport, nqn string) map[string]any {
	return map[string]any{
		"nqn": nqn,
		"listen_address": map[string]any{
			"trtype":  strings.ToUpper(transport),
			"adrfam":  addressFamily(m.targetAddress),
			"traddr":  m.targetAddress,
			"trsvcid": m.targetServiceID,
		},
	}
}

func (m *LocalManager) DeleteExport(ctx context.Context, exportID string) error {
	if strings.TrimSpace(exportID) == "" {
		return errors.New("export id is required")
	}
	if err := m.rpc.Call(ctx, "nvmf_delete_subsystem", map[string]any{
		"nqn": subsystemNQN(m.nqnPrefix, exportID),
	}, nil); err != nil {
		if !isSPDKNotFound(err) {
			if deleted, verifyErr := m.verifyExportDeleted(exportID); verifyErr == nil && deleted {
				return nil
			}
			return err
		}
	}
	if err := m.rpc.Call(ctx, "bdev_fastblock_delete", map[string]any{
		"name": bdevName(exportID),
	}, nil); err != nil {
		if isSPDKNotFound(err) {
			return nil
		}
		if deleted, verifyErr := m.verifyExportDeleted(exportID); verifyErr == nil && deleted {
			return nil
		}
		return err
	}
	return nil
}

func (m *LocalManager) ListExports(ctx context.Context) ([]api.Export, error) {
	var subsystems []subsystemInfo
	if err := m.rpc.Call(ctx, "nvmf_get_subsystems", nil, &subsystems); err != nil {
		if isSPDKNotFound(err) {
			return nil, nil
		}
		return nil, err
	}
	exports := make([]api.Export, 0, len(subsystems))
	for _, subsystem := range subsystems {
		exportID, ok := exportIDFromNQN(m.nqnPrefix, subsystem.NQN)
		if !ok {
			continue
		}
		if len(subsystem.Namespaces) == 0 || len(subsystem.ListenAddresses) == 0 {
			continue
		}
		listener := subsystem.ListenAddresses[0]
		exports = append(exports, api.Export{
			ID:      exportID,
			NQN:     subsystem.NQN,
			NSID:    subsystem.Namespaces[0].NSID,
			Traddr:  listener.Traddr,
			Trsvcid: listener.Trsvcid,
		})
	}
	return exports, nil
}

func (m *LocalManager) GetExport(ctx context.Context, exportID string) (api.Export, error) {
	if strings.TrimSpace(exportID) == "" {
		return api.Export{}, errors.New("export id is required")
	}
	nqn := subsystemNQN(m.nqnPrefix, exportID)
	var subsystems []subsystemInfo
	if err := m.rpc.Call(ctx, "nvmf_get_subsystems", map[string]any{
		"nqn": nqn,
	}, &subsystems); err != nil {
		if isSPDKNotFound(err) {
			return api.Export{}, fmt.Errorf("%w: %s", ErrExportNotFound, exportID)
		}
		return api.Export{}, err
	}
	for _, subsystem := range subsystems {
		if subsystem.NQN != nqn {
			continue
		}
		if len(subsystem.Namespaces) == 0 || len(subsystem.ListenAddresses) == 0 {
			return api.Export{}, fmt.Errorf("subsystem %s missing namespace or listener", nqn)
		}
		listener := subsystem.ListenAddresses[0]
		return api.Export{
			ID:      exportID,
			NQN:     subsystem.NQN,
			NSID:    subsystem.Namespaces[0].NSID,
			Traddr:  listener.Traddr,
			Trsvcid: listener.Trsvcid,
		}, nil
	}
	return api.Export{}, fmt.Errorf("%w: %s", ErrExportNotFound, exportID)
}

func (m *LocalManager) AllowHost(ctx context.Context, exportID, hostNQN string) error {
	if strings.TrimSpace(exportID) == "" {
		return errors.New("export id is required")
	}
	if strings.TrimSpace(hostNQN) == "" {
		return errors.New("host nqn is required")
	}
	err := m.rpc.Call(ctx, "nvmf_subsystem_add_host", map[string]any{
		"nqn":  subsystemNQN(m.nqnPrefix, exportID),
		"host": hostNQN,
	}, nil)
	if err != nil && isSPDKAlreadyExists(err) {
		return nil
	}
	return err
}

func (m *LocalManager) DenyHost(ctx context.Context, exportID, hostNQN string) error {
	if strings.TrimSpace(exportID) == "" {
		return errors.New("export id is required")
	}
	if strings.TrimSpace(hostNQN) == "" {
		return errors.New("host nqn is required")
	}
	err := m.rpc.Call(ctx, "nvmf_subsystem_remove_host", map[string]any{
		"nqn":  subsystemNQN(m.nqnPrefix, exportID),
		"host": hostNQN,
	}, nil)
	if err != nil && (isSPDKNotFound(err) || isSPDKHostAccessMissing(err) || isSPDKInvalidParams(err)) {
		return nil
	}
	return err
}

type subsystemInfo struct {
	NQN             string             `json:"nqn"`
	Namespaces      []subsystemNS      `json:"namespaces"`
	ListenAddresses []subsystemAddress `json:"listen_addresses"`
}

type subsystemNS struct {
	NSID int `json:"nsid"`
}

type subsystemAddress struct {
	Traddr  string `json:"traddr"`
	Trsvcid string `json:"trsvcid"`
}

type bdevInfo struct {
	Name string `json:"name"`
}

func (m *LocalManager) reuseExistingExport(ctx context.Context, exportID string, createErr error) (api.Export, bool) {
	if !isSPDKAlreadyExists(createErr) {
		return api.Export{}, false
	}
	export, err := m.GetExport(ctx, exportID)
	if err != nil {
		return api.Export{}, false
	}
	return export, true
}

func (m *LocalManager) verifyExportDeleted(exportID string) (bool, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()

	subsystems, err := m.getSubsystems(ctx, exportID)
	if err != nil && !isSPDKNotFound(err) {
		return false, err
	}
	if len(subsystems) != 0 {
		return false, nil
	}

	bdevs, err := m.getBdevs(ctx, exportID)
	if err != nil && !isSPDKNotFound(err) {
		return false, err
	}
	return len(bdevs) == 0, nil
}

func (m *LocalManager) getSubsystems(ctx context.Context, exportID string) ([]subsystemInfo, error) {
	var subsystems []subsystemInfo
	err := m.rpc.Call(ctx, "nvmf_get_subsystems", map[string]any{
		"nqn": subsystemNQN(m.nqnPrefix, exportID),
	}, &subsystems)
	return subsystems, err
}

func (m *LocalManager) getBdevs(ctx context.Context, exportID string) ([]bdevInfo, error) {
	var bdevs []bdevInfo
	err := m.rpc.Call(ctx, "bdev_get_bdevs", map[string]any{
		"name": bdevName(exportID),
	}, &bdevs)
	return bdevs, err
}

func isSPDKNotFound(err error) bool {
	var rpcErr *spdkrpc.ResponseError
	if errors.As(err, &rpcErr) {
		return rpcErr.Code == -19 || strings.Contains(strings.ToLower(rpcErr.Message), "no such device")
	}
	return false
}

func isSPDKAlreadyExists(err error) bool {
	var rpcErr *spdkrpc.ResponseError
	if errors.As(err, &rpcErr) {
		msg := strings.ToLower(rpcErr.Message)
		return rpcErr.Code == -17 || strings.Contains(msg, "already exists") || strings.Contains(msg, " exists")
	}
	return false
}

func isSPDKHostAccessMissing(err error) bool {
	var rpcErr *spdkrpc.ResponseError
	if errors.As(err, &rpcErr) {
		msg := strings.ToLower(rpcErr.Message)
		return strings.Contains(msg, "host") && (strings.Contains(msg, "not found") || strings.Contains(msg, "no such"))
	}
	return false
}

func isSPDKInvalidParams(err error) bool {
	var rpcErr *spdkrpc.ResponseError
	if errors.As(err, &rpcErr) {
		return rpcErr.Code == -32602
	}
	return false
}

func isIncompleteExport(err error) bool {
	if err == nil {
		return false
	}
	return strings.Contains(strings.ToLower(err.Error()), "missing namespace or listener")
}
