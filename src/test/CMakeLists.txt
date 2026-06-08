find_package(absl REQUIRED)


# Option to enable/disable test compilation
option(BUILD_FASTBLOCK_TESTS "Build fastblock unit tests" OFF)

if(BUILD_FASTBLOCK_TESTS)
# Legacy test executables (preserved for backward compatibility)

	# Unified test framework
	add_subdirectory(framework)
	
	add_executable(raft_membership
	  osd_client.cc
	  raft_membership.cc
	)

	target_include_directories (raft_membership
	  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

	target_link_libraries(raft_membership
	  PUBLIC
	    base
	    osd_msg
	    msg
	)

	add_executable(snapshot_write_stress
	  snapshot_write_stress.cc
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
	  osd_client.cc
	  snapshot_pg_stress.cc
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
