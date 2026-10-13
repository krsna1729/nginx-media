CC      ?= cc
CFLAGS  ?= -O1 -g
SAN     ?= -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
WARN     = -Wall -Wextra -Werror -std=c2x

INCLUDES = -I../../src/core -I../../src/codec -I../../src/srt -I../../src/mpegts -I../../src/hls -I../../src/record -I../../src/rtmp -Ishim -I.
DEFS     = -DNGX_MEDIA_UNIT_TEST

CORE := ../../src/core/ngx_media_key.c \
        ../../src/core/ngx_media_buffer.c \
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
        ../../src/core/ngx_media_hls_playlist.c \
        ../../src/hls/ngx_media_hls_segmenter.c \
        ../../src/record/ngx_media_record.c \
        ../../src/rtmp/ngx_media_rtmp_wire.c \
        ../../src/rtmp/ngx_media_rtmp_adapter.c

SHIM := shim/ngx_shim.c
HLS_INGEST := ../../src/core/ngx_media_hls_ingest.c
HTTP := ../../src/core/ngx_media_http.c ../../src/core/ngx_media_hls_pull.c
HTTP_CFLAGS := $(shell pkg-config --cflags libcares)
HTTP_LIBS := -Wl,--wrap=ares_init_options -lssl -lcrypto $(shell pkg-config --libs libcares)
LDLIBS ?= -lpthread -lcrypto
PYTHON ?= python3
COVERAGE_CC ?= gcc
COVERAGE_CFLAGS ?= -O0 -g
COVERAGE_GCOV ?= gcov

# Every standalone suite belongs to the normal sanitizer run.
TESTS := $(basename $(wildcard test_*.c))
TSAN_TESTS := test_record test_lifecycle test_feed test_stream test_owner \
              test_srt_output test_hls_ingest test_http
BINS := $(addprefix build/,$(TESTS))
TSAN_BINS := $(addprefix build/tsan/,$(TSAN_TESTS))
NATIVE_BINS := $(addprefix build/native/,$(TESTS))
COVERAGE_BINS := $(addprefix build/coverage/,$(TESTS))

.PHONY: all test tsan native coverage clean FORCE
.DEFAULT_GOAL := all
.SECONDARY:

# The configuration stamp changes only when effective flags, build recipes or
# the resolved compiler/version change. Depfiles track the headers actually used.
# Common and real-HTTP objects never share a directory: their shim definitions
# differ. Normal, TSan, native and coverage also have independent stamps/objects.
define build_mode
$(1)_COMMON := $$(patsubst ../../%.c,build/obj/$(1)/common/%.o,$$(CORE)) build/obj/$(1)/common/$$(SHIM:.c=.o)
$(1)_HTTP := $$(patsubst ../../%.c,build/obj/$(1)/http/%.o,$$(CORE) $$(HTTP)) build/obj/$(1)/http/$$(SHIM:.c=.o)

build/obj/$(1)/.config: FORCE
	@$$(PYTHON) build_tools.py config --compiler "$(4)" $$@ -- \
	    $(5) $$(CPPFLAGS) $$(WARN) $$(DEFS) $$(INCLUDES) $(6) \
	    $$(HTTP_CFLAGS) -DNGX_MEDIA_TEST_REAL_HTTP $$(LDFLAGS) $$(LDLIBS) $$(HTTP_LIBS)

build/obj/$(1)/common/src/%.o: ../../src/%.c build/obj/$(1)/.config
	$$(PYTHON) build_tools.py compile $$@.command.json -- $(4) \
	    $(5) $$(CPPFLAGS) $$(WARN) $$(DEFS) $$(INCLUDES) $(6) \
	    -MMD -MP -MF $$(@:.o=.d) -c $$< -o $$@

build/obj/$(1)/common/shim/%.o: shim/%.c build/obj/$(1)/.config
	$$(PYTHON) build_tools.py compile $$@.command.json -- $(4) \
	    $(5) $$(CPPFLAGS) $$(WARN) $$(DEFS) $$(INCLUDES) $(6) \
	    -MMD -MP -MF $$(@:.o=.d) -c $$< -o $$@

build/obj/$(1)/common/%.o: %.c build/obj/$(1)/.config
	$$(PYTHON) build_tools.py compile $$@.command.json -- $(4) \
	    $(5) $$(CPPFLAGS) $$(WARN) $$(DEFS) $$(INCLUDES) $(6) \
	    -MMD -MP -MF $$(@:.o=.d) -c $$< -o $$@

