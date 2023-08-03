#include "base/core_sharded.h"
#include "base/shard_service.h"

#include <spdk/stdinc.h>
#include <spdk/env.h>
#include <spdk/event.h>
#include <spdk/log.h>

#include <tuple>

class local_service {
public:
    local_service() {
        SPDK_NOTICELOG("local_service crot on shard:%u \n", 
            core_sharded::get_core_sharded().this_shard_id());
    }

    void print_value() {
        SPDK_NOTICELOG("value:%d on shard:%u\n", 
            value, core_sharded::get_core_sharded().this_shard_id());
    }
    void set_value(int in) { 
        SPDK_NOTICELOG("this shard id:%u lcore:%u.\n", 
              core_sharded::get_core_sharded().this_shard_id(),
              spdk_env_get_current_core());
        value = in; 
    }

private:
    int value;
};

void demo_shard_start(void*) {
    uint32_t i;
    SPDK_ENV_FOREACH_CORE(i) {
        SPDK_NOTICELOG("lcore:%u \n", i);
    }

    sharded<local_service> sd;
    sd.start();

    // 运行时至少指定4个核 !!!
    core_sharded::get_core_sharded().invoke_on(0, [&sd]{ sd.local().set_value(0); });
    core_sharded::get_core_sharded().invoke_on(1, [&sd]{ sd.local().set_value(1); });
    // core_sharded::get_core_sharded().invoke_on(2, [&sd]{ sd.local().set_value(2); });

    core_sharded::get_core_sharded().invoke_on(0, [&sd]{ sd.local().print_value(); });
    core_sharded::get_core_sharded().invoke_on(1, [&sd]{ sd.local().print_value(); });
    // core_sharded::get_core_sharded().invoke_on(2, [&sd]{ sd.local().print_value(); });
    // core_sharded::get_core_sharded().invoke_on(3, [&sd]{ sd.local().print_value(); });
}

int
main(int argc, char **argv)
{
  struct spdk_app_opts opts = {};
  int rc = 0;

  spdk_app_opts_init(&opts, sizeof(opts));
  opts.name = "hello_blob";
  if ((rc = spdk_app_parse_args(argc, argv, &opts, NULL, NULL, NULL, NULL)) !=
      SPDK_APP_PARSE_ARGS_SUCCESS) {
    exit(rc);
  }

  rc = spdk_app_start(&opts, demo_shard_start, NULL);

  spdk_app_fini();
  return rc;
}