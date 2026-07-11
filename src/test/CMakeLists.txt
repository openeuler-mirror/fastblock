find_package(absl REQUIRED)


# Option to enable/disable test compilation
option(BUILD_FASTBLOCK_TESTS "Build fastblock unit tests" OFF)

if(BUILD_FASTBLOCK_TESTS)

	# Unified test framework
	add_subdirectory(framework)

	# ============================================================================
	# Unit Tests - Organized by module
	# ============================================================================

	# ----------------------------------------------------------------------------
	# base module tests
	# ----------------------------------------------------------------------------
	add_executable(test_base
	  unit/base/test_base.cc
	)

	target_include_directories(test_base
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_base
	  PUBLIC
	    test_framework
	    base
	)

	# ----------------------------------------------------------------------------
	# client module tests
	# ----------------------------------------------------------------------------
	add_executable(test_client
	  unit/client/test_client.cc
	)

	target_include_directories(test_client
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_client
	  PUBLIC
	    test_framework
	    base
	)

	# ----------------------------------------------------------------------------
	# localstore module tests
	# ----------------------------------------------------------------------------
	add_executable(test_localstore
	  unit/localstore/test_localstore.cc
	)

	target_include_directories(test_localstore
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
	  PRIVATE ${FASTBLOCK_DEPS_INSTALL_DIR}/include)

	target_link_libraries(test_localstore
	  PUBLIC
	    test_framework
	    raft_msg
	)

	# ----------------------------------------------------------------------------
	# monclient module tests
	# ----------------------------------------------------------------------------
	add_executable(test_monclient
	  unit/monclient/test_monclient.cc
	)

	target_include_directories(test_monclient
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_monclient
	  PUBLIC
	    test_framework
	    base
	)

	# ----------------------------------------------------------------------------
	# osd module tests
	# ----------------------------------------------------------------------------
	add_executable(test_osd
	  unit/osd/test_osd.cc
	)

	target_include_directories(test_osd
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_osd
	  PUBLIC
	    test_framework
	    base
	    osd_msg
	)

	# ----------------------------------------------------------------------------
	# bdev module tests
	# ----------------------------------------------------------------------------
	add_executable(test_bdev
	  unit/bdev/test_bdev.cc
	)

	target_include_directories(test_bdev
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_bdev
	  PUBLIC
	    test_framework
	    base
	)

	# ----------------------------------------------------------------------------
	# raft module tests
	# ----------------------------------------------------------------------------
	add_executable(test_raft_state
	  unit/raft/test_raft_state.cc
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
	  unit/raft/test_raft_rpc.cc
	)

	target_include_directories(test_raft_rpc
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_raft_rpc
	  PUBLIC
	    test_framework
	    base
	)

	add_executable(test_raft_log_node
	  unit/raft/test_raft_log_node.cc
	)

	target_include_directories(test_raft_log_node
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_raft_log_node
	  PUBLIC
	    test_framework
	    base
	)

	# ----------------------------------------------------------------------------
	# rpc/msg module tests (communication layer)
	# ----------------------------------------------------------------------------
	add_executable(test_comm
	  unit/rpc/test_comm.cc
	)

	target_include_directories(test_comm
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_comm
	  PUBLIC
	    test_framework
	    base
	)

	# ----------------------------------------------------------------------------
	# utils module tests
	# ----------------------------------------------------------------------------
	add_executable(test_utils
	  unit/utils/test_utils.cc
	)

	target_include_directories(test_utils
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_utils
	  PUBLIC
	    test_framework
	    base
	)

	add_executable(test_utils_extra
	  unit/utils/test_utils_extra.cc
	)

	target_include_directories(test_utils_extra
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_utils_extra
	  PUBLIC
	    test_framework
	    base
	)

	# ----------------------------------------------------------------------------
	# framework tests (test framework itself)
	# ----------------------------------------------------------------------------
	add_executable(test_framework_macros
	  unit/framework/test_framework_macros.cc
	)

	target_include_directories(test_framework_macros
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(test_framework_macros
	  PUBLIC
	    test_framework
	    base
	)

	# ============================================================================
	# Stress Tests
	# ============================================================================
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