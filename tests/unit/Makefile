CC      ?= cc
CFLAGS  ?= -O1 -g
SAN     ?= -fsanitize=address,undefined
WARN     = -Wall -Wextra -Werror -std=c11

INCLUDES = -I../../src/core -I../../src/codec -I../../src/srt -I../../src/mpegts -I../../src/hls -I../../src/record -I../../src/rtmp -Ishim -I.
DEFS     = -DNGX_MEDIA_UNIT_TEST

CORE := ../../src/core/ngx_media_buffer.c \
        ../../src/core/ngx_media_frame.c \
        ../../src/core/ngx_media_feed.c \
        ../../src/core/ngx_media_track.c \
        ../../src/core/ngx_media_timeline.c \
        ../../src/core/ngx_media_source.c \
        ../../src/core/ngx_media_stream.c \
        ../../src/core/ngx_media_policy.c \
        ../../src/core/ngx_media_health.c \
        ../../src/core/ngx_media_compat.c \
        ../../src/core/ngx_media_selector.c \
        ../../src/codec/ngx_media_nal.c \
        ../../src/codec/ngx_media_aac.c \
        ../../src/mpegts/ngx_media_ts_crc.c \
        ../../src/mpegts/ngx_media_ts_demux.c \
        ../../src/mpegts/ngx_media_ts_mux.c \
        ../../src/mpegts/ngx_media_ts_ingest.c \
        ../../src/srt/ngx_media_srt_streamid.c \
        ../../src/hls/ngx_media_hls_segmenter.c \
        ../../src/record/ngx_media_record.c \
        ../../src/rtmp/ngx_media_rtmp_wire.c

SHIM := shim/ngx_shim.c

TESTS := test_buffer test_frame test_feed test_track test_timeline \
         test_stream test_health test_compat test_selector test_codec \
         test_srt_streamid test_ts_ingest test_ts_demux test_ts_mux test_hls test_record \
         test_rtmp_wire
BINS  := $(addprefix build/,$(TESTS))

.PHONY: all test clean

all: $(BINS)

build/%: %.c ngx_media_test.h $(CORE) $(SHIM)
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(SAN) -o $@ $< $(CORE) $(SHIM) -lpthread

test: $(BINS)
	@fail=0; for t in $(TESTS); do ./build/$$t || fail=1; done; exit $$fail

clean:
	rm -rf build
