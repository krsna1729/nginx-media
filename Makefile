NGINX_VERSION ?= 1.30.5

.PHONY: unit nginx smoke clean

unit:
	$(MAKE) -C tests/unit test

nginx:
	NGINX_VERSION=$(NGINX_VERSION) scripts/build-nginx.sh

smoke:
	NGINX_VERSION=$(NGINX_VERSION) tests/integration/smoke.sh

clean:
	$(MAKE) -C tests/unit clean
	rm -rf .build