build/obj/$(1)/http/src/%.o: ../../src/%.c build/obj/$(1)/.config
	$$(PYTHON) build_tools.py compile $$@.command.json -- $(4) \
	    $(5) $$(CPPFLAGS) $$(WARN) $$(DEFS) -DNGX_MEDIA_TEST_REAL_HTTP \
	    $$(INCLUDES) $$(HTTP_CFLAGS) $(6) -MMD -MP -MF $$(@:.o=.d) -c $$< -o $$@

build/obj/$(1)/http/shim/%.o: shim/%.c build/obj/$(1)/.config
	$$(PYTHON) build_tools.py compile $$@.command.json -- $(4) \
	    $(5) $$(CPPFLAGS) $$(WARN) $$(DEFS) -DNGX_MEDIA_TEST_REAL_HTTP \
	    $$(INCLUDES) $$(HTTP_CFLAGS) $(6) -MMD -MP -MF $$(@:.o=.d) -c $$< -o $$@

build/obj/$(1)/http/%.o: %.c build/obj/$(1)/.config
	$$(PYTHON) build_tools.py compile $$@.command.json -- $(4) \
	    $(5) $$(CPPFLAGS) $$(WARN) $$(DEFS) -DNGX_MEDIA_TEST_REAL_HTTP \
	    $$(INCLUDES) $$(HTTP_CFLAGS) $(6) -MMD -MP -MF $$(@:.o=.d) -c $$< -o $$@

$$(addprefix $(2)/,$$(filter-out test_http test_hls_ingest,$(3))): $(2)/%: build/obj/$(1)/common/%.o $$($(1)_COMMON) build/obj/$(1)/.config
	@mkdir -p $$(@D)
	$(4) $(5) $$(WARN) $(6) $$(LDFLAGS) -o $$@ $$(filter %.o,$$^) $$(LDLIBS)

$(2)/test_hls_ingest: build/obj/$(1)/common/test_hls_ingest.o $$($(1)_COMMON) build/obj/$(1)/common/src/core/ngx_media_hls_ingest.o build/obj/$(1)/.config
	@mkdir -p $$(@D)
	$(4) $(5) $$(WARN) $(6) $$(LDFLAGS) -o $$@ $$(filter %.o,$$^) $$(LDLIBS) -Wl,--wrap=fopen

$(2)/test_http: build/obj/$(1)/http/test_http.o $$($(1)_HTTP) build/obj/$(1)/.config
	@mkdir -p $$(@D)
	$(4) $(5) $$(WARN) $(6) $$(LDFLAGS) -o $$@ $$(filter %.o,$$^) $$(LDLIBS) $$(HTTP_LIBS)

-include $$(wildcard build/obj/$(1)/common/*.d build/obj/$(1)/common/shim/*.d build/obj/$(1)/common/src/*/*.d \
                    build/obj/$(1)/http/*.d build/obj/$(1)/http/shim/*.d build/obj/$(1)/http/src/*/*.d)
endef

$(eval $(call build_mode,normal,build,$(TESTS),$(CC),$(CFLAGS),$(SAN)))
$(eval $(call build_mode,tsan,build/tsan,$(TESTS),$(CC),$(CFLAGS),-fsanitize=thread))
$(eval $(call build_mode,native,build/native,$(TESTS),$(CC),$(CFLAGS),))
$(eval $(call build_mode,coverage,build/coverage,$(TESTS),$(COVERAGE_CC),$(COVERAGE_CFLAGS),--coverage))

all: $(BINS)
	@$(PYTHON) build_tools.py database build/compile_commands.json build/obj/normal

test: all
	@$(PYTHON) build_tools.py run . $(BINS)

tsan: $(TSAN_BINS)
	@$(PYTHON) build_tools.py database build/tsan/compile_commands.json build/obj/tsan
	@$(PYTHON) build_tools.py run . $(TSAN_BINS)

native: $(NATIVE_BINS)
	@$(PYTHON) build_tools.py database build/native/compile_commands.json build/obj/native
	@$(PYTHON) build_tools.py run build/native/run $(NATIVE_BINS)

# Native GCC/gcov instrumentation is intentionally independent of CC and SAN.
# Preserve raw .gcno/.gcda, gcov JSON, executed-command database and suite logs.
# Report real line/branch hit counts and every excluded production .c, not a floor.
coverage: $(COVERAGE_BINS)
	@$(PYTHON) build_tools.py database build/coverage/compile_commands.json build/obj/coverage
	@$(PYTHON) build_tools.py coverage build/coverage --objects build/obj/coverage \
	    --gcov "$(COVERAGE_GCOV)" $(COVERAGE_BINS)

clean:
	rm -rf build
