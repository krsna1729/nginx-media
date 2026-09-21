CC      ?= cc
CFLAGS  ?= -O1 -g
SAN     ?= -fsanitize=address,undefined
WARN     = -Wall -Wextra -Werror -std=c11

INCLUDES = -I../../src/core -Ishim -I.
DEFS     = -DNGX_MEDIA_UNIT_TEST

CORE := ../../src/core/ngx_media_buffer.c \
        ../../src/core/ngx_media_frame.c \
        ../../src/core/ngx_media_feed.c \
        ../../src/core/ngx_media_track.c

SHIM := shim/ngx_shim.c

TESTS := test_buffer test_frame test_feed test_track
BINS  := $(addprefix build/,$(TESTS))

.PHONY: all test clean

all: $(BINS)

build/%: %.c ngx_media_test.h $(CORE) $(SHIM)
	@mkdir -p build
	$(CC) $(CFLAGS) $(WARN) $(DEFS) $(INCLUDES) $(SAN) -o $@ $< $(CORE) $(SHIM)

test: $(BINS)
	@fail=0; for t in $(TESTS); do ./build/$$t || fail=1; done; exit $$fail

clean:
	rm -rf build
