compile_library(
  NAME fb_client
  SRCS
    libfblock.cc
    fb_client.cc

  DEPS
    spdk::spdk
    mon
    raft_msg
    msg
    base
    osd_msg
  COPTS
    -Wno-error=format=
    )
