#!/bin/python

import argparse
import asyncio
import json
import os
import subprocess


LIST_IPV4 = """
DEVS=$(ls /sys/class/infiniband/)
for d in $DEVS ; do
	for p in $(ls /sys/class/infiniband/$d/ports/) ; do
		for g in $(ls /sys/class/infiniband/$d/ports/$p/gids/) ; do
			gid=$(cat /sys/class/infiniband/$d/ports/$p/gids/$g);
			if [ $gid = 0000:0000:0000:0000:0000:0000:0000:0000 ] ; then
				continue
			fi
			if [ $gid = fe80:0000:0000:0000:0000:0000:0000:0000 ] ; then
				continue
			fi
			_ndev=$(cat /sys/class/infiniband/$d/ports/$p/gid_attrs/ndevs/$g 2>/dev/null)
			__type=$(cat /sys/class/infiniband/$d/ports/$p/gid_attrs/types/$g 2>/dev/null)
			_type=$(echo $__type| grep -o "[Vv].*")
			if [ $(echo $gid | cut -d ":" -f -1) = "0000" ] ; then
				ipv4=$(printf "%d.%d.%d.%d" 0x${gid:30:2} 0x${gid:32:2} 0x${gid:35:2} 0x${gid:37:2})
				echo -e "$d $p $g $ipv4"
			fi
		done #g (gid)
	done #p (port)
done #d (dev)
"""


def prepare_cmd_args():
    parser = argparse.ArgumentParser(description="generate supervisord conf for rpc_bench")
    parser.add_argument("--log-flags", type=str, help="spdk app log flags, like '-L msg -L osd' ...", default='')
    parser.add_argument("--start-core", type=int, help="start of cpu core number, default is 0", default=0)
    parser.add_argument("--build-path-suffix", type=str, help="for debug test, may ignore it", default=None)
    parser.add_argument("--ci", action="store_true", help="for ci test, may ignore it", default=False)
    parser.add_argument("--node-count", type=int, help="for ci test, specify rpc_bench proc count, at least 2", default=2)
    parser.add_argument("--start-port", type=int, help="for ci test, start of rpc_bench used port no.", default=3000)
    parser.add_argument("--net-device", type=str, help="net device name, for soft roce", default=None)
    parser.add_argument("--rdma-device", type=str, help="rdma device name, for soft roce", default='rxe0')
    return parser.parse_args()


def fastblock_home_path():
    try:
        git_root = subprocess.check_output(["git", "rev-parse", "--show-toplevel"]).decode("utf-8").strip()
        return git_root
    except subprocess.CalledProcessError:
        return None


def rpc_bench_path(fastblock_home, build_suffix):
    build_dir = 'build'
    if not build_suffix is None:
        build_dir = f'build.{build_suffix}'
    bin_path = os.path.join(fastblock_home, build_dir, 'src', 'tools', 'rpc_bench', 'rpc_bench')
    conf_path = os.path.join(fastblock_home, 'src', 'tools', 'rpc_bench', 'rpc_bench.json')
    return bin_path, conf_path


def count_core_num(eps, is_same_cli_core, index):
    count = len(eps[index]['list']) # server core count
    if is_same_cli_core:
        return count + 1

    for i in range(len(eps)):
        if i + 1 == eps[index]['index']:
            continue
        count += len(eps[i]['list'])
    return count


def format_core_arg(n_core, start):
    cores = [f'{i + start}' for i in range(n_core)]
    return ','.join(cores)


def update_rpc_conf(conf_path, rpc_bench_conf, node_num, start_port):
    if node_num < 2:
        print('node_num must be at least 2')
        exit(1)

    proc = subprocess.Popen(['bash', '-c', LIST_IPV4],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                            text=True)
    stdout, stderr = proc.communicate()
    if proc.returncode != 0:
        print('list ipv4 failed')
        exit(1)

    if stderr:
        print(f'list ipv4 failed: {stderr}')
        exit(1)

    for result in stdout.splitlines():
        if result.isspace():
            continue

        print(result)
        splited = result.split(' ')
        dev = splited[0]
        port = int(splited[1])
        index = int(splited[2])
        ipv4 = splited[3]

        print(f'dev: {dev}, port: {port}, index: {index}, ipv4: {ipv4}')

        eps = []
        for i in range(node_num):
            node = {
                'index': i + 1,
                'list': [
                    {'host': ipv4, 'port': start_port + i}
                ]
            }
            eps.append(node)

        rpc_bench_conf['endpoints'] = eps
        rpc_bench_conf['rdma_device_name'] = dev
        rpc_bench_conf['rdma_device_port'] = port
        rpc_bench_conf['rdma_gid_index'] = index
        rpc_bench_conf['msg_server_metadata_memory_pool_capacity'] = 1024
        rpc_bench_conf['msg_server_data_memory_pool_capacity'] = 1024
        rpc_bench_conf['io_size'] = 4096
        rpc_bench_conf['io_count'] = 1024
        rpc_bench_conf['rpc_client_same_core'] = True
        with open(conf_path, 'w') as f:
            json.dump(rpc_bench_conf, f, indent=4)

        return rpc_bench_conf

    print('list ipv4 got empty result')
    exit(1)


