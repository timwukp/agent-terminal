# agent-terminal — single Makefile, per-variant build dirs
#
#   make [BUILD=release|debug|asan|fuzz]   build daemon + client
#   make test                              build and run unit tests
#   make fuzz BUILD=fuzz                   build libFuzzer targets
#   make fmt / tidy                        formatting / static analysis
#
BUILD ?= release
O := build/$(BUILD)

# Stated explicitly because the first rule in this file is the $(VERSION_H)
# stamp, not `all`, so a bare `make` regenerated one header and built NOTHING
# while reporting success. The binaries then stayed at whatever a previous
# `make all` produced, which is the worst possible failure: the test suite
# relinks its own objects and passes, integration tests run last week's daemon,
# and a source fix appears not to work. Keep this above the first rule.
.DEFAULT_GOAL := all

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

# Version stamp: the short commit hash, "-dirty" when the tree differs from
# HEAD, or "unknown" outside a git checkout (release tarballs). Generated into
# a header and compared by CONTENT before replacing, so an unchanged version
# does not touch the mtime and force rebuilds of everything that includes it.
# Outside a git checkout both subcommands fail; the -dirty suffix must not
# fire there (a tarball is not "dirty", it is just not a checkout).
AT_VERSION := $(shell if git rev-parse --short=12 HEAD >/dev/null 2>&1; then   git rev-parse --short=12 HEAD;   git diff-index --quiet HEAD -- 2>/dev/null || echo -dirty; else echo unknown; fi | tr -d '\n')
VERSION_H := $(O)/include/at_version.h

$(VERSION_H): FORCE
	@mkdir -p $(dir $@)
	@printf '#define AT_VERSION "%s"\n' '$(AT_VERSION)' > $@.tmp
	@cmp -s $@.tmp $@ 2>/dev/null && rm $@.tmp || mv $@.tmp $@

.PHONY: FORCE

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

.PHONY: all test fuzz fuzz-regress tools units install uninstall clean fmt tidy

all: $(O)/agent-terminald $(O)/agent-terminal

# -MMD -MP: every object records the headers it included, so editing a header
# rebuilds exactly its dependents. Without this, a struct-layout change in a
# header left stale objects linking against the OLD layout — a silently
# corrupted binary (garbled session names, wrong geometry) that only
# `rm -rf build` cured. Found when reordering session.h for padding.
$(O)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(shell find $(O) -name '*.d' 2>/dev/null)

# Only the two entry points bake the version in; scoping the dependency here
# keeps a hash change from rebuilding the whole tree.
$(O)/src/client/main.o $(O)/src/daemon/main.o: $(VERSION_H)
$(O)/src/client/main.o $(O)/src/daemon/main.o: CFLAGS += -I$(O)/include

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

# Same shape for the event loop, which is daemon code.
$(O)/tests/test_loop: $(O)/src/daemon/loop.o

$(O)/tests/test_layout: $(O)/src/daemon/layout.o

$(O)/tests/test_scan: $(O)/src/client/scan.o

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

# PREFIX arrives from a human's shell, and two forms of it install the binaries
# to exactly the right place while rendering a unit file that can never start:
#
#   PREFIX=~/.local  zsh does not expand a tilde after `=` (magicequalsubst is
#                    off by default) and /bin/sh expands one only at the START
#                    of a word — so `install -d ~/.local/bin` lands in $HOME,
#                    while `$(DESTDIR)~/.local/bin` becomes a directory named
#                    `<destdir>~` and sed copies the tilde into the unit
#                    verbatim. Measured on macOS: ProgramArguments[0] came out
#                    as `~/.local/bin/agent-terminald`.
#   PREFIX=out       a relative path resolves against the recipe's cwd, so the
#                    files land where the caller expects and the unit names a
#                    path that means nothing to a service manager started
#                    somewhere else.
#
# launchd's ProgramArguments[0] and systemd's ExecStart both demand an absolute
# path and expand nothing — the plist template says so in its own notes — so a
# unit holding either form is silently unstartable, which is the same
# absent-or-stale-daemon failure these templates exist to remove. Normalize
# once, here, and use the result for the binaries, the units, the stale-daemon
# check and `uninstall` alike, so those four cannot disagree about where the
# install went.
ifeq ($(HOME),)
ifneq ($(patsubst ~%,,$(PREFIX)),$(PREFIX))
$(error PREFIX=$(PREFIX) starts with a tilde but HOME is unset, so it cannot be \
expanded — pass an absolute PREFIX instead)
endif
endif
PREFIX_ABS := $(patsubst ~,$(HOME),$(patsubst ~/%,$(HOME)/%,$(PREFIX)))
PREFIX_ABS := $(if $(patsubst /%,,$(PREFIX_ABS)),$(CURDIR)/$(PREFIX_ABS),$(PREFIX_ABS))
BINDIR := $(PREFIX_ABS)/bin

