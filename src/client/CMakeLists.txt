compile_library(
  NAME
    client
  ENABLE_INSTALL 1
  SHARED 1
  SRCS
    libfblock.cc
    libfblock_c.cc
    fb_client.cc
  DEPS
    spdk::env_dpdk
    spdk_log
    # spdk_thread
    monclient
    raft_msg
    msg
    base
    osd_msg
  COPTS
    -Wno-error=format=
)
