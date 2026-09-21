NGINX_VERSION ?= 1.30.5

.PHONY: unit nginx smoke srt-ingest srt-ingest-nginx ts-fixture source-switch api-switch api-graph failover hls rtmp rtmp-hevc rtmps srt-output srt-crypto multi-worker soak fault srt-qualify bench-hls clean

unit:
	$(MAKE) -C tests/unit test

nginx:
	NGINX_VERSION=$(NGINX_VERSION) scripts/build-nginx.sh

smoke:
	NGINX_VERSION=$(NGINX_VERSION) tests/integration/smoke.sh

srt-ingest:
	tests/integration/srt_ingest.sh

srt-ingest-nginx:
	tests/integration/srt_ingest_nginx.sh

ts-fixture:
	tests/integration/ts_fixture.sh

source-switch:
	tests/integration/source_switch.sh

api-switch:
	tests/integration/api_switch_nginx.sh

api-graph:
	tests/integration/api_graph_nginx.sh

failover:
	tests/integration/failover_nginx.sh

hls:
	tests/integration/hls_nginx.sh

rtmp:
	tests/integration/rtmp_nginx.sh

rtmp-hevc:
	tests/integration/rtmp_hevc_nginx.sh

rtmps:
	tests/integration/rtmps_nginx.sh

srt-output:
	tests/integration/srt_output_nginx.sh

multi-worker:
	tests/integration/multi_worker_nginx.sh

soak:
	tests/integration/soak_nginx.sh

fault:
	tests/integration/fault_nginx.sh

srt-crypto:
	tests/integration/srt_crypto.sh

bench-hls:
	tests/bench/hls_serve.sh

srt-qualify:
	MEDIA_SRT_BACKEND=both $(MAKE) nginx
	tests/integration/srt_backend_qualify.sh

clean:
	$(MAKE) -C tests/unit clean
	rm -rf .build
