find_package(absl REQUIRED)


# Option to enable/disable test compilation
option(BUILD_FASTBLOCK_TESTS "Build fastblock unit tests" OFF)

if(BUILD_FASTBLOCK_TESTS)

	# Unified test framework
	add_subdirectory(framework)

	# Unit tests
	add_executable(test_raft_state
	  unit/test_raft_state.cc
	)

	target_include_directories(test_raft_state
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_raft_state
	  PUBLIC
	    test_framework
	    base
	    raft_msg
	    raft
	    monclient
	    msg
	    localstore
	    osd_msg
	)

	add_executable(test_raft_rpc
	  unit/test_raft_rpc.cc
	)

	target_include_directories(test_raft_rpc
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_raft_rpc
	  PUBLIC
	    test_framework
	    base
	)

	add_executable(test_framework_macros
	  unit/test_framework_macros.cc
	)

	target_include_directories(test_framework_macros
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_framework_macros
	  PUBLIC
	    test_framework
	    base
	)

	add_executable(test_raft_log_node
	  unit/test_raft_log_node.cc
	)

	target_include_directories(test_raft_log_node
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_raft_log_node
	  PUBLIC
	    test_framework
	    base
	)

	add_executable(test_utils
	  unit/test_utils.cc
	)

	target_include_directories(test_utils
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_utils
	  PUBLIC
	    test_framework
	    base
	)

	# Extra utils tests: covers utils contracts NOT in test_utils.cc —
	# err_num error codes, utils.h align_up/align_down (bitmask version,
	# power-of-2 only), constants (port range, blobstore cluster size),
	# osd_info_t / pg_info_type / core_shard_map, overload helper,
	# duration_map percentile arithmetic. Headers are mirrored locally
	# (don't link any production code) so the test binary stays free of
	# spdk_env / pthread globals.
	add_executable(test_utils_extra
	  unit/test_utils_extra.cc
	)

	target_include_directories(test_utils_extra
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_utils_extra
	  PUBLIC
	    test_framework
	    base
	)


	add_executable(test_localstore
	  unit/test_localstore.cc
	)

	target_include_directories(test_localstore
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_localstore
	  PUBLIC
	    test_framework
	    base
	    raft_msg
	)
	add_executable(test_osd
	  unit/test_osd.cc
	)

	target_include_directories(test_osd
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_osd
	  PUBLIC
	    test_framework
	    base
	    osd_msg
	)

	add_executable(test_base
	  unit/test_base.cc
	)

	target_include_directories(test_base
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_base
	  PUBLIC
	    test_framework
	    base
	)

	# Communication-link unit tests: msg / rpc / monclient combined.
	# Does NOT link the real msg/rpc/monclient libs (they pull in rdmacm /
	# ibverbs / spdk env that may be absent in CI). Tests mirror the data
	# contracts; production headers stay the source of truth.
	add_executable(test_comm
	  unit/test_comm.cc
	)

	target_include_directories(test_comm
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_comm
	  PUBLIC
	    test_framework
	    base
	)

	# Client unit tests: routing, addressing, object splitting.
	# Part 1 of the client test suite covering the arithmetic contracts
	# invoked on every IO path. Mirrors contracts locally to avoid
	# pulling rdmacm/ibverbs/spdk_env into the test binary.
	add_executable(test_client
	  unit/test_client.cc
	)

	target_include_directories(test_client
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_client
	  PUBLIC
	    test_framework
	    base
	)

	# Stress tests
	add_executable(raft_membership
	  framework/osd_client.cc
	  stress/raft_membership.cc
	)

	target_include_directories(raft_membership
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(raft_membership
	  PUBLIC
	    base
	    osd_msg
	    msg
	)

	add_executable(snapshot_write_stress
	  stress/snapshot_write_stress.cc
	)

	target_include_directories(snapshot_write_stress
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(snapshot_write_stress
	  PUBLIC
	    base
	    osd_msg
	    msg
	    monclient
	    fbclient
	)

	add_executable(snapshot_pg_stress
	  framework/osd_client.cc
	  stress/snapshot_pg_stress.cc
	)

	target_include_directories(snapshot_pg_stress
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(snapshot_pg_stress
	  PUBLIC
	    base
	    osd_msg
	    msg
	)
endif()