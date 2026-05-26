#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ACTION="${1:-up}"
if [[ $# -gt 0 ]]; then
    shift
fi

NAMESPACE="fastblock-csi"
TEST_NAMESPACE="fastblock-csi-test"
DRIVER_NAME="csi.fastblock.io"
NODE_ID="kerneldev"
POOL_NAME="fb"
TRANSPORT="rdma"
K3S_VERSION="v1.31.5+k3s1"
KUBELET_DIR="/var/lib/kubelet"
PLUGIN_IMAGE="docker.io/library/debian:12-slim"
TEST_IMAGE="docker.io/library/busybox:1.36.1"
PROVISIONER_IMAGE="registry.k8s.io/sig-storage/csi-provisioner:v5.1.0"
ATTACHER_IMAGE="registry.k8s.io/sig-storage/csi-attacher:v4.8.0"
REGISTRAR_IMAGE="registry.k8s.io/sig-storage/csi-node-driver-registrar:v2.11.1"
HOST_IP="${HOST_IP:-}"
BUILD=1

usage() {
    cat <<'EOF'
Usage:
  scripts/k8s-csi-lab.sh [up|down|status|test|uninstall-k3s] [options]

Actions:
  up             Ensure host lab, install k3s if needed, deploy CSI manifests, and run PVC/Pod validation.
  down           Remove CSI manifests and test workloads, leave k3s installed.
  status         Show k3s, CSI, and test workload status.
  test           Re-run only the PVC/Pod validation flow against an existing deployment.
  uninstall-k3s  Remove k3s from this machine.

Options:
  --host-ip <ip>          Host IP used for monitor/exporter/nvmf target (auto-detect by default)
  --transport <name>      StorageClass transport: rdma or tcp (default: rdma)
  --pool <name>           Fastblock pool name (default: fb)
  --skip-build            Reuse existing binaries instead of rebuilding through scripts/csi-lab.sh
  --k3s-version <ver>     K3s version to install when needed (default: v1.31.5+k3s1)
  -h, --help              Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --host-ip)
            HOST_IP="$2"
            shift 2
            ;;
        --transport)
            TRANSPORT="$2"
            shift 2
            ;;
        --pool)
            POOL_NAME="$2"
            shift 2
            ;;
        --skip-build)
            BUILD=0
            shift
            ;;
        --k3s-version)
            K3S_VERSION="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown argument: $1" >&2
            usage
            exit 1
            ;;
    esac
done

log() {
    printf '[k8s-csi] %s\n' "$*"
}

die() {
    printf '[k8s-csi] ERROR: %s\n' "$*" >&2
    exit 1
}

require_root() {
    if [[ "$(id -u)" -ne 0 ]]; then
        die "this script must run as root"
    fi
}

ensure_host_ip() {
    if [[ -n "$HOST_IP" ]]; then
        return
    fi
    HOST_IP="$(ip route get 1.1.1.1 2>/dev/null | awk '{for (i = 1; i <= NF; i++) if ($i == "src") {print $(i+1); exit}}')"
    if [[ -z "$HOST_IP" ]]; then
        HOST_IP="$(ip -o -4 addr show up scope global | awk '{split($4, a, "/"); print a[1]; exit}')"
    fi
    [[ -n "$HOST_IP" ]] || die "failed to detect host IP"
}

ensure_k3s() {
    if command -v kubectl >/dev/null 2>&1 && kubectl get nodes >/dev/null 2>&1; then
        log "k3s is already available"
        return
    fi
    log "installing k3s ${K3S_VERSION}"
    curl -sfL https://get.k3s.io | \
        INSTALL_K3S_SKIP_SELINUX_RPM=true \
        INSTALL_K3S_SELINUX_WARN=true \
        INSTALL_K3S_VERSION="$K3S_VERSION" \
        INSTALL_K3S_EXEC="server --disable traefik --disable servicelb --write-kubeconfig-mode 644 --kubelet-arg=eviction-hard=nodefs.available<1%,nodefs.inodesFree<1% --kubelet-arg=eviction-minimum-reclaim=nodefs.available=0Mi" \
        sh -
    wait_for_k3s
}

