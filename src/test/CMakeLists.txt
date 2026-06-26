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
	    osd_msg
	    raft_msg
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