def prepare_soft_roce(netdev, rdma_dev):
    print('run "modprobe rdma_rxe"')
    try:
        subprocess.run(['modprobe', 'rdma_rxe'], check=True)
        print('modprobe rdma_rxe success')
    except subprocess.CalledProcessError as e:
        print(f'modprobe rdma_rxe failed: {e}')
        exit(1)

    print(f'run "rdma link show {rdma_dev}"')
    proc = subprocess.Popen(['rdma', 'link', 'show', rdma_dev],
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                            text=True)
    stdout, stderr = proc.communicate()
    if proc.returncode != 0:
        if not stderr.startswith('Wrong device name'):
            print(f'rdma link show {rdma_dev} failed: {stderr}')
            exit(1)
        else:
            print(f'run "rdma link del {rdma_dev}"')
            try:
                subprocess.run(['rdma', 'link', 'del', rdma_dev], check=True)
                print(f'"rdma link del {rdma_dev}" success')
            except subprocess.CalledProcessError as e:
                print(f'"rdma link del {rdma_dev}" failed: {e}')
                exit(1)

    print(f'run "rdma link add {rdma_dev} type rxe netdev {netdev}"')
    try:
        subprocess.run(['rdma', 'link', 'add', rdma_dev, 'type', 'rxe', 'netdev', netdev], check=True)
        print(f'"rdma link add {rdma_dev} type rxe netdev {netdev}" success')
    except subprocess.CalledProcessError as e:
        print(f'"rdma link add {rdma_dev} type rxe netdev {netdev}" failed: {e}')
        exit(1)


async def process_batch(batch, label):
    print(f"{label}:")
    for line in batch:
        print(line)
    print(f"{label} - Batch processed")


async def read_and_process_output_in_batches(stream, label, batch_size=10):
    batch = []
    while True:
        line = await stream.readline()
        if not line:
            break
        decoded_line = line.decode().rstrip()
        batch.append(decoded_line)
        if len(batch) >= batch_size:
            await process_batch(batch, label)
            batch.clear()
    if batch:
        await process_batch(batch, label)


async def run_command_with_batched_output(cmd, cmd_name):
    process = await asyncio.create_subprocess_exec(
        *cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE
    )
    stdout_task = asyncio.create_task(read_and_process_output_in_batches(process.stdout, f"{cmd_name} STDOUT"))
    stderr_task = asyncio.create_task(read_and_process_output_in_batches(process.stderr, f"{cmd_name} STDERR"))
    await process.wait()
    stdout_task.cancel()
    stderr_task.cancel()

    return process.returncode


async def run_rpc_bench(cmds):
    tasks = []
    for i, cmd in enumerate(cmds):
        task = asyncio.create_task(run_command_with_batched_output(cmd, f'{app_base_name}_{i + 1}'))
        print(f'execed {" ".join(cmd)}')
        tasks.append(task)

    await asyncio.gather(*tasks)


if __name__ == '__main__':
    args = prepare_cmd_args()
    fastblock_home = fastblock_home_path()
    if fastblock_home is None:
        print('not a git repo')
        exit(1)

    bin_path, conf_path = rpc_bench_path(fastblock_home, args.build_path_suffix)
    rpc_bench_conf = {}
    with open(conf_path, 'r', encoding='utf-8') as file:
        rpc_bench_conf = json.load(file)

    if args.ci:
        if args.net_device is None:
            print('--net-device is required for ci test')
            exit(1)

        prepare_soft_roce(args.net_device, args.rdma_device)
        rpc_bench_conf = update_rpc_conf(conf_path, rpc_bench_conf, args.node_count, args.start_port)

    app_nums = len(rpc_bench_conf['endpoints'])
    app_base_name = 'rpc_bench'
    app_core_counter = args.start_core
    cmds = []
    for i in range(app_nums):
        n_core = count_core_num(rpc_bench_conf['endpoints'], rpc_bench_conf['rpc_client_same_core'], i)
        cmds.append([bin_path, '-C', conf_path, '-I', f'{i + 1}', '-m', f'[{format_core_arg(n_core, app_core_counter)}]'])
        app_core_counter += n_core

    rc = asyncio.run(run_rpc_bench(cmds))
    exit(rc)