wait_for_k3s() {
    local i
    for ((i = 0; i < 120; i++)); do
        if kubectl get nodes >/dev/null 2>&1; then
            if kubectl get node "$(hostname -s)" >/dev/null 2>&1 && kubectl get node "$(hostname -s)" -o jsonpath='{.status.conditions[?(@.type=="Ready")].status}' 2>/dev/null | grep -q True; then
                return
            fi
        fi
        sleep 2
    done
    systemctl status k3s --no-pager -n 80 || true
    die "k3s did not become ready"
}

ensure_host_lab() {
    ensure_host_ip
    local args=(up --skip-smoke --smoke-transport "$TRANSPORT")
    if [[ "$BUILD" -eq 0 ]]; then
        args+=(--skip-build)
    fi
    "$ROOT/scripts/csi-lab.sh" "${args[@]}"
}

build_csi_binaries() {
    (
        cd "$ROOT/csi"
        go build -o bin/fastblock-csi-controller ./cmd/controller
        go build -o bin/fastblock-csi-node ./cmd/node
    )
}

apply_manifests() {
    ensure_host_ip
    build_csi_binaries

    kubectl apply -f - <<EOF
apiVersion: v1
kind: Namespace
metadata:
  name: ${NAMESPACE}
---
apiVersion: v1
kind: ServiceAccount
metadata:
  name: fastblock-csi-controller
  namespace: ${NAMESPACE}
---
apiVersion: rbac.authorization.k8s.io/v1
kind: ClusterRoleBinding
metadata:
  name: fastblock-csi-controller-cluster-admin
roleRef:
  apiGroup: rbac.authorization.k8s.io
  kind: ClusterRole
  name: cluster-admin
subjects:
- kind: ServiceAccount
  name: fastblock-csi-controller
  namespace: ${NAMESPACE}
---
apiVersion: storage.k8s.io/v1
kind: CSIDriver
metadata:
  name: ${DRIVER_NAME}
spec:
  attachRequired: true
  podInfoOnMount: false
  fsGroupPolicy: None
  volumeLifecycleModes:
  - Persistent
---
apiVersion: apps/v1
kind: Deployment
metadata:
  name: fastblock-csi-controller
  namespace: ${NAMESPACE}
spec:
  replicas: 1
  selector:
    matchLabels:
      app: fastblock-csi-controller
  template:
    metadata:
      labels:
        app: fastblock-csi-controller
    spec:
      serviceAccountName: fastblock-csi-controller
      nodeSelector:
        kubernetes.io/os: linux
      tolerations:
      - operator: Exists
      containers:
      - name: plugin
        image: ${PLUGIN_IMAGE}
        imagePullPolicy: IfNotPresent
        command:
        - /opt/fastblock/bin/fastblock-csi-controller
        - -endpoint=unix:///csi/csi.sock
        - -driver-name=${DRIVER_NAME}
        - -monitor-address=${HOST_IP}:3333
        - -exporter-endpoint=http://${HOST_IP}:9500
        - -default-host-nqn-file=/etc/nvme/hostnqn
        volumeMounts:
        - name: csi-socket
          mountPath: /csi
        - name: plugin-bin
          mountPath: /opt/fastblock/bin
          readOnly: true
        - name: host-nvme
          mountPath: /etc/nvme
          readOnly: true
      - name: csi-provisioner
        image: ${PROVISIONER_IMAGE}
        imagePullPolicy: IfNotPresent
        args:
        - --csi-address=/csi/csi.sock
        - --leader-election
        - --timeout=60s
        volumeMounts:
        - name: csi-socket
          mountPath: /csi
      - name: csi-attacher
        image: ${ATTACHER_IMAGE}
        imagePullPolicy: IfNotPresent
        args:
        - --csi-address=/csi/csi.sock
        - --leader-election
        - --timeout=60s
        volumeMounts:
        - name: csi-socket
          mountPath: /csi
      volumes:
      - name: csi-socket
        emptyDir: {}
      - name: plugin-bin
        hostPath:
          path: ${ROOT}/csi/bin
          type: Directory
      - name: host-nvme
        hostPath:
          path: /etc/nvme
          type: Directory
---
apiVersion: apps/v1
kind: DaemonSet
metadata:
  name: fastblock-csi-node
  namespace: ${NAMESPACE}
spec:
  selector:
    matchLabels:
      app: fastblock-csi-node
  template:
    metadata:
      labels:
        app: fastblock-csi-node
    spec:
      hostNetwork: true
      dnsPolicy: ClusterFirstWithHostNet
      nodeSelector:
        kubernetes.io/os: linux
      tolerations:
      - operator: Exists
      containers:
      - name: plugin
        image: ${PLUGIN_IMAGE}
        imagePullPolicy: IfNotPresent
        command:
        - /bin/sh
        - -ec
        - |
          mkdir -p ${KUBELET_DIR}/plugins/${DRIVER_NAME}
          mkdir -p /host-tools
          cat >/host-tools/nvme <<'EOS'
          #!/bin/sh
          exec /host-libs/lib64/ld-linux-x86-64.so.2 --library-path /host-libs/usr/lib64:/host-libs/lib64 /host-bin/nvme.real "\$@"
          EOS
          chmod +x /host-tools/nvme
          export PATH=/host-tools:/usr/sbin:/usr/bin:/sbin:/bin
          exec /opt/fastblock/bin/fastblock-csi-node \
            -endpoint=unix://${KUBELET_DIR}/plugins/${DRIVER_NAME}/csi.sock \
            -driver-name=${DRIVER_NAME} \
            -node-id=\${KUBE_NODE_NAME}
        env:
        - name: KUBE_NODE_NAME
          valueFrom:
            fieldRef:
              fieldPath: spec.nodeName
        securityContext:
          privileged: true
        volumeMounts:
        - name: plugin-bin
          mountPath: /opt/fastblock/bin
          readOnly: true
        - name: kubelet-dir
          mountPath: ${KUBELET_DIR}
          mountPropagation: Bidirectional
        - name: registration-dir
          mountPath: /registration
        - name: host-dev
          mountPath: /dev
        - name: host-sys
          mountPath: /sys
        - name: host-udev
          mountPath: /run/udev
          readOnly: true
        - name: host-nvme
          mountPath: /etc/nvme
          readOnly: true
        - name: host-tools
          mountPath: /host-tools
        - name: host-nvme-bin
          mountPath: /host-bin/nvme.real
          readOnly: true
        - name: host-lib64
          mountPath: /host-libs/lib64
          readOnly: true
        - name: host-usr-lib64
          mountPath: /host-libs/usr/lib64
          readOnly: true
      - name: node-driver-registrar
        image: ${REGISTRAR_IMAGE}
        imagePullPolicy: IfNotPresent
        args:
        - --csi-address=${KUBELET_DIR}/plugins/${DRIVER_NAME}/csi.sock
        - --kubelet-registration-path=${KUBELET_DIR}/plugins/${DRIVER_NAME}/csi.sock
        volumeMounts:
        - name: kubelet-dir
          mountPath: ${KUBELET_DIR}
        - name: registration-dir
          mountPath: /registration
      volumes:
      - name: plugin-bin
        hostPath:
          path: ${ROOT}/csi/bin
          type: Directory
      - name: kubelet-dir
        hostPath:
          path: ${KUBELET_DIR}
          type: Directory
      - name: registration-dir
        hostPath:
          path: ${KUBELET_DIR}/plugins_registry
          type: DirectoryOrCreate
      - name: host-dev
        hostPath:
          path: /dev
          type: Directory
      - name: host-sys
        hostPath:
          path: /sys
          type: Directory
      - name: host-udev
        hostPath:
          path: /run/udev
          type: DirectoryOrCreate
      - name: host-nvme
        hostPath:
          path: /etc/nvme
          type: Directory
      - name: host-tools
        emptyDir: {}
      - name: host-nvme-bin
        hostPath:
          path: /usr/sbin/nvme
          type: File
      - name: host-lib64
        hostPath:
          path: /lib64
          type: Directory
      - name: host-usr-lib64
        hostPath:
          path: /usr/lib64
          type: Directory
---
apiVersion: storage.k8s.io/v1
kind: StorageClass
metadata:
  name: fastblock-${TRANSPORT}
provisioner: ${DRIVER_NAME}
reclaimPolicy: Delete
volumeBindingMode: Immediate
allowVolumeExpansion: false
parameters:
  pool: ${POOL_NAME}
  objectSize: "4194304"
  blockSize: "4096"
  transport: ${TRANSPORT}
EOF
}

