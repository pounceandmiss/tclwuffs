# tclwuffs — memory-safe image decode/encode/resize for Tcl/Tk.
#
# Standalone build:           make
# Zippy/embedded build:       set TCL_INCLUDE, TCL_STUB_LIB, TK_INCLUDE, TK_STUB_LIB
# Run smoke tests:            make smoke && make test

VERSION   := 0.1.0

CC        ?= cc
AR        ?= ar
INSTALL   ?= install
DESTDIR   ?=
PREFIX    ?= /usr/local

# ---- Tcl 9 detection ------------------------------------------------------
# Picks the first tclConfig.sh whose TCL_VERSION starts with "9.".
# Override with TCLCONFIG=/path/to/tclConfig.sh, or set TCL_INCLUDE /
# TCL_STUB_LIB directly (that's how embedding hosts like zippy drive the build).
TCL_CANDIDATES := \
  /usr/local/lib/tclConfig.sh \
  /usr/lib/tclConfig.sh \
  /usr/lib64/tclConfig.sh \
  /usr/lib/tcl9.0/tclConfig.sh \
  /opt/homebrew/lib/tclConfig.sh
TCLCONFIG ?= $(shell for f in $(TCL_CANDIDATES); do \
  [ -f "$$f" ] || continue; \
  v=$$(. "$$f" 2>/dev/null; echo $$TCL_VERSION); \
  if [ "$${v#9.}" != "$$v" ]; then echo "$$f"; exit 0; fi; \
done)
ifneq ($(wildcard $(TCLCONFIG)),)
  TCL_INCLUDE  ?= $(shell . $(TCLCONFIG); echo $$TCL_INCLUDE_SPEC)
  TCL_STUB_LIB ?= $(shell . $(TCLCONFIG); echo $$TCL_STUB_LIB_SPEC)
endif

# ---- Tk 9 detection -------------------------------------------------------
# Same pattern as Tcl: only matches Tk 9.x. Override with TKCONFIG=/path or
# set TK_INCLUDE / TK_STUB_LIB directly.
TK_CANDIDATES := \
  /usr/local/lib/tkConfig.sh \
  /usr/lib/tkConfig.sh \
  /usr/lib64/tkConfig.sh \
  /usr/lib/tk9.0/tkConfig.sh \
  /opt/homebrew/lib/tkConfig.sh
TKCONFIG ?= $(shell for f in $(TK_CANDIDATES); do \
  [ -f "$$f" ] || continue; \
  v=$$(. "$$f" 2>/dev/null; echo $$TK_VERSION); \
  if [ "$${v#9.}" != "$$v" ]; then echo "$$f"; exit 0; fi; \
done)
ifneq ($(wildcard $(TKCONFIG)),)
  TK_INCLUDE  ?= $(shell . $(TKCONFIG); echo $$TK_INCLUDE_SPEC)
  TK_STUB_LIB ?= $(shell . $(TKCONFIG); echo $$TK_STUB_LIB_SPEC)
  HAVE_TK := 1
endif

# ---- Flags ----------------------------------------------------------------
WARN       := -Wall -Wno-unused-function
OPT        ?= -O2
COMMON_CF  := $(WARN) $(OPT) -Isrc -Ivendor \
              -DPACKAGE_VERSION='"$(VERSION)"' \
              -DUSE_TCL_STUBS \
              $(TCL_INCLUDE) $(CFLAGS)
PIC_CF     := $(COMMON_CF) -fPIC
STATIC_CF  := $(COMMON_CF)
TK_CF      := -DUSE_TK_STUBS $(TK_INCLUDE)

LDLIBS_BASE := -lm
SO_FLAGS    := -shared

# ---- Sources --------------------------------------------------------------
SHARED_SRC := src/wuffs_glue.c src/vendor_wuffs.c src/vendor_stbresize.c src/vendor_stbwrite.c
TCL_SRC    := src/tcl_bindings.c
TK_SRC     := src/tk_bindings.c src/tk_formats.c

