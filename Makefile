PACKAGE := usbradioplus
.DEFAULT_GOAL := all
VERSION := $(strip $(shell cat VERSION))
DISTNAME := $(PACKAGE)-$(VERSION)

prefix ?= /usr/local
exec_prefix ?= $(prefix)
sbindir ?= $(exec_prefix)/sbin
libdir ?= $(exec_prefix)/lib
datarootdir ?= $(prefix)/share
docdir ?= $(datarootdir)/doc/$(PACKAGE)
mandir ?= $(datarootdir)/man
sysconfdir ?= /etc
MULTIARCH ?= $(strip $(shell dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null))
asteriskmoduledir ?= $(libdir)$(if $(MULTIARCH),/$(MULTIARCH))/asterisk/modules
agcplugindir ?= $(libdir)$(if $(MULTIARCH),/$(MULTIARCH))/usbradioplus
DESTDIR ?=

CC ?= cc
NM ?= nm
PKG_CONFIG ?= pkg-config
INSTALL ?= install
INSTALL_PROGRAM ?= $(INSTALL)
INSTALL_DATA ?= $(INSTALL) -m 0644
PYTHON ?= python3
TAR ?= tar
GCOVR ?= gcovr
DOXYGEN ?= doxygen
RUFF ?= ruff
CLANG_FORMAT ?= clang-format
CPPCHECK ?= cppcheck
SHELLCHECK ?= shellcheck
CARGO ?= cargo
CARGO_LLVM_COV ?= cargo llvm-cov
READELF ?= readelf

CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=
WARNFLAGS ?= -Wall -Wextra -Werror -Wno-old-style-declaration
ASTERISK_INCLUDEDIR ?= /usr/include
BUILD_DIR ?= build
DIST_DIR ?= dist
CARGO_TARGET_DIR ?= $(abspath $(BUILD_DIR)/cargo)
COVERAGE_TOOLCHAIN ?= nightly-2025-02-20
# Use the released shared ring ABI. CI and normal builds consume installed
# pkg-config metadata and runtime SONAMEs; a source override is diagnostic-only.
RPCR_SOURCE ?=
ifneq ($(strip $(RPCR_SOURCE)),)
RPCR_STAGE ?= $(CURDIR)/build/rpcr-stage
RPCR_PREFIX := $(RPCR_STAGE)/usr
RPCR_LIBRARY := $(RPCR_PREFIX)/lib/librate_adjusting_pcm_ring2.so
RPCR_SOURCE_FILES := $(RPCR_SOURCE)/Makefile \
	$(RPCR_SOURCE)/Cargo.toml $(RPCR_SOURCE)/Cargo.lock \
	$(RPCR_SOURCE)/rate_adjusting_pcm_ring2.pc.in \
	$(wildcard $(RPCR_SOURCE)/include/*.h $(RPCR_SOURCE)/src/*.rs)
RPCR_CFLAGS := -I$(RPCR_PREFIX)/include
RPCR_LIBS := -L$(RPCR_PREFIX)/lib -lrate_adjusting_pcm_ring2
RPCR_BUILD_DEP := $(RPCR_LIBRARY)
else
ifeq ($(shell $(PKG_CONFIG) --exists rate_adjusting_pcm_ring2 && echo yes),)
$(error USBRadioPlus requires the librate-adjusting-pcm-ring2 development package)
endif
RPCR_CFLAGS := $(shell $(PKG_CONFIG) --cflags rate_adjusting_pcm_ring2)
# pkg-config suppresses the standard multiarch -L path. Keep it explicitly so
# a stale /usr/local development symlink cannot select an older ring SONAME.
RPCR_LIBS := -L$(shell $(PKG_CONFIG) --variable=libdir rate_adjusting_pcm_ring2) \
	$(shell $(PKG_CONFIG) --libs rate_adjusting_pcm_ring2)
RPCR_BUILD_DEP :=
endif
# USBRadioPlus consumes the portable Rust radio core through its released
# versioned shared-object ABI. CI and ordinary builds consume its installed
# development package; a source override is diagnostic-only.
RPTADV_RADIO_SOURCE ?=
ifneq ($(strip $(RPTADV_RADIO_SOURCE)),)
RPTADV_RADIO_STAGE ?= $(CURDIR)/build/rptadvradio-stage
RPTADV_RADIO_PREFIX := $(RPTADV_RADIO_STAGE)/usr
RPTADV_RADIO_LIBDIR := $(RPTADV_RADIO_PREFIX)/lib$(if $(MULTIARCH),/$(MULTIARCH))
RPTADV_RADIO_LIBRARY := $(RPTADV_RADIO_LIBDIR)/librptadvradio.so
RPTADV_RADIO_SOURCE_FILES := $(RPTADV_RADIO_SOURCE)/Makefile \
	$(RPTADV_RADIO_SOURCE)/Cargo.toml $(RPTADV_RADIO_SOURCE)/Cargo.lock \
	$(RPTADV_RADIO_SOURCE)/rptadvradio.pc.in \
	$(wildcard $(RPTADV_RADIO_SOURCE)/include/rptadvradio/*.h \
		$(RPTADV_RADIO_SOURCE)/src/*.rs)
RPTADV_RADIO_CFLAGS := -I$(RPTADV_RADIO_PREFIX)/include
RPTADV_RADIO_LIBS := $(RPTADV_RADIO_LIBRARY)
RPTADV_RADIO_BUILD_DEP := $(RPTADV_RADIO_LIBRARY)
else
ifeq ($(shell $(PKG_CONFIG) --exists rptadvradio && echo yes),)
$(error USBRadioPlus requires the librptadvradio development package)
endif
ifneq ($(shell $(PKG_CONFIG) --variable=abi_version rptadvradio),4)
$(error USBRadioPlus requires librptadvradio descriptor ABI 4 from alpha.4 or newer)
endif
RPTADV_RADIO_CFLAGS := $(shell $(PKG_CONFIG) --cflags rptadvradio)
RPTADV_RADIO_LIBS := -L$(shell $(PKG_CONFIG) --variable=libdir rptadvradio) \
	$(shell $(PKG_CONFIG) --libs rptadvradio)
RPTADV_RADIO_BUILD_DEP :=
endif
# USBRadioPlus routes its current mono sinc compatibility conversion through
# this released dynamically linked adapter. There is no direct converter
# fallback in the native compatibility path.
RPTADV_SAMPLERATE_SOURCE ?=
ifneq ($(strip $(RPTADV_SAMPLERATE_SOURCE)),)
RPTADV_SAMPLERATE_STAGE ?= $(CURDIR)/build/rptadv-samplerate-adapter-stage
RPTADV_SAMPLERATE_PREFIX := $(RPTADV_SAMPLERATE_STAGE)/usr
RPTADV_SAMPLERATE_LIBDIR := $(RPTADV_SAMPLERATE_PREFIX)/lib$(if $(MULTIARCH),/$(MULTIARCH))
RPTADV_SAMPLERATE_LIBRARY := $(RPTADV_SAMPLERATE_LIBDIR)/librptadv_samplerate_adapter.so
RPTADV_SAMPLERATE_SOURCE_FILES := $(RPTADV_SAMPLERATE_SOURCE)/Makefile \
	$(RPTADV_SAMPLERATE_SOURCE)/Cargo.toml $(RPTADV_SAMPLERATE_SOURCE)/Cargo.lock \
	$(wildcard $(RPTADV_SAMPLERATE_SOURCE)/include/rptadv_samplerate_adapter/*.h \
		$(RPTADV_SAMPLERATE_SOURCE)/src/*.rs)
RPTADV_SAMPLERATE_CFLAGS := -I$(RPTADV_SAMPLERATE_PREFIX)/include
RPTADV_SAMPLERATE_LIBS := -L$(RPTADV_SAMPLERATE_LIBDIR) -lrptadv_samplerate_adapter
RPTADV_SAMPLERATE_BUILD_DEP := $(RPTADV_SAMPLERATE_LIBRARY)
else
ifeq ($(shell $(PKG_CONFIG) --exists rptadv_samplerate_adapter && echo yes),)
$(error USBRadioPlus requires the librptadv-samplerate-adapter development package)
endif
RPTADV_SAMPLERATE_CFLAGS := $(shell $(PKG_CONFIG) --cflags rptadv_samplerate_adapter)
RPTADV_SAMPLERATE_LIBS := -L$(shell $(PKG_CONFIG) --variable=libdir rptadv_samplerate_adapter) \
	$(shell $(PKG_CONFIG) --libs rptadv_samplerate_adapter)
RPTADV_SAMPLERATE_BUILD_DEP :=
endif
# Native signaling graphs use the released dynamic FFmpeg adapter.
RPTADV_FFMPEG_SOURCE ?=
ifneq ($(strip $(RPTADV_FFMPEG_SOURCE)),)
RPTADV_FFMPEG_STAGE ?= $(CURDIR)/build/rptadv-ffmpeg-adapter-stage
RPTADV_FFMPEG_PREFIX := $(RPTADV_FFMPEG_STAGE)/usr
RPTADV_FFMPEG_LIBDIR := $(RPTADV_FFMPEG_PREFIX)/lib$(if $(MULTIARCH),/$(MULTIARCH))
RPTADV_FFMPEG_LIBRARY := $(RPTADV_FFMPEG_LIBDIR)/librptadv_ffmpeg_adapter.so
RPTADV_FFMPEG_SOURCE_FILES := $(RPTADV_FFMPEG_SOURCE)/Makefile \
	$(RPTADV_FFMPEG_SOURCE)/Cargo.toml $(RPTADV_FFMPEG_SOURCE)/Cargo.lock \
	$(wildcard $(RPTADV_FFMPEG_SOURCE)/include/rptadv_ffmpeg_adapter/*.h \
		$(RPTADV_FFMPEG_SOURCE)/src/*.rs $(RPTADV_FFMPEG_SOURCE)/src/*.c \
		$(RPTADV_FFMPEG_SOURCE)/src/*.h)
RPTADV_FFMPEG_CFLAGS := -I$(RPTADV_FFMPEG_PREFIX)/include
RPTADV_FFMPEG_LIBS := -L$(RPTADV_FFMPEG_LIBDIR) -lrptadv_ffmpeg_adapter
RPTADV_FFMPEG_BUILD_DEP := $(RPTADV_FFMPEG_LIBRARY)
else
ifeq ($(shell $(PKG_CONFIG) --exists rptadv_ffmpeg_adapter && echo yes),)
$(error USBRadioPlus requires the librptadv-ffmpeg-adapter development package)
endif
RPTADV_FFMPEG_CFLAGS := $(shell $(PKG_CONFIG) --cflags rptadv_ffmpeg_adapter)
RPTADV_FFMPEG_LIBS := -L$(shell $(PKG_CONFIG) --variable=libdir rptadv_ffmpeg_adapter) \
	$(shell $(PKG_CONFIG) --libs rptadv_ffmpeg_adapter)
RPTADV_FFMPEG_BUILD_DEP :=
endif
PARALLEL_JOBS ?= $(strip $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2))
SOURCE_DATE_EPOCH ?= $(shell git log -1 --format=%ct 2>/dev/null || echo 0)

CHANNEL_SOURCE := src/chan_usbradioplus_shim.c
CHANNEL_CPPFLAGS :=
# Every channel uses the released audio and GPIO contracts. Neither capability
# may be omitted and no Asterisk resource-module hardware API is selected.
RADIO_PACKAGES := rptadv_portaudio_alsa_adapter rptadv_gpio_adapter rptadv_rnnoise_adapter
ifeq ($(shell $(PKG_CONFIG) --exists $(RADIO_PACKAGES) && echo yes),)
$(error USBRadioPlus requires the released audio, GPIO, and RNNoise adapter development packages)
endif
ifeq ($(shell $(PKG_CONFIG) --atleast-version=0.1.0~alpha2 rptadv_portaudio_alsa_adapter && echo yes),)
$(error USBRadioPlus requires librptadv-portaudio-alsa-adapter-dev 0.1.0~alpha2 or newer)
endif
RADIO_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(RADIO_PACKAGES))
RADIO_LIBS := $(shell $(PKG_CONFIG) --libs $(RADIO_PACKAGES))
# External Asterisk headers use GNU pthread declarations before autoconfig.h
# can request them, so make that feature set explicit for every module build.
COMMON_CPPFLAGS := -D_GNU_SOURCE -I$(ASTERISK_INCLUDEDIR) -Isrc -Irust/asterisk/include $(RPCR_CFLAGS) \
	$(RPTADV_RADIO_CFLAGS) $(RPTADV_SAMPLERATE_CFLAGS) $(RPTADV_FFMPEG_CFLAGS)
MODULE := $(BUILD_DIR)/chan_usbradioplus.so
USBRADIOPLUS_LIBDIR ?= $(libdir)$(if $(MULTIARCH),/$(MULTIARCH))
ASTERISK_ADAPTER_HEADER := rust/asterisk/include/usbradioplus_asterisk.h
ASTERISK_ADAPTER_SONAME := libusbradioplus_asterisk.so.1
ASTERISK_ADAPTER_VERSIONED := $(BUILD_DIR)/$(ASTERISK_ADAPTER_SONAME)
ASTERISK_ADAPTER := $(BUILD_DIR)/libusbradioplus_asterisk.so
RUST_ASTERISK_ADAPTER := $(CARGO_TARGET_DIR)/release/libusbradioplus_asterisk.so
TUNER := $(BUILD_DIR)/usbradioplus-tune
RUST_TUNER := $(CARGO_TARGET_DIR)/release/usbradioplus-tune
AGC_PLUGIN := $(BUILD_DIR)/usbradioplus_agc.so
AGC_PLUGIN_SONAME := usbradioplus_agc.so.1
AGC_PLUGIN_VERSIONED := $(BUILD_DIR)/$(AGC_PLUGIN_SONAME)
RUST_AGC_LIBRARY := $(CARGO_TARGET_DIR)/release/libusbradioplus_agc.so
RUST_SOURCES := $(shell find rust -type f \( -name '*.rs' -o -name Cargo.toml \))
RUST_BUILD_STAMP := $(BUILD_DIR)/.rust-release
AGC_PLUGIN_CPPFLAGS := -DURP_AGC_PLUGIN_PATH='"$(agcplugindir)/usbradioplus_agc.so"'
BUILD_CONFIG_STAMP := $(BUILD_DIR)/.module-build-config
TARBALL := $(DIST_DIR)/$(DISTNAME).tar.xz

CHANNEL_OBJECT := $(BUILD_DIR)/$(patsubst src/%.c,%.o,$(CHANNEL_SOURCE))
MODULE_OBJECTS := $(CHANNEL_OBJECT)

MODULE_SOURCES := $(CHANNEL_SOURCE) $(ASTERISK_ADAPTER_HEADER)
DIST_TOP := Makefile VERSION CHANGELOG.md COPYING README.md INSTALL.md \
	Cargo.toml Cargo.lock rust-toolchain.toml \
	RELEASE-CHECKLIST.md CONTRIBUTING.md AGENTS.md Doxyfile pyproject.toml \
	.clang-format .clang-tidy .dockerignore install.sh
DIST_DIRS := .github containers debian packaging src rust scripts examples man doc tests tests_py tests_docs tools
DIST_FILES := $(DIST_TOP) $(shell find $(DIST_DIRS) -type f \
	! -name '*.pyc' ! -name '*.gcda' ! -name '*.gcno' ! -name '*.gcov' \
	! -name '*.profraw' ! -name '*.profdata' ! -path '*/target/*' \
	! -name '*.cap' ! -name '*.raw' ! -name '*.wav' ! -name '*.au' \
	! -path '*/__pycache__/*' ! -path '*/.pytest_cache/*' ! -path '*/.ruff_cache/*' \
	! -path '*/.coverage/*' ! -path '*/.coverage-*/*' \
	! -path '*/build/*' ! -path '*/dist/*' ! -path '*/work/*' ! -path '*/outputs/*' | LC_ALL=C sort)

.PHONY: all check ci coverage rust-coverage rustdoc docs lint static-analysis platform-verify \
	clean dist distcheck install install-strip install-from-dist \
	uninstall validate-release

.PHONY: force-agc-path force-build-config

all: $(RPCR_BUILD_DEP) $(RPTADV_RADIO_BUILD_DEP) $(RPTADV_SAMPLERATE_BUILD_DEP) $(RPTADV_FFMPEG_BUILD_DEP) $(MODULE) $(AGC_PLUGIN) $(TUNER)

$(BUILD_DIR):
	mkdir -p $@

# Object files embed the selected headers and shared adapter state layout.
# Invalidate objects atomically whenever the compilation configuration changes.
$(BUILD_CONFIG_STAMP): force-build-config | $(BUILD_DIR)
	@printf '%s\n' 'ASTERISK_INCLUDEDIR=$(ASTERISK_INCLUDEDIR)' \
		'CPPFLAGS=$(CPPFLAGS)' 'CFLAGS=$(CFLAGS)' \
		'CHANNEL_CPPFLAGS=$(CHANNEL_CPPFLAGS)' \
		'COMMON_CPPFLAGS=$(COMMON_CPPFLAGS)' \
		'RADIO_CFLAGS=$(RADIO_CFLAGS)' \
		'RUSTFLAGS=$(RUSTFLAGS)' 'CARGO_TARGET_DIR=$(CARGO_TARGET_DIR)' \
		'RPCR_CFLAGS=$(RPCR_CFLAGS)' 'RPTADV_RADIO_CFLAGS=$(RPTADV_RADIO_CFLAGS)' \
		'RPTADV_SAMPLERATE_CFLAGS=$(RPTADV_SAMPLERATE_CFLAGS)' \
		'RPTADV_FFMPEG_CFLAGS=$(RPTADV_FFMPEG_CFLAGS)' > $@.tmp
	@if test -f $@ && cmp -s $@.tmp $@; then \
		rm -f $@.tmp; \
	else \
		mv $@.tmp $@; \
		rm -f $(MODULE_OBJECTS) $(RUST_BUILD_STAMP); \
	fi

ifneq ($(strip $(RPCR_SOURCE)),)
$(RPCR_LIBRARY): $(RPCR_SOURCE_FILES)
	$(MAKE) -C $(RPCR_SOURCE) DESTDIR=$(RPCR_STAGE) PREFIX=/usr LIBDIR=/usr/lib install
endif

ifneq ($(strip $(RPTADV_RADIO_SOURCE)),)
$(RPTADV_RADIO_LIBRARY): $(RPTADV_RADIO_SOURCE_FILES)
	$(MAKE) -C $(RPTADV_RADIO_SOURCE) DESTDIR=$(RPTADV_RADIO_STAGE) PREFIX=/usr \
		LIBDIR=/usr/lib$(if $(MULTIARCH),/$(MULTIARCH)) install
endif

ifneq ($(strip $(RPTADV_SAMPLERATE_SOURCE)),)
$(RPTADV_SAMPLERATE_LIBRARY): $(RPTADV_SAMPLERATE_SOURCE_FILES)
	$(MAKE) -C $(RPTADV_SAMPLERATE_SOURCE) DESTDIR=$(RPTADV_SAMPLERATE_STAGE) PREFIX=/usr \
		LIBDIR=/usr/lib$(if $(MULTIARCH),/$(MULTIARCH)) install
endif

ifneq ($(strip $(RPTADV_FFMPEG_SOURCE)),)
$(RPTADV_FFMPEG_LIBRARY): $(RPTADV_FFMPEG_SOURCE_FILES)
	$(MAKE) -C $(RPTADV_FFMPEG_SOURCE) DESTDIR=$(RPTADV_FFMPEG_STAGE) PREFIX=/usr \
		LIBDIR=/usr/lib$(if $(MULTIARCH),/$(MULTIARCH)) install
endif

# A later staged install may select a different prefix from the initial build.
# Track it so the module cannot retain a stale private-plugin location.
$(BUILD_DIR)/agc-plugin-path: force-agc-path | $(BUILD_DIR)
	@printf '%s\n' '$(agcplugindir)/usbradioplus_agc.so' > $@.tmp
	@if cmp -s $@.tmp $@; then \
		rm -f $@.tmp; \
	else \
		mv $@.tmp $@; \
		rm -f $(CHANNEL_OBJECT); \
	fi

$(CHANNEL_OBJECT): $(BUILD_DIR)/agc-plugin-path

$(BUILD_DIR)/%.o: src/%.c $(MODULE_SOURCES) $(RPCR_BUILD_DEP) $(RPTADV_RADIO_BUILD_DEP) $(RPTADV_SAMPLERATE_BUILD_DEP) $(RPTADV_FFMPEG_BUILD_DEP) $(BUILD_CONFIG_STAMP) | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CHANNEL_CPPFLAGS) $(AGC_PLUGIN_CPPFLAGS) $(COMMON_CPPFLAGS) $(RADIO_CFLAGS) $(CFLAGS) $(WARNFLAGS) \
		-fPIC -DAST_MODULE='"chan_usbradioplus"' \
		-DAST_MODULE_SELF_SYM=__internal_chan_usbradioplus_self -c -o $@ $<

$(MODULE): $(RPCR_BUILD_DEP) $(RPTADV_RADIO_BUILD_DEP) $(RPTADV_SAMPLERATE_BUILD_DEP) $(RPTADV_FFMPEG_BUILD_DEP) $(ASTERISK_ADAPTER) $(MODULE_OBJECTS)
	@echo "Building $(PACKAGE) as a minimal Asterisk shim over the Rust adapter"
	$(CC) -shared $(LDFLAGS) -o $@ $(MODULE_OBJECTS) \
		-L$(BUILD_DIR) -lusbradioplus_asterisk \
		$(RPCR_LIBS) $(RPTADV_RADIO_LIBS) $(RPTADV_SAMPLERATE_LIBS) \
		$(RPTADV_FFMPEG_LIBS) $(RADIO_LIBS) -lm
	$(READELF) -d $@ | grep -F 'Shared library: [$(ASTERISK_ADAPTER_SONAME)]'
	$(READELF) -d $@ | grep -F 'Shared library: [librate_adjusting_pcm_ring2.so.2]'
	$(READELF) -d $@ | grep -F 'Shared library: [librptadvradio.so.4]'
	$(READELF) -d $@ | grep -F 'Shared library: [librptadv_samplerate_adapter.so.1]'
	$(READELF) -d $@ | grep -F 'Shared library: [librptadv_ffmpeg_adapter.so.1]'
	$(READELF) -d $@ | grep -F 'Shared library: [librptadv_portaudio_alsa_adapter.so.2]'
	$(READELF) -d $@ | grep -F 'Shared library: [librptadv_gpio_adapter.so.1]'
	$(READELF) -d $@ | grep -F 'Shared library: [librptadv_rnnoise_adapter.so.1]'
	@set -eu; undefined_symbols="$$($(NM) -D --undefined-only $@)"; \
		if printf '%s\n' "$$undefined_symbols" | grep -Eq '[[:space:]]ast_radio_'; then \
			echo 'USBRadioPlus must not import retired ast_radio hardware helpers' >&2; \
			exit 1; \
		fi

# Build each Rust artifact once so distinct linker flags cannot race through a
# shared Cargo target directory under parallel make.
$(RUST_BUILD_STAMP): Cargo.toml Cargo.lock rust-toolchain.toml $(RUST_SOURCES) $(BUILD_CONFIG_STAMP)
	CARGO_TARGET_DIR="$(CARGO_TARGET_DIR)" $(CARGO) build --release --locked -p usbradioplus-tune
	CARGO_TARGET_DIR="$(CARGO_TARGET_DIR)" \
		$(CARGO) rustc --release --locked -p usbradioplus-asterisk -- \
		-C link-arg=-Wl,-soname,$(ASTERISK_ADAPTER_SONAME)
	CARGO_TARGET_DIR="$(CARGO_TARGET_DIR)" \
		$(CARGO) rustc --release --locked -p usbradioplus_agc -- \
		-C link-arg=-Wl,-soname,$(AGC_PLUGIN_SONAME)
	touch $@

$(ASTERISK_ADAPTER_VERSIONED): $(RUST_BUILD_STAMP) | $(BUILD_DIR)
	cp $(RUST_ASTERISK_ADAPTER) $@
	$(READELF) -d $@ | grep -F '$(ASTERISK_ADAPTER_SONAME)'
	@! $(READELF) -d $@ | grep -E 'libstd-|RPATH|RUNPATH'

$(ASTERISK_ADAPTER): $(ASTERISK_ADAPTER_VERSIONED)
	ln -sf $(ASTERISK_ADAPTER_SONAME) $@

$(TUNER): $(RUST_BUILD_STAMP) | $(BUILD_DIR)
	cp $(RUST_TUNER) $@

$(AGC_PLUGIN_VERSIONED): $(RUST_BUILD_STAMP) | $(BUILD_DIR)
	cp $(RUST_AGC_LIBRARY) $@
	$(READELF) -d $@ | grep -F '$(AGC_PLUGIN_SONAME)'
	@! $(READELF) -d $@ | grep -E 'libstd-|RPATH|RUNPATH'

$(AGC_PLUGIN): $(AGC_PLUGIN_VERSIONED)
	ln -sf $(AGC_PLUGIN_SONAME) $@

check: all
	$(PYTHON) -m pytest -q tests_py
	CARGO_TARGET_DIR="$(CARGO_TARGET_DIR)" $(CARGO) test --workspace --all-targets --locked
	LD_LIBRARY_PATH="$(if $(strip $(RPCR_SOURCE)),$(RPCR_PREFIX)/lib:)$(if $(strip $(RPTADV_RADIO_SOURCE)),$(RPTADV_RADIO_LIBDIR):)$(if $(strip $(RPTADV_SAMPLERATE_SOURCE)),$(RPTADV_SAMPLERATE_LIBDIR):)$(if $(strip $(RPTADV_FFMPEG_SOURCE)),$(RPTADV_FFMPEG_LIBDIR):)$${LD_LIBRARY_PATH:-}" \
		RPCR_CFLAGS="$(RPCR_CFLAGS)" RPCR_LIBS="$(RPCR_LIBS)" \
		RPTADV_RADIO_CFLAGS="$(RPTADV_RADIO_CFLAGS)" RPTADV_RADIO_LIBS="$(RPTADV_RADIO_LIBS)" \
		RPTADV_SAMPLERATE_CFLAGS="$(RPTADV_SAMPLERATE_CFLAGS)" RPTADV_SAMPLERATE_LIBS="$(RPTADV_SAMPLERATE_LIBS)" \
		RPTADV_FFMPEG_CFLAGS="$(RPTADV_FFMPEG_CFLAGS)" RPTADV_FFMPEG_LIBS="$(RPTADV_FFMPEG_LIBS)" \
		sh ./tests/run_c_tests.sh
	$(MAKE) validate-release

validate-release:
	$(PYTHON) tools/validate_release.py

lint:
	$(CARGO) fmt --all --check
	$(RUFF) check tests_py tests_docs tools
	$(RUFF) format --check tests_py tests_docs tools
	$(CLANG_FORMAT) --dry-run --Werror \
		$(CHANNEL_SOURCE) $(ASTERISK_ADAPTER_HEADER) \
		tests/test_chan_usbradioplus_shim.c \
		$(shell find tests/fixtures/shim-host -type f -name '*.h') \
		tests/test_rms_agc_ladspa.c tests/fixtures/rms_agc_ladspa.h \
		tests/fixtures/rms_agc_reference.c
	$(SHELLCHECK) install.sh scripts/*.sh tests/*.sh \
		packaging/repository/install-usbradioplus.sh

# Cppcheck uses the complete deterministic host fixture; the compiler and
# Clang-Tidy separately validate the same shim against installed Asterisk headers.
# Asterisk owns callback signatures, so their mutable pointer parameters cannot
# be const-qualified even when this shim only reads them.
static-analysis: $(RPCR_BUILD_DEP) $(RPTADV_RADIO_BUILD_DEP) $(RPTADV_SAMPLERATE_BUILD_DEP) $(RPTADV_FFMPEG_BUILD_DEP)
	@set +e; \
	CARGO_TARGET_DIR="$(CARGO_TARGET_DIR)" $(CARGO) clippy --workspace --all-targets --all-features --locked -- -D warnings & rust_clippy_pid=$$!; \
	$(CPPCHECK) -j$(PARALLEL_JOBS) --std=c11 \
		--enable=warning,style,performance,portability \
		--error-exitcode=1 --inline-suppr --suppress=missingIncludeSystem \
		--suppress=normalCheckLevelMaxBranches --suppress=constParameterCallback \
		-Itests/fixtures/shim-host/include -Irust/asterisk/include \
		$(CHANNEL_SOURCE) & cppcheck_pid=$$!; \
	clang-tidy $(CHANNEL_SOURCE) \
		-- $(CHANNEL_CPPFLAGS) $(COMMON_CPPFLAGS) $(RADIO_CFLAGS) -std=gnu11 -fblocks \
		-DAST_MODULE='"chan_usbradioplus"' \
		-DAST_MODULE_SELF_SYM=__internal_chan_usbradioplus_self \
		& channel_tidy_pid=$$!; \
	status=0; \
	for pid in $$rust_clippy_pid $$cppcheck_pid $$channel_tidy_pid; do \
		wait $$pid || status=1; \
	done; \
	exit $$status

coverage: $(RPCR_BUILD_DEP) $(RPTADV_RADIO_BUILD_DEP) $(RPTADV_SAMPLERATE_BUILD_DEP) $(RPTADV_FFMPEG_BUILD_DEP)
	rm -rf $(BUILD_DIR)/coverage $(BUILD_DIR)/coverage-focus
	rm -f $(MODULE) $(AGC_PLUGIN) $(MODULE_OBJECTS)
	rm -f $(BUILD_DIR)/*.gcda $(BUILD_DIR)/*.gcno
	# Manual focused runs may place GCC counters at the repository root. Never
	# allow counters produced by another compiler/image to enter this report.
	rm -f ./*.gcda ./*.gcno
	# Focused diagnostics keep their temporary output under ignored .work. Remove
	# only GCC counters there so an aggregate report cannot import paths from a
	# different container or an earlier source copy.
	@if [ -d .work ]; then \
		find .work -type f \( -name '*.gcda' -o -name '*.gcno' \) -delete; \
	fi
	mkdir -p $(BUILD_DIR)/coverage
	$(PYTHON) -m pytest -q -n auto tests_py \
		--cov=tools --cov-branch --cov-fail-under=100 \
		--cov-report=term --cov-report=html:$(BUILD_DIR)/coverage/python \
		--cov-report=xml:$(BUILD_DIR)/coverage/python.xml
	C_TEST_CFLAGS="--coverage -O0 -g" \
		C_TEST_OUTPUT="$(CURDIR)/$(BUILD_DIR)/coverage/raw" \
		LD_LIBRARY_PATH="$(if $(strip $(RPCR_SOURCE)),$(RPCR_PREFIX)/lib:)$(if $(strip $(RPTADV_RADIO_SOURCE)),$(RPTADV_RADIO_LIBDIR):)$(if $(strip $(RPTADV_SAMPLERATE_SOURCE)),$(RPTADV_SAMPLERATE_LIBDIR):)$(if $(strip $(RPTADV_FFMPEG_SOURCE)),$(RPTADV_FFMPEG_LIBDIR):)$${LD_LIBRARY_PATH:-}" \
		RPCR_CFLAGS="$(RPCR_CFLAGS)" RPCR_LIBS="$(RPCR_LIBS)" \
		RPTADV_RADIO_CFLAGS="$(RPTADV_RADIO_CFLAGS)" RPTADV_RADIO_LIBS="$(RPTADV_RADIO_LIBS)" \
		RPTADV_SAMPLERATE_CFLAGS="$(RPTADV_SAMPLERATE_CFLAGS)" RPTADV_SAMPLERATE_LIBS="$(RPTADV_SAMPLERATE_LIBS)" \
		RPTADV_FFMPEG_CFLAGS="$(RPTADV_FFMPEG_CFLAGS)" RPTADV_FFMPEG_LIBS="$(RPTADV_FFMPEG_LIBS)" \
		sh ./tests/run_c_tests.sh
	$(MAKE) -j$(PARALLEL_JOBS) all CFLAGS="--coverage -O0 -g" LDFLAGS="--coverage"
	LD_LIBRARY_PATH="$(if $(strip $(RPCR_SOURCE)),$(RPCR_PREFIX)/lib:)$(if $(strip $(RPTADV_RADIO_SOURCE)),$(RPTADV_RADIO_LIBDIR):)$(if $(strip $(RPTADV_SAMPLERATE_SOURCE)),$(RPTADV_SAMPLERATE_LIBDIR):)$(if $(strip $(RPTADV_FFMPEG_SOURCE)),$(RPTADV_FFMPEG_LIBDIR):)$${LD_LIBRARY_PATH:-}" \
		sh ./tests/run_coverage_integration.sh
	# The smoke run validates real Asterisk startup. Measure coverage from the
	# deterministic harness alone because its host fixtures produce a different
	# GCC branch graph than the installed Asterisk headers.
	$(GCOVR) $(BUILD_DIR)/coverage/raw --root . \
		--object-directory $(BUILD_DIR)/coverage/raw \
		--filter 'src/chan_usbradioplus_shim\.c' \
		--exclude-unreachable-branches --exclude-throw-branches \
		--txt - \
		--html-details $(BUILD_DIR)/coverage/index.html \
		--xml $(BUILD_DIR)/coverage/coverage.xml \
		--fail-under-line 100 --fail-under-branch 100 --print-summary
	$(MAKE) rust-coverage

# Keep Rust and GCC counters separate. The validator uses LCOV source lines and
# merges JSON branches by source span so generic monomorphizations cannot create
# false gaps; dedicated test modules remain outside the production report.
rust-coverage:
	mkdir -p $(BUILD_DIR)/coverage/rust
	RUSTUP_TOOLCHAIN=$(COVERAGE_TOOLCHAIN) \
		CARGO_LLVM_COV_TARGET_DIR="$(abspath $(BUILD_DIR)/rust-coverage-target)" \
		$(CARGO_LLVM_COV) --workspace --all-targets --locked --branch --json \
		--output-path $(BUILD_DIR)/coverage/rust/coverage.json
	RUSTUP_TOOLCHAIN=$(COVERAGE_TOOLCHAIN) \
		CARGO_LLVM_COV_TARGET_DIR="$(abspath $(BUILD_DIR)/rust-coverage-target)" \
		$(CARGO_LLVM_COV) report --lcov \
		--output-path $(BUILD_DIR)/coverage/rust/coverage.lcov
	$(PYTHON) tools/validate_rust_coverage.py \
		$(BUILD_DIR)/coverage/rust/coverage.json \
		$(BUILD_DIR)/coverage/rust/coverage.lcov $(CURDIR)/rust

# Coverage already executes every Python and C test.  Follow it with the
# non-test release validator and a clean build/install from the tarball instead
# of running the identical suites two more times on every platform.
platform-verify:
	$(MAKE) coverage
	$(MAKE) validate-release
	$(MAKE) distcheck DISTCHECK_TEST_TARGET=

rustdoc:
	CARGO_TARGET_DIR="$(CARGO_TARGET_DIR)" RUSTDOCFLAGS="-D warnings" \
		$(CARGO) doc --workspace --no-deps --locked

docs: rustdoc $(RPCR_BUILD_DEP) $(RPTADV_RADIO_BUILD_DEP) $(RPTADV_SAMPLERATE_BUILD_DEP) $(RPTADV_FFMPEG_BUILD_DEP)
	mkdir -p $(BUILD_DIR)
	rm -f $(BUILD_DIR)/doxygen-warnings.log
	$(DOXYGEN) Doxyfile
	test ! -s $(BUILD_DIR)/doxygen-warnings.log
	$(PYTHON) -m pytest -q tests_docs

ci:
	$(MAKE) lint
	$(MAKE) static-analysis
	$(MAKE) platform-verify
	$(MAKE) docs

install: all
	$(INSTALL) -d $(DESTDIR)$(asteriskmoduledir) $(DESTDIR)$(USBRADIOPLUS_LIBDIR) \
		$(DESTDIR)$(agcplugindir) $(DESTDIR)$(sbindir) \
		$(DESTDIR)$(docdir) $(DESTDIR)$(mandir)/man5 \
		$(DESTDIR)$(mandir)/man7 $(DESTDIR)$(mandir)/man8 \
		$(DESTDIR)$(sysconfdir)/asterisk
	$(INSTALL_DATA) $(MODULE) $(DESTDIR)$(asteriskmoduledir)/chan_usbradioplus.so
	$(INSTALL_PROGRAM) $(ASTERISK_ADAPTER_VERSIONED) \
		$(DESTDIR)$(USBRADIOPLUS_LIBDIR)/$(ASTERISK_ADAPTER_SONAME)
	$(INSTALL_DATA) $(AGC_PLUGIN_VERSIONED) $(DESTDIR)$(agcplugindir)/$(AGC_PLUGIN_SONAME)
	ln -sf $(AGC_PLUGIN_SONAME) $(DESTDIR)$(agcplugindir)/usbradioplus_agc.so
	$(INSTALL_PROGRAM) $(TUNER) $(DESTDIR)$(sbindir)/usbradioplus-tune
	$(INSTALL_DATA) README.md $(DESTDIR)$(docdir)/
	$(INSTALL_DATA) CHANGELOG.md $(DESTDIR)$(docdir)/
	$(INSTALL_DATA) doc/native-radio.md $(DESTDIR)$(docdir)/
	$(INSTALL_DATA) doc/native-mode-retirement.md $(DESTDIR)$(docdir)/
	$(INSTALL_DATA) examples/usbradioplus.conf.sample $(DESTDIR)$(docdir)/
	$(INSTALL_DATA) doc/agc.md $(DESTDIR)$(docdir)/
	@if test ! -e $(DESTDIR)$(sysconfdir)/asterisk/usbradioplus.conf; then \
		$(INSTALL_DATA) examples/usbradioplus.conf.sample \
			$(DESTDIR)$(sysconfdir)/asterisk/usbradioplus.conf; \
	else \
		echo "Preserving existing usbradioplus.conf"; \
	fi
	$(INSTALL_DATA) man/usbradioplus.conf.5 $(DESTDIR)$(mandir)/man5/
	$(INSTALL_DATA) man/usbradioplus.7 $(DESTDIR)$(mandir)/man7/
	$(INSTALL_DATA) man/usbradioplus-tune.8 $(DESTDIR)$(mandir)/man8/

install-strip: install
	strip $(DESTDIR)$(asteriskmoduledir)/chan_usbradioplus.so
	strip $(DESTDIR)$(USBRADIOPLUS_LIBDIR)/$(ASTERISK_ADAPTER_SONAME)
	strip $(DESTDIR)$(agcplugindir)/$(AGC_PLUGIN_SONAME)
	strip $(DESTDIR)$(sbindir)/usbradioplus-tune

uninstall:
	rm -f $(DESTDIR)$(asteriskmoduledir)/chan_usbradioplus.so \
		$(DESTDIR)$(USBRADIOPLUS_LIBDIR)/$(ASTERISK_ADAPTER_SONAME) \
		$(DESTDIR)$(agcplugindir)/usbradioplus_agc.so \
		$(DESTDIR)$(agcplugindir)/$(AGC_PLUGIN_SONAME) \
		$(DESTDIR)$(sbindir)/usbradioplus-tune \
		$(DESTDIR)$(mandir)/man5/usbradioplus.conf.5 \
		$(DESTDIR)$(mandir)/man7/usbradioplus.7 \
		$(DESTDIR)$(mandir)/man8/usbradioplus-tune.8 \
		$(DESTDIR)$(docdir)/README.md \
		$(DESTDIR)$(docdir)/CHANGELOG.md \
		$(DESTDIR)$(docdir)/native-radio.md \
		$(DESTDIR)$(docdir)/native-mode-retirement.md \
		$(DESTDIR)$(docdir)/usbradioplus.conf.sample \
		$(DESTDIR)$(docdir)/agc.md

dist: $(TARBALL)

$(TARBALL): $(DIST_FILES)
	rm -rf $(BUILD_DIR)/$(DISTNAME)
	mkdir -p $(BUILD_DIR)/$(DISTNAME) $(DIST_DIR)
	cp -a $(DIST_TOP) $(DIST_DIRS) $(BUILD_DIR)/$(DISTNAME)/
	# Copying source directories deliberately retains ordinary source metadata;
	# remove only generated files so a locally exercised tree cannot contaminate
	# an upstream archive or the release package built from it.
	find $(BUILD_DIR)/$(DISTNAME) -type d \
		\( -name .git -o -name __pycache__ -o -name .pytest_cache -o -name .ruff_cache \
			-o -name .coverage -o -name '.coverage-*' -o -name build -o -name dist \
			-o -name work -o -name outputs -o -name target \) -prune -exec rm -rf {} +
	find $(BUILD_DIR)/$(DISTNAME) -type f \
		\( -name '*.pyc' -o -name '*.gcda' -o -name '*.gcno' -o -name '*.gcov' \
			-o -name '*.profraw' -o -name '*.profdata' \
			-o -name '*.cap' -o -name '*.raw' -o -name '*.wav' -o -name '*.au' \) -delete
	find $(BUILD_DIR)/$(DISTNAME) -type d -exec chmod 0755 {} +
	find $(BUILD_DIR)/$(DISTNAME) -type f -exec chmod 0644 {} +
	chmod 0755 $(BUILD_DIR)/$(DISTNAME)/scripts/* \
		$(BUILD_DIR)/$(DISTNAME)/install.sh \
		$(BUILD_DIR)/$(DISTNAME)/tests/run_c_tests.sh \
		$(BUILD_DIR)/$(DISTNAME)/tests/run-in-quality-container.sh \
		$(BUILD_DIR)/$(DISTNAME)/tests/container-smoke-test.sh
	$(TAR) --sort=name --mtime=@$(SOURCE_DATE_EPOCH) --owner=0 --group=0 \
		--numeric-owner -C $(BUILD_DIR) -cJf $@ $(DISTNAME)

DISTCHECK_TEST_TARGET ?= check

# Release-tree checks must link the staged shared ring directly, not rebuild an
# externally checked-out source tree that may be read-only.  Keep unrelated
# pkg-config dependencies on their normal host paths.
ifneq ($(strip $(RPCR_SOURCE)),)
DIST_RPCR_ARGS := 'RPCR_CFLAGS=-I$(RPCR_PREFIX)/include' \
	'RPCR_LIBS=-L$(RPCR_PREFIX)/lib -lrate_adjusting_pcm_ring2'
DIST_RPCR_ENV = export LD_LIBRARY_PATH="$(RPCR_PREFIX)/lib$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}"; \
	unset RPCR_SOURCE RPCR_STAGE;
else
DIST_RPCR_ARGS :=
DIST_RPCR_ENV = :;
endif

# Release-tree checks use the staged radio core in exactly the same way as the
# staged ring.  An unpacked source archive must never depend on a writable
# external checkout, and the emitted module must retain its dynamic SONAME.
ifneq ($(strip $(RPTADV_RADIO_SOURCE)),)
DIST_RPTADV_RADIO_ARGS := 'RPTADV_RADIO_CFLAGS=-I$(RPTADV_RADIO_PREFIX)/include' \
	'RPTADV_RADIO_LIBS=$(RPTADV_RADIO_LIBRARY)'
DIST_RPTADV_RADIO_ENV = export LD_LIBRARY_PATH="$(RPTADV_RADIO_LIBDIR)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}"; \
	export PKG_CONFIG_PATH="$(RPTADV_RADIO_LIBDIR)/pkgconfig$${PKG_CONFIG_PATH:+:$$PKG_CONFIG_PATH}"; \
	unset RPTADV_RADIO_SOURCE RPTADV_RADIO_STAGE;
else
DIST_RPTADV_RADIO_ARGS :=
DIST_RPTADV_RADIO_ENV = :;
endif

# Release-tree checks use the staged sample-rate adapter only as an installed
# shared object. The unpacked source must not rebuild or vendor that adapter.
ifneq ($(strip $(RPTADV_SAMPLERATE_SOURCE)),)
DIST_RPTADV_SAMPLERATE_ARGS := \
	'RPTADV_SAMPLERATE_CFLAGS=-I$(RPTADV_SAMPLERATE_PREFIX)/include' \
	'RPTADV_SAMPLERATE_LIBS=-L$(RPTADV_SAMPLERATE_LIBDIR) -lrptadv_samplerate_adapter'
DIST_RPTADV_SAMPLERATE_ENV = export LD_LIBRARY_PATH="$(RPTADV_SAMPLERATE_LIBDIR)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}"; \
	unset RPTADV_SAMPLERATE_SOURCE RPTADV_SAMPLERATE_STAGE;
else
DIST_RPTADV_SAMPLERATE_ARGS :=
DIST_RPTADV_SAMPLERATE_ENV = :;
endif

ifneq ($(strip $(RPTADV_FFMPEG_SOURCE)),)
DIST_RPTADV_FFMPEG_ARGS := \
	'RPTADV_FFMPEG_CFLAGS=-I$(RPTADV_FFMPEG_PREFIX)/include' \
	'RPTADV_FFMPEG_LIBS=-L$(RPTADV_FFMPEG_LIBDIR) -lrptadv_ffmpeg_adapter'
DIST_RPTADV_FFMPEG_ENV = export LD_LIBRARY_PATH="$(RPTADV_FFMPEG_LIBDIR)$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}"; \
	unset RPTADV_FFMPEG_SOURCE RPTADV_FFMPEG_STAGE;
else
DIST_RPTADV_FFMPEG_ARGS :=
DIST_RPTADV_FFMPEG_ENV = :;
endif

distcheck: dist $(RPCR_BUILD_DEP) $(RPTADV_RADIO_BUILD_DEP) $(RPTADV_SAMPLERATE_BUILD_DEP) $(RPTADV_FFMPEG_BUILD_DEP)
	set -eu; tmp=$$(mktemp -d "$(CURDIR)/build/distcheck.XXXXXX"); \
		trap 'rm -rf "$$tmp"' EXIT; \
		$(TAR) -C "$$tmp" -xf $(TARBALL); \
		$(DIST_RPCR_ENV) \
		$(DIST_RPTADV_RADIO_ENV) \
		$(DIST_RPTADV_SAMPLERATE_ENV) \
		$(DIST_RPTADV_FFMPEG_ENV) \
		if test -n "$(DISTCHECK_TEST_TARGET)"; then \
			$(MAKE) -C "$$tmp/$(DISTNAME)" $(DIST_RPCR_ARGS) $(DIST_RPTADV_RADIO_ARGS) $(DIST_RPTADV_SAMPLERATE_ARGS) $(DIST_RPTADV_FFMPEG_ARGS) $(DISTCHECK_TEST_TARGET); \
		fi; \
		$(MAKE) -j$(PARALLEL_JOBS) -C "$$tmp/$(DISTNAME)" $(DIST_RPCR_ARGS) $(DIST_RPTADV_RADIO_ARGS) $(DIST_RPTADV_SAMPLERATE_ARGS) $(DIST_RPTADV_FFMPEG_ARGS) \
			DESTDIR="$$tmp/stage" prefix=/usr install

install-from-dist: dist $(RPCR_BUILD_DEP) $(RPTADV_RADIO_BUILD_DEP) $(RPTADV_SAMPLERATE_BUILD_DEP) $(RPTADV_FFMPEG_BUILD_DEP)
	set -eu; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
		$(TAR) -C "$$tmp" -xf $(TARBALL); \
		$(DIST_RPCR_ENV) \
		$(DIST_RPTADV_RADIO_ENV) \
		$(DIST_RPTADV_SAMPLERATE_ENV) \
		$(DIST_RPTADV_FFMPEG_ENV) \
		$(MAKE) -C "$$tmp/$(DISTNAME)" $(DIST_RPCR_ARGS) $(DIST_RPTADV_RADIO_ARGS) $(DIST_RPTADV_SAMPLERATE_ARGS) $(DIST_RPTADV_FFMPEG_ARGS) all; \
		$(MAKE) -C "$$tmp/$(DISTNAME)" $(DIST_RPCR_ARGS) $(DIST_RPTADV_RADIO_ARGS) $(DIST_RPTADV_SAMPLERATE_ARGS) $(DIST_RPTADV_FFMPEG_ARGS) DESTDIR="$(DESTDIR)" prefix="$(prefix)" \
			asteriskmoduledir="$(asteriskmoduledir)" install

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)
