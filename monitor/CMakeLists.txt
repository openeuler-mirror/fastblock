EXECUTE_PROCESS(
        COMMAND go build -o fastblock-mon monitor.go
        WORKING_DIRECTORY monitor
)

EXECUTE_PROCESS(
        COMMAND go build -o fastblock-fakeosd fakeosd.go
        WORKING_DIRECTORY monitor
)

EXECUTE_PROCESS(
        COMMAND go build -o fastblock-client fbclient.go
        WORKING_DIRECTORY monitor
)
