import argparse
import json
import os
import subprocess

import xmlrpc.client

from jinja2 import Template

PROCESSES_LOG_HOME = "/var/log/supervisor"

CI_SUP_CONF_J2 = """
; supervisor config file
[unix_http_server]
file=/tmp/supervisor.sock   ; the path to the socket file

[supervisord]
logfile=/var/log/supervisord.log ; main log file; default $CWD/supervisord.log
logfile_maxbytes=50MB        ; max main logfile bytes b4 rotation; default 50MB
logfile_backups=10           ; # of main logfile backups; 0 means none, default 10
loglevel=info                ; log level; default info; others: debug,warn,trace
pidfile=/tmp/supervisord.pid ; supervisord pidfile; default supervisord.pid
nodaemon=false               ; start in foreground if true; default false
minfds=1024                  ; min. avail startup file descriptors; default 1024
minprocs=200                 ; min. avail process descriptors;default 200

[inet_http_server]
port=127.0.0.1:{{ supervisor.port }}

[supervisorctl]
serverurl=unix:///tmp/supervisor.sock ; use a unix:// URL  for a unix socket

[rpcinterface:supervisor]
supervisor.rpcinterface_factory = supervisor.rpcinterface:make_main_rpcinterface

[group:{{ group.name }}]
programs={{ group.programs }}

{% for app in apps %}
[program:{{ app.name }}]
command={{ app.command }}
autostart={{ app.autostart | lower }}
autorestart={{ app.autorestart | lower }}
startsecs=5
startretries=3
redirect_stderr=true
stdout_logfile_maxbytes = 1GB
stdout_logfile={{ app.log_home }}/{{ app.name }}.log
stderr_logfile={{ app.log_home }}/{{ app.name }}_err.log
pre-start = /bin/bash -c ">{{ app.log_home }}/{{ app.name }}.log;>{{ app.log_home }}/{{ app.name }}_err.log"
{% endfor %}
"""
CI_SUP_CONF_PATH = "/etc/supervisor.con"


RPC_BENCH_SUP_J2 = """
[group:{{ group.name }}]
programs={{ group.programs }}

{% for app in apps %}
[program:{{ app.name }}]
command={{ app.command }}
autostart={{ app.autostart | lower }}
autorestart={{ app.autorestart | lower }}
startsecs=5
startretries=3
redirect_stderr=true
stdout_logfile_maxbytes = 1GB
stdout_logfile={{ app.log_home }}/{{ app.name }}.log
stderr_logfile={{ app.log_home }}/{{ app.name }}_err.log
pre-start = /bin/bash -c ">{{ app.log_home }}/{{ app.name }}.log;>{{ app.log_home }}/{{ app.name }}_err.log"
{% endfor %}
"""
RPC_BENCH_SUP_CONF_PATH = "/etc/supervisor/conf.d/rpc_bench.conf"


class supervisor_client:
    def __init__(self, url, conf_path):
        self.conf_path = conf_path
        print(f'connecting to supervisor xml rpc {url}')
        self.xml_client = xmlrpc.client.ServerProxy(url)
        print(f'connected to supervisor xml rpc {url}')

    def set_conf(self, conf_path):
        self.conf_path = conf_path

    def _check_supervisor_running(self):
        try:
            process = subprocess.Popen(['ps', 'aux'], stdout=subprocess.PIPE)
            output, _ = process.communicate()
            if b'supervisord' in output:
                print("supervisord is running")
                return True
            else:
                print("supervisord is not running")
                return False
        except Exception as e:
            print(f"check_supervisord_running() failed: {e}")
            return False

    def maybe_start_supervisor(self):
        try:
            if not self._check_supervisor_running():
                subprocess.run(['supervisord', '-c', self.conf_path])
                print(f"started supervisord with conf {self.conf_path}")
        except Exception as e:
            print(f"starting supervisord error: {e}")

    def reload_supervisor_config(self):
        try:
            self.xml_client.supervisor.reloadConfig()
            print("supervisor reloaded")
        except xmlrpc.client.Fault as fault:
            print(f"unknown xmlrpc reload error: {fault.faultString}")
        except Exception as e:
            print(f"unknown error: {str(e)}")


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


def format_group_name(base_name, build_suffix):
    if len(build_suffix) == 0:
        return base_name
    else:
        return f'{base_name}_{build_suffix}'


def format_app_name(base_name, build_suffix, index):
    if len(build_suffix) == 0:
        return f'{base_name}_{index + 1}'
    else:
        return f'{base_name}_{build_suffix}_{index + 1}'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="generate supervisord conf for rpc_bench")
    parser.add_argument("--log-flags", type=str, help="spdk app log flags, like '-L msg -L osd' ...", default='')
    parser.add_argument("--start-core", type=int, help="start of cpu core number, default is 0", default=0)
    parser.add_argument("--build-path-suffix", type=str, help="for debug test, may ignore it", default='')
    parser.add_argument("--ci", action='store_true', help="for ci test, may ignore it")
    parser.add_argument("--supervisor-xmlrpc-port", type=int, help="for ci test, set supervisor xml rpc port, may ignore it", default=5371)
    args = parser.parse_args()

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
    app_bin_path = bin_path
    app_conf_path = conf_path
    app_log_mod = args.log_flags
    app_core_counter = args.start_core

    print(f'app_nums: {app_nums}')

    apps = []
    for i in range(app_nums):
        n_core = count_core_num(rpc_bench_conf['endpoints'], i)
        rpc_bench_meta = {
            'name': format_app_name(app_base_name, args.build_path_suffix, i + 1),
            'command': f'{app_bin_path} -C {app_conf_path} -I {i + 1} -m [{format_core_arg(n_core, app_core_counter)}] {app_log_mod}',
            'autostart': False,
            'autorestart': False,
            'log_home': PROCESSES_LOG_HOME
        }
        apps.append(rpc_bench_meta)
        app_core_counter += n_core

    group_meta = {
        'name': format_group_name(app_base_name, args.build_path_suffix),
        'programs': ','.join([m['name'] for m in apps])
    }

    supervisor_meta = {'port': args.supervisor_xmlrpc_port}

    os.makedirs(PROCESSES_LOG_HOME, exist_ok=True)
    sup_rpc_cli = supervisor_client(f'http://localhost:{args.supervisor_xmlrpc_port}/RPC2', RPC_BENCH_SUP_CONF_PATH)
    if args.ci:
        template = Template(CI_SUP_CONF_J2)
        output = template.render(apps=apps, group=group_meta, supervisor=supervisor_meta)
        with open(CI_SUP_CONF_PATH, 'w') as f:
            f.write(output)
        sup_rpc_cli.set_conf(CI_SUP_CONF_PATH)
    else:
        template = Template(RPC_BENCH_SUP_J2)
        output = template.render(apps=apps, group=group_meta)
        with open(RPC_BENCH_SUP_CONF_PATH, 'w') as f:
            f.write(output)

    sup_rpc_cli.maybe_start_supervisor()
    sup_rpc_cli.reload_supervisor_config()