wait_for_workload() {
    kubectl rollout status deployment/fastblock-csi-controller -n "$NAMESPACE" --timeout=300s
    kubectl rollout status daemonset/fastblock-csi-node -n "$NAMESPACE" --timeout=300s
    kubectl get pods -n "$NAMESPACE" -o wide
}

apply_test_workload() {
    kubectl apply -f - <<EOF
apiVersion: v1
kind: Namespace
metadata:
  name: ${TEST_NAMESPACE}
---
apiVersion: v1
kind: PersistentVolumeClaim
metadata:
  name: fastblock-pvc
  namespace: ${TEST_NAMESPACE}
spec:
  accessModes:
  - ReadWriteOnce
  volumeMode: Block
  storageClassName: fastblock-${TRANSPORT}
  resources:
    requests:
      storage: 16Mi
---
apiVersion: v1
kind: Pod
metadata:
  name: fastblock-block-pod
  namespace: ${TEST_NAMESPACE}
spec:
  restartPolicy: Never
  containers:
  - name: app
    image: ${TEST_IMAGE}
    imagePullPolicy: IfNotPresent
    command:
    - sh
    - -ec
    - |
      ls -l /dev/fastblock0
      dd if=/dev/zero of=/dev/fastblock0 bs=4096 count=1
      sleep 3600
    securityContext:
      privileged: true
    volumeDevices:
    - name: data
      devicePath: /dev/fastblock0
  volumes:
  - name: data
    persistentVolumeClaim:
      claimName: fastblock-pvc
EOF
}

