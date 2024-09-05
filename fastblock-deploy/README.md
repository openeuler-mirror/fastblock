# fastblock-deploy
基于ansible实现fastblock自动部署

## 1. 集群部署
执行 `make_rpm.sh` 脚本构建rpm包
```bash
../make_rpm.sh
```

执行 `fastblock-deploy/fastblock-deploy.sh` 脚本，脚本完成ansible安装以及环境准备
```bash
./fastblock-deploy.sh -m MONITOR_IPS -o OSD_IPS -c CLIENT_IPS -d OSD_DISKS_MAP -n NIC -t BDEV_TYPE
```
检查 `fastblock-deploy/hosts` 中的场景是否符合实际需求，具体可以查看[说明文档](./docs/configure.md)

运行用于部署的ansible playbook，完成部署
```bash
ansible-playbook -vv -i hosts site.yml
```

查看集群状态：
```bash
fastblock-client -op=status
```

## 2. 收集日志
执行 `fastblock-deploy/infrastructure-playbooks/gather-fastblock-logs.yml` playbook即可收集集群中的日志文件
```bash
ansible-playbook -vv infrastructure-playbooks/gather-fastblock-logs.yml -i hosts
```
收集的文件将保存在 `/tmp/` 目录下，以 `fastblock_deploy` 为前缀的文件夹中

## 3. 集群卸载
执行 `fastblock-deploy/infrastructure-playbooks/purge-cluster.yml` playbook即可卸载集群
```bash
ansible-playbook -vv infrastructure-playbooks/purge-cluster.yml -i hosts
```