# Service units are TEMPLATES, rendered here. Shipping them ready-to-copy meant
# hardcoding one prefix, and the file then named the wrong path for everyone who
# followed the other documented one — with the failure mode that a leftover
# daemon at the hardcoded prefix answers the socket and the protocol's
# skip-unknown-frames rule turns every newer message into a silent no-op. A
# stale daemon is an unpatched daemon. See tools/check_install_paths.sh.
UNIT_IN  := contrib/launchd/dev.agentterminal.daemon.plist.in \
            contrib/systemd/agent-terminald.service.in
UNITS    := $(patsubst contrib/%.in,$(O)/contrib/%,$(UNIT_IN))

# The PATH the daemon hands to session commands. BINDIR goes first, but only if
# the base list does not already contain it — a duplicate entry is harmless to
# the shell and confusing in a file someone reads.
UNIT_PATH_BASE := /opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin
UNIT_PATH := $(if $(findstring :$(BINDIR):,:$(UNIT_PATH_BASE):),$(UNIT_PATH_BASE),$(BINDIR):$(UNIT_PATH_BASE))

# FORCE, and the same compare-by-content dance as $(VERSION_H) above, because a
# rendered unit depends on the VALUE of PREFIX and no file timestamp records that.
# `make install PREFIX=/a` then `make install PREFIX=/b` found the output newer
# than the template and reinstalled /a's unit under /b — the exact stale-path bug
# these templates exist to remove, arriving through the build system instead of
# the docs. A stamp file holding the prefix does not fix it either: make 3.81
# (what macOS ships) compares mtimes at 1-second granularity, so a stamp rewritten
# in the same second as the unit still reads as "not newer". Re-running sed over a
# 100-line template costs nothing; the content compare keeps the mtime stable so
# nothing downstream churns.
#
# The delete comes first, so the template-only note — which talks ABOUT the
# placeholders — is gone before they are substituted. Otherwise the installed
# file would carry a paragraph telling its reader it is a template, with the
# placeholder in that paragraph helpfully replaced by the real path.
$(O)/contrib/%: contrib/%.in FORCE
	@mkdir -p $(dir $@)
	@sed -e '/@TEMPLATE_NOTE_BEGIN@/,/@TEMPLATE_NOTE_END@/d' \
	     -e 's|@BINDIR@|$(BINDIR)|g' \
	     -e 's|@UNIT_PATH@|$(UNIT_PATH)|g' $< > $@.tmp
	@cmp -s $@.tmp $@ 2>/dev/null && rm $@.tmp || mv $@.tmp $@

units: $(UNITS)

install: all units
	install -d $(DESTDIR)$(PREFIX_ABS)/bin $(DESTDIR)$(PREFIX_ABS)/share/man/man1 \
	           $(DESTDIR)$(PREFIX_ABS)/share/agent-terminal
	install -m 0755 $(O)/agent-terminald $(DESTDIR)$(PREFIX_ABS)/bin/
	install -m 0755 $(O)/agent-terminal $(DESTDIR)$(PREFIX_ABS)/bin/
	install -m 0644 docs/agent-terminal.1 $(DESTDIR)$(PREFIX_ABS)/share/man/man1/
	install -m 0644 $(UNITS) $(DESTDIR)$(PREFIX_ABS)/share/agent-terminal/
	@if [ -n "$(DESTDIR)" ]; then \
	    echo "note: DESTDIR set, skipping the stale-daemon check — the prefixes it looks at belong to this build host, not to the target root"; \
	else \
	    sh tools/check_install_paths.sh "$(BINDIR)"; \
	fi

uninstall:
	rm -f $(DESTDIR)$(PREFIX_ABS)/bin/agent-terminald \
	      $(DESTDIR)$(PREFIX_ABS)/bin/agent-terminal \
	      $(DESTDIR)$(PREFIX_ABS)/share/man/man1/agent-terminal.1 \
	      $(DESTDIR)$(PREFIX_ABS)/share/agent-terminal/dev.agentterminal.daemon.plist \
	      $(DESTDIR)$(PREFIX_ABS)/share/agent-terminal/agent-terminald.service
	-rmdir $(DESTDIR)$(PREFIX_ABS)/share/agent-terminal 2>/dev/null || true

fuzz-regress: $(FUZZ_REGRESS_BIN)
	@for t in $(FUZZ_REGRESS_BIN); do echo "== $$t"; $$t fuzz/corpus/vt || exit 1; done

fmt:
	clang-format -i $(shell find src tests fuzz tools -name '*.[ch]' 2>/dev/null)

tidy: $(VERSION_H)
	clang-tidy $(VT_SRC) $(COMMON_SRC) $(CLIENT_SRC) $(DAEMON_SRC) -- $(CFLAGS) -I$(O)/include

clean:
	rm -rf build
