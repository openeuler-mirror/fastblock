.PHONY: all clean install

# monitor.go 的内部依赖目录
MONITOR_DEPS_DIRS := config election etcdapi leader log msg osd
# 查找所有 monitor 依赖目录中的 Go 文件
MONITOR_DEPS_FILES := $(foreach dir,$(MONITOR_DEPS_DIRS),$(wildcard $(dir)/*.go))
# 添加 monitor.go 本身
MONITOR_FILES := monitor.go $(MONITOR_DEPS_FILES)

# fbclient.go 的内部依赖目录
FBCLIENT_DEPS_DIRS := config msg utils
# 查找所有 fbclient 依赖目录中的 Go 文件
FBCLIENT_DEPS_FILES := $(foreach dir,$(FBCLIENT_DEPS_DIRS),$(wildcard $(dir)/*.go))
# 添加 fbclient.go 本身
FBCLIENT_FILES := fbclient.go $(FBCLIENT_DEPS_FILES)

all: fastblock-mon fastblock-client

fastblock-mon: $(MONITOR_FILES)
	go build -o fastblock-mon monitor.go

fastblock-client: $(FBCLIENT_FILES)
	go build -o fastblock-client fbclient.go

clean:
	rm -f fastblock-mon fastblock-client

install:
	install -m 0755 fastblock-mon /usr/local/bin
	install -m 0755 fastblock-client /usr/local/bin
	
