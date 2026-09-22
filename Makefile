NGINX_VERSION ?= 1.30.5

.PHONY: unit nginx smoke bench-worker-scaling test-image test-in-container srt-ingest srt-ingest-nginx ts-fixture source-switch api-switch api-graph failover hls hls-push hls-pull hls-ingest hls-profile file-source churn rtmp rtmp-hevc rtmps srt-output srt-crypto multi-worker soak fault netns srt-qualify bench-hls bench-hls-fanout bench-push-fanout bench-fanout-delay clean

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

hls-push:
	tests/integration/hls_push_nginx.sh

hls-pull:
	tests/integration/hls_pull_nginx.sh

hls-ingest:
	tests/integration/hls_ingest_nginx.sh

hls-profile:
	tests/integration/hls_profile_nginx.sh

churn:
	tests/integration/churn_nginx.sh

file-source:
	tests/integration/file_source_nginx.sh

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

netns:
	tests/integration/netns_nginx.sh

# The integration suite's own environment, and the one CI runs it in, so a
# failure on a runner can be reproduced here instead of guessed at.
#
#   make test-in-container                          everything
#   make test-in-container TEST_TARGETS="api-graph" one target
#
# The image is the Containerfile's test stage: the build stage plus ffmpeg.
#
# netns is deliberately not in the list.  It builds a topology of network
# namespaces, and doing that inside a container needs privileges and a
# fragile docker-in-docker-shaped setup for no gain - the namespace suite
# runs on the host, and on the CI runner, where namespaces are ordinary.
DOCKER ?= $(shell docker info >/dev/null 2>&1 && echo docker || echo "sudo -n docker")
TEST_IMAGE ?= nginx-media:test
# The base the image and its test stage are built on.  sid is the canary for
# the newest libraries; trixie is what ships.
BASE ?= debian:trixie
TEST_TARGETS ?= unit srt-ingest srt-ingest-nginx ts-fixture source-switch \
    api-switch api-graph failover hls hls-push hls-pull hls-ingest \
    hls-profile file-source churn rtmp rtmp-hevc rtmps srt-output \
    srt-crypto multi-worker soak fault

test-image:
	$(DOCKER) build -f Containerfile --target test --build-arg BASE=$(BASE) \
	    -t $(TEST_IMAGE) .

test-in-container: test-image
	$(DOCKER) run --rm $(TEST_IMAGE) make $(TEST_TARGETS)

srt-crypto:
	tests/integration/srt_crypto.sh

bench-hls:
	tests/bench/hls_serve.sh

bench-hls-fanout:
	tests/bench/hls_fanout.sh

bench-push-fanout:
	tests/bench/hls_push_fanout.sh

bench-fanout-delay:
	tests/bench/fanout_delay.sh

# What more workers buy, and what they do not: the API is per-worker state,
# and one program's fanout is owned by one worker.
bench-worker-scaling:
	tests/bench/worker_scaling.sh

srt-qualify:
	MEDIA_SRT_BACKEND=both $(MAKE) nginx
	tests/integration/srt_backend_qualify.sh

clean:
	$(MAKE) -C tests/unit clean
	rm -rf .build
