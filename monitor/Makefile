.PHONY: all clean install


all:
	go build -o fastblock-mon monitor.go
	go build -o fastblock-client fbclient.go
clean:
	rm -f fastblock-mon fastblock-client
install:
	install -m 0755 fastblock-mon /usr/local/bin
	install -m 0755 fastblock-client /usr/local/bin
	
