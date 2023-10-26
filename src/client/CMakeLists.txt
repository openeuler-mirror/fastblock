compile_library(
  NAME fbclient
  SRCS
    libfblock.cc
    fb_client.cc

  DEPS
    spdk::spdk
    monclient
    raft_msg
    msg
    base
    osd_msg
  COPTS
    -Wno-error=format=
    )
