#!/bin/bash
set -euo pipefail

journal_dir="/var/log/journal"
dropin_dir="/etc/systemd/journald.conf.d"
dropin_file="$dropin_dir/90-kfastblock-crash-capture.conf"
group_name="systemd-journal"
group_owner="root"

if [ "$(id -u)" -ne 0 ]; then
    echo "run as root" >&2
    exit 1
fi

if getent group "$group_name" >/dev/null 2>&1; then
    group_owner="$group_name"
fi

mkdir -p "$journal_dir"
chown root:"$group_owner" "$journal_dir"
chmod 2755 "$journal_dir"

mkdir -p "$dropin_dir"
cat > "$dropin_file" <<'EOF'
[Journal]
Storage=persistent
SystemMaxUse=512M
RuntimeMaxUse=256M
Compress=yes
Seal=yes
ForwardToWall=no
EOF

systemctl restart systemd-journald
journalctl --flush >/dev/null 2>&1 || true

echo "persistent journal enabled"
echo "journal_dir=$journal_dir"
echo "dropin_file=$dropin_file"
echo "systemd-journald status:"
systemctl status systemd-journald --no-pager
echo
echo "recent boots:"
journalctl --list-boots || true
