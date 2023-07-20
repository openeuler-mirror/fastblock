find_package(spdk REQUIRED)
find_package(absl REQUIRED)

add_subdirectory(rpc)
add_subdirectory(osd)
add_subdirectory(raft)
add_subdirectory(storage)
add_subdirectory(localstore)
