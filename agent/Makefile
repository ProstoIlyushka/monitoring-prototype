# Makefile для сборки агента и deb-пакета

.PHONY: all clean build-agent build-deb

all: build-agent build-deb

build-agent:
	g++ -o monitoring-agent agent.cpp -lcurl -std=c++17 -O2 -static-libstdc++

build-deb: build-agent
	# Создаём структуру deb-пакета
	mkdir -p deb/usr/local/bin
	mkdir -p deb/etc/systemd/system
	mkdir -p deb/etc/monitoring-agent
	cp monitoring-agent deb/usr/local/bin/
	cp monitoring-agent.service deb/etc/systemd/system/
	chmod 755 deb/usr/local/bin/monitoring-agent
	# Собираем пакет
	dpkg-deb --build deb monitoring-agent.deb

clean:
	rm -f monitoring-agent
	rm -rf deb/usr
	rm -rf deb/etc
	rm -f monitoring-agent.deb