CC      ?= cc
CFLAGS  ?= -O1 -g
SAN     ?= -fsanitize=address,undefined
WARN     = -Wall -Wextra -Werror -std=c11

INCLUDES = -I../../src/core -I../../src/codec -I../../src/srt -I../../src/mpegts -I../../src/hls -I../../src/record -I../../src/rtmp -Ishim -I.
DEFS     = -DNGX_MEDIA_UNIT_TEST

CORE := ../../src/core/ngx_media_buffer.c \
        ../../src/core/ngx_media_executor.c \
        ../../src/core/ngx_media_transform.c \
        ../../src/core/ngx_media_frame.c \
        ../../src/core/ngx_media_feed.c \
        ../../src/core/ngx_media_track.c \
        ../../src/core/ngx_media_timeline.c \
        ../../src/core/ngx_media_source.c \
        ../../src/core/ngx_media_destination.c \
        ../../src/core/ngx_media_egress_manager.c \
        ../../src/core/ngx_media_stream.c \
        ../../src/core/ngx_media_policy.c \
        ../../src/core/ngx_media_health.c \
        ../../src/core/ngx_media_compat.c \
        ../../src/core/ngx_media_selector.c \
        ../../src/core/ngx_media_owner.c \
        ../../src/core/ngx_media_tombstone.c \
        ../../src/core/ngx_media_ipc.c \
        ../../src/codec/ngx_media_nal.c \
        ../../src/codec/ngx_media_aac.c \
        ../../src/mpegts/ngx_media_ts_crc.c \
        ../../src/mpegts/ngx_media_ts_demux.c \
        ../../src/mpegts/ngx_media_ts_mux.c \
        ../../src/mpegts/ngx_media_ts_ingest.c \
        ../../src/srt/ngx_media_srt_streamid.c \
        ../../src/srt/ngx_media_srt_transport.c \
        ../../src/srt/ngx_media_srt_udp.c \
        ../../src/srt/ngx_media_srt_output_queue.c \
        ../../src/srt/ngx_media_srt_output.c \
        ../../src/core/ngx_media_hls_profile.c \
        ../../src/core/ngx_media_hls_push.c \
        ../../src/hls/ngx_media_hls_segmenter.c \
        ../../src/record/ngx_media_record.c \
        ../../src/rtmp/ngx_media_rtmp_wire.c \
        ../../src/rtmp/ngx_media_rtmp_adapter.c

SHIM := shim/ngx_shim.c

TESTS := test_buffer test_destination test_egress_manager test_frame test_feed test_track test_timeline \
         test_rtmp_wire test_rtmp_flv test_srt_output test_owner test_ipc test_lifecycle test_fuzz test_bounds test_srt_udp test_security test_tombstone test_transform test_executor
BINS  := $(addprefix build/,$(TESTS))

.PHONY: all test tsan clean

all: $(BINS)

build/%: %.c ngx_media_test.h $(CORE) $(SHIM)
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(SAN) -o $@ $< $(CORE) $(SHIM) -lpthread

test: $(BINS)
	@fail=0; for t in $(TESTS); do ./build/$$t || fail=1; done; exit $$fail

# The suites that drive threads, rebuilt under ThreadSanitizer.  They live in
# a separate directory so a normal build never mixes instrumentation.
TSAN_TESTS := test_record test_lifecycle test_feed test_stream test_owner \
              test_srt_output test_hls_ingest
TSAN_BINS  := $(addprefix build/tsan/,$(TSAN_TESTS))

tsan: $(TSAN_BINS)
	@fail=0; for t in $(TSAN_TESTS); do ./build/tsan/$$t || fail=1; done; exit $$fail

build/tsan/%: %.c ngx_media_test.h $(CORE) $(SHIM)
	@mkdir -p build/tsan
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) -fsanitize=thread -o $@ $< $(CORE) $(SHIM) -lpthread

# The HLS ingest reader is host-side code - it reads a directory, demuxes and
# sleeps - and no other suite links it, so it gets its own rule instead of a
# place in CORE.  Without it the queue the reader thread fills and the worker
# drains is the one threaded path under a sanitizer that is never run.
build/tsan/test_hls_ingest: test_hls_ingest.c ngx_media_test.h $(CORE) $(SHIM)
	@mkdir -p build/tsan
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) -fsanitize=thread -o $@ $< \
	    ../../src/core/ngx_media_hls_ingest.c $(CORE) $(SHIM) -lpthread

clean:
	rm -rf build
