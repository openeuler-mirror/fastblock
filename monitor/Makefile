# Copyright (c) 2023-2024 ChinaUnicom. Licensed under Mulan PSL v2.

.PHONY: all clean install

# Internal dependency directories for monitor
MONITOR_DEPS_DIRS := config election etcdapi leader log msg osd
# Go source files in all monitor dependency directories
MONITOR_DEPS_FILES := $(foreach dir,$(MONITOR_DEPS_DIRS),$(wildcard $(dir)/*.go))
# Add monitor main entry point
MONITOR_FILES := cmd/fastblock-mon/main.go $(MONITOR_DEPS_FILES)

# Internal dependency directories for fbclient
FBCLIENT_DEPS_DIRS := config msg utils
# Go source files in all fbclient dependency directories
FBCLIENT_DEPS_FILES := $(foreach dir,$(FBCLIENT_DEPS_DIRS),$(wildcard $(dir)/*.go))
# Add fbclient main entry point
FBCLIENT_FILES := cmd/fastblock-client/main.go $(FBCLIENT_DEPS_FILES)

all: fastblock-mon fastblock-client

fastblock-mon: $(MONITOR_FILES)
	go build -o fastblock-mon ./cmd/fastblock-mon

fastblock-client: $(FBCLIENT_FILES)
	go build -o fastblock-client ./cmd/fastblock-client
clean:
	rm -f fastblock-mon fastblock-client

install:
	install -m 0755 fastblock-mon /usr/local/bin
	install -m 0755 fastblock-client /usr/local/bin
	
