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
    parser.add_argument("--build-path-suffix", type=str, help="for debug test, may ignore it", default='')
    parser.add_argument("--ci", action='store_true', help="for ci test, may ignore it")
    parser.add_argument("--supervisor-xmlrpc-port", type=int, help="for ci test, set supervisor xml rpc port, may ignore it", default=5371)
    args = parser.parse_args()

    return args


def fastblock_home_path():
    try:
        git_root = subprocess.check_output(["git", "rev-parse", "--show-toplevel"]).decode("utf-8").strip()
        return git_root
    except subprocess.CalledProcessError:
        return None


def rpc_bench_path(fastblock_home, build_suffix):
    bin_path = os.path.join(fastblock_home, f'build.{build_suffix}', 'src', 'tools', 'rpc_bench', 'rpc_bench')
    conf_path = os.path.join(fastblock_home, 'src', 'tools', 'rpc_bench', 'rpc_bench.json')
    return bin_path, conf_path


def count_core_num(eps, index):
    count = len(eps[index]['list']) # server core count
    for i in range(len(eps)):
        if i + 1 == eps[index]['index']:
            continue
        count += len(eps[i]['list'])

    print(f'rpc_bench_{index + 1} needs core size is {count}')
    return count


def format_core_arg(n_core, start):
    cores = [f'{i + start}' for i in range(n_core)]
    return ','.join(cores)


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
    apps = []
    for i in range(app_nums):
        n_core = count_core_num(rpc_bench_conf['endpoints'], i)
        apps.append(f'{bin_path} -C {conf_path} -I {i + 1} -m [{format_core_arg(n_core, app_core_counter)}]')
        app_core_counter += n_core
        print(apps[-1])
