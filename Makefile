# agent-terminal — single Makefile, per-variant build dirs
#
#   make [BUILD=release|debug|asan|fuzz]   build daemon + client
#   make test                              build and run unit tests
#   make fuzz BUILD=fuzz                   build libFuzzer targets
#   make fmt / tidy                        formatting / static analysis
#
BUILD ?= release
O := build/$(BUILD)

UNAME_S := $(shell uname -s)

CC ?= cc
# Feature-test macros: strict -std=c17 hides POSIX declarations on glibc
# (CLOCK_MONOTONIC, lstat, ...). Harmless duplicates of per-file defines.
COMMON_CFLAGS := -std=c17 -Wall -Wextra -Wpedantic -Wshadow -Wvla -Wconversion \
                 -fno-common -fstack-protector-strong -fPIE \
                 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE \
                 -Isrc

ifeq ($(BUILD),release)
  CFLAGS := $(COMMON_CFLAGS) -O2
  ifeq ($(UNAME_S),Linux)
    CFLAGS += -D_FORTIFY_SOURCE=2
  endif
else ifeq ($(BUILD),debug)
  CFLAGS := $(COMMON_CFLAGS) -O0 -g3
else ifeq ($(BUILD),asan)
  CFLAGS := $(COMMON_CFLAGS) -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all
  LDFLAGS += -fsanitize=address,undefined
else ifeq ($(BUILD),fuzz)
  CFLAGS := $(COMMON_CFLAGS) -O1 -g -fsanitize=fuzzer-no-link,address,undefined
  LDFLAGS += -fsanitize=address,undefined
else
  $(error unknown BUILD '$(BUILD)')
endif

ifeq ($(UNAME_S),Linux)
  LDFLAGS += -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack
endif

COMMON_SRC := $(wildcard src/common/*.c)
VT_SRC     := $(wildcard src/vt/*.c)
DAEMON_SRC := $(wildcard src/daemon/*.c)
CLIENT_SRC := $(wildcard src/client/*.c)

COMMON_OBJ := $(COMMON_SRC:%.c=$(O)/%.o)
VT_OBJ     := $(VT_SRC:%.c=$(O)/%.o)
DAEMON_OBJ := $(DAEMON_SRC:%.c=$(O)/%.o)
CLIENT_OBJ := $(CLIENT_SRC:%.c=$(O)/%.o)

# libvt.a exists only once src/vt/ has sources (lands in M2); everything
# below links $(LIBS), which includes it conditionally.
LIBS := $(O)/libcommon.a
ifneq ($(strip $(VT_SRC)),)
  LIBS := $(O)/libvt.a $(LIBS)
endif

.PHONY: all test fuzz fuzz-regress tools install uninstall clean fmt tidy

all: $(O)/agent-terminald $(O)/agent-terminal

$(O)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(O)/libvt.a: $(VT_OBJ)
	ar rcs $@ $^

$(O)/libcommon.a: $(COMMON_OBJ)
	ar rcs $@ $^

$(O)/agent-terminald: $(DAEMON_OBJ) $(LIBS)
	$(CC) $(LDFLAGS) -o $@ $^

$(O)/agent-terminal: $(CLIENT_OBJ) $(O)/libcommon.a
	$(CC) $(LDFLAGS) -o $@ $^

# ---- tests ------------------------------------------------------------------
TEST_SRC := $(wildcard tests/unit/*.c)
TEST_BIN := $(TEST_SRC:tests/unit/%.c=$(O)/tests/%)

# Archives go LAST on the link line, after every object. GNU ld resolves static
# libraries in command-line order and does not revisit an archive it has already
# passed, so an extra object appended by a target-specific prerequisite (below)
# would fail to find xmalloc/sb_read_log. Apple's ld64 resolves regardless of
# order, so getting this wrong builds clean on macOS and fails only on Linux.
$(O)/tests/%: tests/unit/%.c $(LIBS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -Itests/unit $(LDFLAGS) -o $@ \
	    $(filter-out $(LIBS),$^) $(LIBS)

# The pager is client code, not a library, so it is not in $(LIBS). Naming the
# object as an extra prerequisite keeps the generic rule above unchanged (the
# recipe picks it up) instead of duplicating the recipe.
$(O)/tests/test_pager: $(O)/src/client/pager.o

test: $(TEST_BIN)
	@rc=0; for t in $(TEST_BIN); do echo "== $$t"; $$t || rc=1; done; exit $$rc

# ---- fuzz -------------------------------------------------------------------
# Two flavors:
#   make fuzz            libFuzzer binaries (needs clang WITH fuzzer runtime —
#                        Linux clang or brew llvm; Apple clang lacks it)
#   make fuzz-regress    standalone drivers: replay corpus under ASan/UBSan,
#                        works with any compiler (local regression gate)
FUZZ_SRC := $(filter-out fuzz/standalone_driver.c,$(wildcard fuzz/*.c))
FUZZ_BIN := $(FUZZ_SRC:fuzz/%.c=$(O)/fuzz/%)
FUZZ_REGRESS_BIN := $(FUZZ_SRC:fuzz/%.c=$(O)/fuzz/%_regress)

$(O)/fuzz/%_regress: fuzz/%.c fuzz/standalone_driver.c $(LIBS)
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -fsanitize=fuzzer-no-link%,$(CFLAGS)) -fsanitize=address,undefined -o $@ $^

$(O)/fuzz/%: fuzz/%.c $(LIBS)
	@mkdir -p $(dir $@)
	$(CC) $(filter-out -fsanitize=fuzzer-no-link%,$(CFLAGS)) -fsanitize=fuzzer,address,undefined -o $@ $^

fuzz: $(FUZZ_BIN)

# ---- tools ------------------------------------------------------------------
$(O)/vtdump: tools/vtdump.c $(O)/libvt.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

tools: $(O)/vtdump

# ---- install ----------------------------------------------------------------
PREFIX ?= /usr/local

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/man/man1
	install -m 0755 $(O)/agent-terminald $(DESTDIR)$(PREFIX)/bin/
	install -m 0755 $(O)/agent-terminal $(DESTDIR)$(PREFIX)/bin/
	install -m 0644 docs/agent-terminal.1 $(DESTDIR)$(PREFIX)/share/man/man1/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/agent-terminald \
	      $(DESTDIR)$(PREFIX)/bin/agent-terminal \
	      $(DESTDIR)$(PREFIX)/share/man/man1/agent-terminal.1

fuzz-regress: $(FUZZ_REGRESS_BIN)
	@for t in $(FUZZ_REGRESS_BIN); do echo "== $$t"; $$t fuzz/corpus/vt || exit 1; done

fmt:
	clang-format -i $(shell find src tests fuzz tools -name '*.[ch]' 2>/dev/null)

tidy:
	clang-tidy $(VT_SRC) $(COMMON_SRC) -- $(CFLAGS)

clean:
	rm -rf build
