EXECUTE_PROCESS(
        COMMAND go build -o fastblock-mon monitor.go
        WORKING_DIRECTORY monitor
)

install(FILES fastblock-mon DESTINATION bin PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
                    GROUP_EXECUTE GROUP_READ WORLD_READ WORLD_EXECUTE)

EXECUTE_PROCESS(
        COMMAND go build -o fastblock-fakeosd fakeosd.go
        WORKING_DIRECTORY monitor
)

install(FILES fastblock-fakeosd DESTINATION bin PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
                    GROUP_EXECUTE GROUP_READ WORLD_READ WORLD_EXECUTE)

EXECUTE_PROCESS(
        COMMAND go build -o fastblock-client fbclient.go
        WORKING_DIRECTORY monitor
)

install(FILES fastblock-client DESTINATION bin PERMISSIONS OWNER_EXECUTE OWNER_WRITE OWNER_READ
                    GROUP_EXECUTE GROUP_READ WORLD_READ WORLD_EXECUTE)
