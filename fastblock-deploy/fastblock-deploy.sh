#!/bin/bash

usage() {
    echo "Usage: $0 [options]"
    echo "Options:"
    echo "  -m: monitor ip address separated by comma(,)"
    echo "  -o: osd ip address separated by comma(,)"
    echo "  -c: client ip address separated by comma(,)"
    echo "  -d: disks separated by comma(,), format: 'ip1:disk1,disk2 ip2:disk3,disk4'"
    echo "  -n: rdma nic name"
    echo "  -t: type of bdev, can be nvme or aio, default to aio"

    exit 1
}

if [ "$#" -eq 0 ] || [ "$1" = "--help" ]; then
    usage
fi

# Parse command line arguments
while getopts ":m:o:c:d:n:t:" opt; do
  case $opt in
    m) MONITOR_IPS=(${OPTARG//,/ }) ;;
    o) OSD_IPS=(${OPTARG//,/ }) ;;
    c) CLIENT_IPS=(${OPTARG//,/ }) ;;
    d) DISK_MAP=$OPTARG ;;
    n) NIC=$OPTARG ;;
    t) BDEVTYPE=$OPTARG ;;
    \?) echo "Invalid option -$OPTARG" >&2; exit 1 ;;
    :) echo "Option -$OPTARG requires an argument." >&2; exit 1 ;;
  esac
done

# Check for required parameters
if [ -z "$MONITOR_IPS" ] || [ -z "$OSD_IPS" ] || [ -z "$CLIENT_IPS" ] || [ -z "$DISK_MAP" ] || [ -z "$NIC" ]; then
    echo "Error: Missing required parameters."
    usage
fi

# 1. Install ansible and requirements.
echo "1.Start installing ansible and requirements."
yum -y install ansible
pip install -r requirements.txt
ansible-galaxy install -r requirements.yml

# 2. Configure the inventory file
echo "2.Start configuring the hosts file."
cat > ./hosts <<EOF
[monitors]
EOF
for i in "${!MONITOR_IPS[@]}"; do
    ip=${MONITOR_IPS[$i]}
    if ! [[ $ip =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "Error: Invalid IP address '$ip'."
        exit 1
    fi
    echo "monitor$(($i+1)) ansible_host=$ip" >> ./hosts
done

echo -e "\n[osds]" >> ./hosts
IFS=' ' read -r -a disk_pairs <<< "$DISK_MAP"
declare -A disk_map
for pair in "${disk_pairs[@]}"; do
    IFS=':' read -r ip disks <<< "$pair"
    if ! [[ $ip =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "Error: Invalid IP address '$ip'."
        exit 1
    fi
    disk_map[$ip]=$disks
done

for i in "${!OSD_IPS[@]}"; do
    ip=${OSD_IPS[$i]}
    if ! [[ $ip =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "Error: Invalid IP address '$ip'."
        exit 1
    fi
    disks=${disk_map[$ip]:-"none"}
    echo "osd$(($i+1)) ansible_host=$ip disks=$disks nic=$NIC" >> ./hosts
done

echo -e "\n[clients]" >> ./hosts
for i in "${!CLIENT_IPS[@]}"; do
    ip=${CLIENT_IPS[$i]}
    if ! [[ $ip =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        echo "Error: Invalid IP address '$ip'."
        exit 1
    fi
    echo "client$(($i+1)) ansible_host=$ip" >> ./hosts
done

# 3. Configure secure login
echo "3.Start configuring secure login."
ssh-keygen -t rsa -N "" -f ~/.ssh/id_rsa
for ip in "${MONITOR_IPS[@]}" "${OSD_IPS[@]}" "${CLIENT_IPS[@]}"; do
    if ! ssh-copy-id -i ~/.ssh/id_rsa.pub $ip; then
        echo "Error: Failed to copy SSH key to $ip."
        exit 1
    fi
done

# 4. Copy the rpm packages to fastblock-deploy/roles/fastblock-common/files/.
echo "4.Start copying the rpm packages."
RPM_DIR="$(rpm --eval %{_rpmdir})/$(uname -m)"
cp ${RPM_DIR}/fastblock-mon*.rpm ./roles/fastblock-common/files/fastblock-mon.rpm
cp ${RPM_DIR}/fastblock-osd*.rpm ./roles/fastblock-common/files/fastblock-osd.rpm
cp ${RPM_DIR}/fastblock-v*.rpm ./roles/fastblock-common/files/fastblock.rpm

# 5. Configure bdevtype
echo "5.Start configuring bdevtype."
sed -i "s/^_bdev_type:.*/_bdev_type: ${BDEVTYPE:-aio}/" ./roles/fastblock-defaults/defaults/main.yml

echo "Ansible configuration completed."

exit 0