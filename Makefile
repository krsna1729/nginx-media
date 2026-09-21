NGINX_VERSION ?= 1.30.5

.PHONY: unit nginx smoke srt-ingest clean

unit:
	$(MAKE) -C tests/unit test

nginx:
	NGINX_VERSION=$(NGINX_VERSION) scripts/build-nginx.sh

smoke:
	NGINX_VERSION=$(NGINX_VERSION) tests/integration/smoke.sh

srt-ingest:
	tests/integration/srt_ingest.sh

clean:
	$(MAKE) -C tests/unit clean
	rm -rf .build
