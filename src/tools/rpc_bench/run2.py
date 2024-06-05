#!/bin/python

import argparse
import asyncio
import json
import os
import subprocess

def prepare_cmd_args():
    parser = argparse.ArgumentParser(description="generate supervisord conf for rpc_bench")
    parser.add_argument("--log-flags", type=str, help="spdk app log flags, like '-L msg -L osd' ...", default='')
    parser.add_argument("--start-core", type=int, help="start of cpu core number, default is 0", default=0)
    parser.add_argument("--build-path-suffix", type=str, help="for debug test, may ignore it", default=None)
    args = parser.parse_args()

    return args


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

    app_nums = len(rpc_bench_conf['endpoints'])
    app_base_name = 'rpc_bench'
    app_core_counter = args.start_core
    cmds = []
    for i in range(app_nums):
        n_core = count_core_num(rpc_bench_conf['endpoints'], rpc_bench_conf['rpc_client_same_core'], i)
        # cmds.append(f'{bin_path} -C {conf_path} -I {i + 1} -m [{format_core_arg(n_core, app_core_counter)}]')
        cmds.append([bin_path, '-C', conf_path, '-I', f'{i + 1}', '-m', f'[{format_core_arg(n_core, app_core_counter)}]'])
        app_core_counter += n_core

    asyncio.run(run_rpc_bench(cmds))