PIC_DIR    := build/pic
STATIC_DIR := build/static

# Object lists
PIC_SHARED_O    := $(SHARED_SRC:src/%.c=$(PIC_DIR)/%.o)
STATIC_SHARED_O := $(SHARED_SRC:src/%.c=$(STATIC_DIR)/%.o)
PIC_TCL_O       := $(TCL_SRC:src/%.c=$(PIC_DIR)/%.o)
STATIC_TCL_O    := $(TCL_SRC:src/%.c=$(STATIC_DIR)/%.o)
PIC_TK_O        := $(TK_SRC:src/%.c=$(PIC_DIR)/%.o)
STATIC_TK_O     := $(TK_SRC:src/%.c=$(STATIC_DIR)/%.o)

# ---- Outputs --------------------------------------------------------------
LIB_TCL_SO := libtclwuffs$(VERSION).so
LIB_TCL_A  := libtclwuffs$(VERSION).a
LIB_TK_SO  := libtkwuffs$(VERSION).so
LIB_TK_A   := libtkwuffs$(VERSION).a
PKGINDEX   := pkgIndex.tcl

# ---- Phony targets --------------------------------------------------------
.PHONY: all tclwuffs tkwuffs smoke test test-tcl test-tk clean config install

ifeq ($(HAVE_TK),1)
all: tclwuffs tkwuffs $(PKGINDEX)
else
all: tclwuffs $(PKGINDEX)
	@echo "Note: Tk not detected (set TKCONFIG); skipped tkwuffs flavor."
endif

tclwuffs: $(LIB_TCL_SO) $(LIB_TCL_A)

ifeq ($(HAVE_TK),1)
tkwuffs:  $(LIB_TK_SO) $(LIB_TK_A)
else
tkwuffs:
	@echo "ERROR: Tk not detected — set TKCONFIG to your Tk 9 tkConfig.sh." >&2; exit 1
endif

# ---- Build directories ----------------------------------------------------
$(PIC_DIR) $(STATIC_DIR) build:
	mkdir -p $@

# ---- Compile rules --------------------------------------------------------
# Shared & Tcl-binding objects (Tcl-only flags)
$(PIC_DIR)/wuffs_glue.o $(PIC_DIR)/vendor_wuffs.o $(PIC_DIR)/vendor_stbwrite.o $(PIC_DIR)/vendor_stbresize.o $(PIC_DIR)/tcl_bindings.o: $(PIC_DIR)/%.o: src/%.c | $(PIC_DIR)
	$(CC) $(PIC_CF) -c $< -o $@

$(STATIC_DIR)/wuffs_glue.o $(STATIC_DIR)/vendor_wuffs.o $(STATIC_DIR)/vendor_stbwrite.o $(STATIC_DIR)/vendor_stbresize.o $(STATIC_DIR)/tcl_bindings.o: $(STATIC_DIR)/%.o: src/%.c | $(STATIC_DIR)
	$(CC) $(STATIC_CF) -c $< -o $@

# Tk-binding objects need TK_CF
$(PIC_DIR)/tk_bindings.o $(PIC_DIR)/tk_formats.o: $(PIC_DIR)/%.o: src/%.c | $(PIC_DIR)
	$(CC) $(PIC_CF) $(TK_CF) -c $< -o $@

$(STATIC_DIR)/tk_bindings.o $(STATIC_DIR)/tk_formats.o: $(STATIC_DIR)/%.o: src/%.c | $(STATIC_DIR)
	$(CC) $(STATIC_CF) $(TK_CF) -c $< -o $@

# ---- Link rules -----------------------------------------------------------
$(LIB_TCL_SO): $(PIC_SHARED_O) $(PIC_TCL_O)
	$(CC) $(SO_FLAGS) -o $@ $^ $(TCL_STUB_LIB) $(LDLIBS_BASE)

