NGINX_VERSION ?= 1.30.5

.PHONY: unit tsan nginx graph-conflict incarnation smoke bench-worker-scaling \
    bench-worker-topology bench-ingest-egress bench-ingest-egress-fanout \
    bench-capacity-curve bench-capacity-quality bench-burst-sizing test-image test-in-container \
    srt-ingest srt-ingest-nginx ts-fixture source-switch api-switch api-graph \
    failover hls hls-push hls-pull hls-ingest hls-profile file-source transform \
    stream-delete churn rtmp rtmp-hevc rtmps rtmp-workers srt-output srt-crypto \
    srt-worker-ports multi-worker soak fault netns srt-qualify bench-hls \
    bench-hls-fanout bench-push-fanout bench-fanout-delay clean

unit:
	$(MAKE) -C tests/unit test

tsan:
	$(MAKE) -C tests/unit tsan

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

transform:
	tests/integration/transform_nginx.sh

# Deleting a stream has to release the memory it held, including when a reader
# thread or an in-flight upload is still looking at it: the pools come back
# only after the last reader is closed, and never by making the delete wait on
# an origin.
stream-delete:
	tests/integration/stream_delete_nginx.sh

# A stream deleted and created again under the same name: the publisher whose
# session was routed to the deleted stream must not reach the new one.
incarnation:
	tests/integration/incarnation_nginx.sh

# Two workers mutating one stream at the same time converge on one state:
# revisions are one sequence for every worker, so a replica resolves the race
# by the higher number rather than by whichever operation reached it last.
graph-conflict:
	tests/integration/graph_conflict_nginx.sh

rtmp:
	tests/integration/rtmp_nginx.sh

rtmp-hevc:
	tests/integration/rtmp_hevc_nginx.sh

rtmps:
	tests/integration/rtmps_nginx.sh

rtmp-workers:
	tests/integration/rtmp_workers_nginx.sh

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
# Extra flags for the image build, empty for a local run.  CI sets
# DOCKER_CACHE_ARGS to the BuildKit GHA cache backend (--cache-from/--cache-to
# type=gha), which only exists where ACTIONS_RUNTIME_TOKEN does; passing those
# flags locally would fail, so they stay out of the default.
DOCKER_CACHE_ARGS ?=
TEST_TARGETS ?= unit srt-ingest srt-ingest-nginx ts-fixture source-switch \
    api-switch api-graph failover hls hls-push hls-pull hls-ingest \
    hls-profile file-source transform stream-delete incarnation graph-conflict churn \
    rtmp \
    rtmp-hevc rtmps rtmp-workers srt-output srt-crypto srt-worker-ports \
    srt-shared-port multi-worker soak fault

test-image:
	$(DOCKER) build -f Containerfile --target test --build-arg BASE=$(BASE) \
	    -t $(TEST_IMAGE) $(DOCKER_CACHE_ARGS) .

test-in-container: test-image
	$(DOCKER) run --rm $(TEST_IMAGE) make $(TEST_TARGETS)

srt-crypto:
	tests/integration/srt_crypto.sh

# One SRT ingest endpoint per worker: media_srt_listen may be given once per
# worker, worker i binds the i-th entry, and a publisher placed on a worker's
# endpoint is accepted by that worker instead of funnelling every publisher
# through worker 0.
srt-worker-ports:
	tests/integration/srt_worker_ports_nginx.sh

# One SRT ingest endpoint shared by every worker: media_srt_listen_shared
# makes each worker bind the same address on a socket it creates with
# SO_REUSEPORT, so the kernel spreads publishers across the workers instead of
# funnelling them through the worker that bound the port.  What that costs - a
# reload ends every live publisher's session - is measured here rather than
# predicted, and written up in docs/operations.md.
srt-shared-port:
	tests/integration/srt_shared_port_nginx.sh

bench-hls:
	tests/bench/hls_serve.sh

bench-hls-fanout:
	tests/bench/hls_fanout.sh

bench-push-fanout:
	tests/bench/hls_push_fanout.sh

bench-fanout-delay:
	tests/bench/fanout_delay.sh

# Sweep the real-media MPEG-TS backing allocation and report the first
# lossless capacity rather than treating bitrate as a sufficient proxy.
bench-burst-sizing:
	tests/bench/burst_sizing.sh

# What more workers buy, and what they do not: the API is per-worker state,
# and one program's fanout is owned by one worker.
bench-worker-scaling:
	tests/bench/worker_scaling.sh

# Deterministic four-worker owner/topology matrix with per-program pressure and
# route outcome counters, not arbitrary names whose hash distribution is luck.
bench-worker-topology:
	PHASES=topology tests/bench/ingest_egress_fanout.sh

# Ingest and egress scale differently: one program has one owner worker, so
# its fanout is bound by that worker, while more programs spread across more
# workers.
bench-ingest-egress:
	tests/bench/ingest_egress_scaling.sh

# Where ingest and egress each saturate under extreme fanout: publishers ramp
# onto one endpoint per worker (per-thread CPU attribution, and one endpoint
# with more workers bound to show the ceiling is the port and not the worker),
# HLS readers against the shared segment store versus live destinations fed by
# the owner, and the same publisher placed on its owner and routed from
# another worker so the escape hatch has a price.
bench-ingest-egress-fanout:
	tests/bench/ingest_egress_fanout.sh

# Fixed-worker capacity curve through 1000 SRT/RTMP destinations, with delivered
# bytes, shard/resource metrics, slow-reader isolation, saturated fanout, and
# sustained multi-program fairness.
bench-capacity-curve:
	PHASES=capacity tests/bench/ingest_egress_fanout.sh

# Set CAPACITY_FIXED_SRT_WORKERS and CAPACITY_FIXED_HLS_PUSH_WORKERS to pin
# either pool (1-16 SRT senders, 1-4 HLS uploaders); default is adaptive.
# Six calibrated 8M delivery-quality ladders: pure SRT, RTMP, HLS readers,
# pure HLS push, and both 95/5 RTMP/SRT and HLS-push/SRT mixes.
# Set CAPACITY_QUALITY_MIXES to a subset; default `all` runs every mix.
bench-capacity-quality:
	PHASES=capacity-quality-ladder tests/bench/ingest_egress_fanout.sh

srt-qualify:
	MEDIA_SRT_BACKEND=both $(MAKE) nginx
	tests/integration/srt_backend_qualify.sh

clean:
	$(MAKE) -C tests/unit clean
	rm -rf .build