wait_for_pvc_bound() {
    local i phase
    for ((i = 0; i < 180; i++)); do
        phase="$(kubectl get pvc fastblock-pvc -n "$TEST_NAMESPACE" -o jsonpath='{.status.phase}' 2>/dev/null || true)"
        if [[ "$phase" == "Bound" ]]; then
            return
        fi
        sleep 2
    done
    kubectl describe pvc fastblock-pvc -n "$TEST_NAMESPACE" || true
    die "PVC did not bind"
}

wait_for_pod_ready() {
    local i phase
    for ((i = 0; i < 180; i++)); do
        phase="$(kubectl get pod fastblock-block-pod -n "$TEST_NAMESPACE" -o jsonpath='{.status.phase}' 2>/dev/null || true)"
        if [[ "$phase" == "Running" ]]; then
            if kubectl get pod fastblock-block-pod -n "$TEST_NAMESPACE" -o jsonpath='{.status.containerStatuses[0].ready}' 2>/dev/null | grep -q true; then
                return
            fi
        fi
        sleep 2
    done
    kubectl describe pod fastblock-block-pod -n "$TEST_NAMESPACE" || true
    kubectl logs fastblock-block-pod -n "$TEST_NAMESPACE" --all-containers=true || true
    die "test pod did not become ready"
}

run_test() {
    apply_test_workload
    wait_for_pvc_bound
    wait_for_pod_ready
    kubectl get pvc,pv,pod -n "$TEST_NAMESPACE" -o wide
    kubectl logs fastblock-block-pod -n "$TEST_NAMESPACE" --tail=20 || true
}

down() {
    kubectl delete namespace "$TEST_NAMESPACE" --ignore-not-found=true --wait=false >/dev/null 2>&1 || true
    kubectl delete namespace "$NAMESPACE" --ignore-not-found=true --wait=false >/dev/null 2>&1 || true
}

show_status() {
    kubectl get nodes -o wide || true
    kubectl get pods -n "$NAMESPACE" -o wide || true
    kubectl get pods -n "$TEST_NAMESPACE" -o wide || true
    kubectl get pvc,pv -n "$TEST_NAMESPACE" || true
    kubectl get sc fastblock-"$TRANSPORT" || true
}

uninstall_k3s() {
    if [[ -x /usr/local/bin/k3s-uninstall.sh ]]; then
        /usr/local/bin/k3s-uninstall.sh
    fi
}

up() {
    require_root
    ensure_host_lab
    ensure_k3s
    apply_manifests
    wait_for_workload
    run_test
    show_status
}

case "$ACTION" in
    up)
        up
        ;;
    down)
        down
        ;;
    status)
        show_status
        ;;
    test)
        ensure_k3s
        run_test
        ;;
    uninstall-k3s)
        uninstall_k3s
        ;;
    *)
        usage
        die "unknown action: $ACTION"
        ;;
esac
