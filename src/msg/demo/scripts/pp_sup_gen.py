import argparse
import json

from jinja2 import Environment, FileSystemLoader

def count_core_num(eps, index):
    count = len(eps[index]['list']) # server core count
    for i in range(len(eps)):
        if i + 1 == eps[index]['index']:
            continue
        count += len(eps[i]['list'])

    print(f'ping_pong_{index + 1} needs core size is {count}')
    return count


def format_core_arg(n_core, start):
    cores = [f'{i + start}' for i in range(n_core)]
    return ','.join(cores)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="generate supervisord conf for ping_pong")
    parser.add_argument("conf", type=str, help="ping_pong json conf abs path")
    parser.add_argument("bin_home", type=str, help="ping_pong binary abs path")
    args = parser.parse_args()
    ping_pong_conf = {}
    with open(args.conf, 'r', encoding='utf-8') as file:
        ping_pong_conf = json.load(file)

    app_nums = len(ping_pong_conf['endpoints'])
    app_base_name = 'ping_pong'
    env = 'debug'
    app_bin_path = f'{args.bin_home}'
    app_conf_path = f'{args.conf}'
    app_log_mod = '-L ping_pong'
    app_core_counter = 0

    print(f'app_nums: {app_nums}')

    apps = []
    for i in range(app_nums):
        n_core = count_core_num(ping_pong_conf['endpoints'], i)
        ping_pong_meta = {
            'name': f'{app_base_name}_{env}_{i + 1}',
            'command': f'{app_bin_path} -C {app_conf_path} -I {i + 1} -m [{format_core_arg(n_core, app_core_counter)}] {app_log_mod}',
            'autostart': False,
            'autorestart': False,
            'log_home': f'{app_base_name}'
        }
        apps.append(ping_pong_meta)
        app_core_counter += n_core

    group_meta = {
        'name': f'{app_base_name}_{env}',
        'programs': ','.join([m['name'] for m in apps])
    }

    env = Environment(loader=FileSystemLoader('./'))
    template = env.get_template('supervisord.j2')
    output = template.render(apps=apps, group=group_meta)
    with open('/etc/supervisor/conf.d/ping_pong.conf', 'w') as f:
        f.write(output)