$(LIB_TCL_A): $(STATIC_SHARED_O) $(STATIC_TCL_O)
	$(AR) rcs $@ $^

$(LIB_TK_SO): $(PIC_SHARED_O) $(PIC_TK_O)
	$(CC) $(SO_FLAGS) -o $@ $^ $(TCL_STUB_LIB) $(TK_STUB_LIB) $(LDLIBS_BASE)

# libtkwuffs.a strictly extends libtclwuffs.a — shared glue lives only in
# libtclwuffs.a, so static consumers link both (-ltkwuffs -ltclwuffs) with no
# duplicate symbols.
$(LIB_TK_A): $(STATIC_TK_O)
	$(AR) rcs $@ $^

# ---- pkgIndex.tcl ---------------------------------------------------------
$(PKGINDEX): pkgIndex.tcl.in Makefile
	sed -e 's/@VERSION@/$(VERSION)/g' pkgIndex.tcl.in > $@

# ---- Smoke / tests --------------------------------------------------------
SMOKE_BIN := build/smoke

smoke: $(SMOKE_BIN)
	./$(SMOKE_BIN)

$(SMOKE_BIN): tests/smoke.c $(STATIC_SHARED_O) | build
	$(CC) $(OPT) $(WARN) -Isrc -Ivendor -o $@ tests/smoke.c $(STATIC_SHARED_O) $(LDLIBS_BASE)

test: test-tcl
ifeq ($(HAVE_TK),1)
test: test-tk
endif

test-tcl: $(LIB_TCL_SO) $(PKGINDEX)
	@command -v tclsh9.0 >/dev/null 2>&1 || { echo "tclsh9.0 not found in PATH" >&2; exit 1; }
	tclsh9.0 tests/test_tclwuffs.tcl

test-tk: $(LIB_TK_SO) $(PKGINDEX)
	@command -v wish9.0 >/dev/null 2>&1 || { echo "wish9.0 not found in PATH" >&2; exit 1; }
	wish9.0 tests/test_tkwuffs.tcl
	wish9.0 tests/test_tk_formats.tcl

# ---- Vendored sources ----------------------------------------------------
# vendor/ is committed; refresh manually when bumping pins.
# google/wuffs:  https://github.com/google/wuffs @ 3f03a885b4aedf236fa52e8cb94baf3fa2ef9a24
# nothings/stb: https://github.com/nothings/stb  @ 904aa67e1e2d1dec92959df63e700b166d5c1022

# ---- Housekeeping ---------------------------------------------------------
clean:
	rm -rf build $(LIB_TCL_SO) $(LIB_TCL_A) $(LIB_TK_SO) $(LIB_TK_A) $(PKGINDEX)

config:
	@echo "VERSION:      $(VERSION)"
	@echo "CC:           $(CC)"
	@echo "TCL_INCLUDE:  $(TCL_INCLUDE)"
	@echo "TCL_STUB_LIB: $(TCL_STUB_LIB)"
	@echo "TK_INCLUDE:   $(TK_INCLUDE)"
	@echo "TK_STUB_LIB:  $(TK_STUB_LIB)"
	@echo "HAVE_TK:      $(HAVE_TK)"
	@echo "TCLCONFIG:    $(TCLCONFIG)"
	@echo "TKCONFIG:     $(TKCONFIG)"

# ---- Install --------------------------------------------------------------
# Drops the .so + pkgIndex.tcl into PREFIX/lib/tclwuffs$(VERSION)/.
INSTALL_DIR := $(DESTDIR)$(PREFIX)/lib/tclwuffs$(VERSION)

install: all
	$(INSTALL) -d $(INSTALL_DIR)
	$(INSTALL) -m 0644 $(LIB_TCL_SO) $(PKGINDEX) $(INSTALL_DIR)/
ifeq ($(HAVE_TK),1)
	$(INSTALL) -m 0644 $(LIB_TK_SO) tkwuffs_animate.tcl $(INSTALL_DIR)/
endif
