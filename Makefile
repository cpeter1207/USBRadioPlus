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

CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=
WARNFLAGS ?= -Wall -Wextra -Werror -Wno-old-style-declaration
ASTERISK_INCLUDEDIR ?= /usr/include
BUILD_DIR ?= build
DIST_DIR ?= dist
PARALLEL_JOBS ?= $(strip $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2))
SOURCE_DATE_EPOCH ?= $(shell git log -1 --format=%ct 2>/dev/null || echo 0)

ASL_RADIO_API ?= $(strip $(shell \
	grep -q ast_radio_device_acquire \
		$(ASTERISK_INCLUDEDIR)/asterisk/res_usbradio.h 2>/dev/null \
		&& echo modern || echo legacy))

DSP_PACKAGES := rnnoise samplerate libavfilter libavutil alsa
DSP_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(DSP_PACKAGES))
DSP_LIBS := $(shell $(PKG_CONFIG) --libs $(DSP_PACKAGES))
ifeq ($(ASL_RADIO_API),modern)
CHANNEL_SOURCE := src/chan_usbradioplus_modern.c
CHANNEL_CPPFLAGS := -DURP_CHANNEL_MODERN
RADIO_PACKAGES := portaudio-2.0 libusb-1.0
RADIO_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(RADIO_PACKAGES))
RADIO_LIBS := $(shell $(PKG_CONFIG) --libs $(RADIO_PACKAGES))
else ifeq ($(ASL_RADIO_API),legacy)
CHANNEL_SOURCE := src/chan_usbradioplus.c
CHANNEL_CPPFLAGS :=
RADIO_CFLAGS :=
RADIO_LIBS := -lusb
else
$(error ASL_RADIO_API must be legacy or modern)
endif
COMMON_CPPFLAGS := -I$(ASTERISK_INCLUDEDIR) -Isrc
MODULE := $(BUILD_DIR)/chan_usbradioplus.so
AGC_PLUGIN := $(BUILD_DIR)/usbradioplus_agc.so
AGC_PLUGIN_CPPFLAGS := -DURP_AGC_PLUGIN_PATH='"$(agcplugindir)/usbradioplus_agc.so"'
TARBALL := $(DIST_DIR)/$(DISTNAME).tar.xz

SHARED_SOURCES := src/usbradioplus_config.c src/usbradioplus_radio.c \
	src/usbradioplus_dsp.c src/usbradioplus_ctcss.c src/usbradioplus_hardware.c \
	src/usbradioplus_repeat.c src/usbradioplus_channel_core.c \
	src/usbradioplus_channel_common.c src/usbradioplus_native_tick.c \
	src/usbradioplus_tune_menu.c \
	src/usbradioplus_processing.c src/txagc/agc_core.c \
	src/txagc/avfilter_processor.c src/txagc/rnnoise_processor.c
CHANNEL_OBJECT := $(BUILD_DIR)/$(patsubst src/%.c,%.o,$(CHANNEL_SOURCE))
SHARED_OBJECTS := $(patsubst src/%.c,$(BUILD_DIR)/%.o,$(SHARED_SOURCES))
MODULE_OBJECTS := $(CHANNEL_OBJECT) $(SHARED_OBJECTS)

MODULE_SOURCES := $(wildcard src/*.c src/*.h src/txagc/*)
DIST_TOP := Makefile VERSION CHANGELOG.md COPYING README.md INSTALL.md \
	RELEASE-CHECKLIST.md CONTRIBUTING.md AGENTS.md Doxyfile pyproject.toml \
	.clang-format .clang-tidy .dockerignore install.sh
DIST_DIRS := .github containers debian packaging src scripts examples man doc tests tests_py tests_docs tools
DIST_FILES := $(DIST_TOP) $(shell find $(DIST_DIRS) -type f \
	! -name '*.pyc' ! -path '*/__pycache__/*' | LC_ALL=C sort)

.PHONY: all check ci coverage docs lint static-analysis platform-verify \
	clean dist distcheck install install-strip install-from-dist \
	print-asl-radio-api uninstall validate-release

.PHONY: force-agc-path

print-asl-radio-api:
	@echo $(ASL_RADIO_API)

all: $(MODULE) $(AGC_PLUGIN)

$(BUILD_DIR):
	mkdir -p $@

# A later staged install may select a different prefix from the initial build.
# Track it so the module cannot retain a stale private-plugin location.
$(BUILD_DIR)/agc-plugin-path: force-agc-path | $(BUILD_DIR)
	@printf '%s\n' '$(agcplugindir)/usbradioplus_agc.so' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

$(BUILD_DIR)/txagc/avfilter_processor.o: $(BUILD_DIR)/agc-plugin-path

$(BUILD_DIR)/%.o: src/%.c $(MODULE_SOURCES) | $(BUILD_DIR)
	mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CHANNEL_CPPFLAGS) $(AGC_PLUGIN_CPPFLAGS) $(COMMON_CPPFLAGS) $(DSP_CFLAGS) $(RADIO_CFLAGS) $(CFLAGS) $(WARNFLAGS) \
		-fPIC -DAST_MODULE='"chan_usbradioplus"' \
		-DAST_MODULE_SELF_SYM=__internal_chan_usbradioplus_self -c -o $@ $<

$(MODULE): $(MODULE_OBJECTS)
	@echo "Building $(PACKAGE) for the $(ASL_RADIO_API) ASL3 radio API"
	$(CC) -shared $(LDFLAGS) -o $@ $(MODULE_OBJECTS) \
		$(DSP_LIBS) $(RADIO_LIBS) -lm

# The graph loads this private LADSPA effect; it is not an Asterisk module.
$(AGC_PLUGIN): src/txagc/rms_agc_ladspa.c src/txagc/rms_agc_ladspa.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNFLAGS) -fPIC -shared $(LDFLAGS) \
		-o $@ src/txagc/rms_agc_ladspa.c -lm

check: all
	$(PYTHON) -m pytest -q tests_py
	sh ./tests/run_c_tests.sh
	$(MAKE) validate-release

validate-release:
	$(PYTHON) tools/validate_release.py

lint:
	$(RUFF) check scripts tests_py tests_docs tools
	$(RUFF) format --check scripts/usbradioplus-tune tests_py tests_docs tools
	$(CLANG_FORMAT) --dry-run --Werror \
		$(wildcard src/*.c src/*.h src/*.inc src/txagc/*.c src/txagc/*.h tests/*.c)
	$(SHELLCHECK) install.sh scripts/*.sh tests/*.sh \
		packaging/repository/install-usbradioplus.sh

static-analysis:
	@set +e; \
	$(CPPCHECK) -j$(PARALLEL_JOBS) --std=c11 \
		--enable=warning,style,performance,portability \
		--error-exitcode=1 --inline-suppr --suppress=missingIncludeSystem \
		--suppress=normalCheckLevelMaxBranches \
		--suppress=syntaxError:src/chan_usbradioplus.c \
		--suppress=syntaxError:src/chan_usbradioplus_modern.c \
		-Isrc src & cppcheck_pid=$$!; \
	clang-tidy $(CHANNEL_SOURCE) \
		-- $(COMMON_CPPFLAGS) $(DSP_CFLAGS) $(RADIO_CFLAGS) -std=gnu11 -fblocks \
		-DAST_MODULE='"chan_usbradioplus"' \
		-DAST_MODULE_SELF_SYM=__internal_chan_usbradioplus_self \
		& channel_tidy_pid=$$!; \
	clang-tidy src/usbradioplus_ctcss.c src/usbradioplus_dsp.c \
		src/usbradioplus_hardware.c src/usbradioplus_repeat.c \
		src/usbradioplus_channel_core.c \
		src/txagc/agc_core.c src/txagc/avfilter_processor.c \
		src/txagc/rms_agc_ladspa.c \
		src/txagc/rnnoise_processor.c \
		-- $(COMMON_CPPFLAGS) $(DSP_CFLAGS) -std=gnu11 \
		& shared_tidy_pid=$$!; \
	status=0; \
	for pid in $$cppcheck_pid $$channel_tidy_pid $$shared_tidy_pid; do \
		wait $$pid || status=1; \
	done; \
	exit $$status

coverage:
	rm -rf $(BUILD_DIR)/coverage $(BUILD_DIR)/coverage-focus
	rm -f $(MODULE) $(AGC_PLUGIN) $(SHARED_OBJECTS) $(CHANNEL_OBJECT)
	rm -f $(BUILD_DIR)/*.gcda $(BUILD_DIR)/*.gcno
	# Manual focused runs may place GCC counters at the repository root. Never
	# allow counters produced by another compiler/image to enter this report.
	rm -f ./*.gcda ./*.gcno
	mkdir -p $(BUILD_DIR)/coverage
	$(PYTHON) -m pytest -q -n auto tests_py \
		--cov=scripts --cov=tools --cov-branch --cov-fail-under=100 \
		--cov-report=term --cov-report=html:$(BUILD_DIR)/coverage/python \
		--cov-report=xml:$(BUILD_DIR)/coverage/python.xml
	C_TEST_CFLAGS="--coverage -O0 -g" \
		C_TEST_OUTPUT="$(CURDIR)/$(BUILD_DIR)/coverage/raw" \
		sh ./tests/run_c_tests.sh
	$(MAKE) -j$(PARALLEL_JOBS) all CFLAGS="--coverage -O0 -g" LDFLAGS="--coverage"
	sh ./tests/run_coverage_integration.sh
	# Keep the real-module smoke test mandatory, while using the channel harness
	# counters for the adapter source compiled with explicit test interfaces.
	rm -f $(CHANNEL_OBJECT:.o=.gcda) $(CHANNEL_OBJECT:.o=.gcno)
	$(GCOVR) --root . --filter 'src/.*\.c' \
		--exclude-unreachable-branches --exclude-throw-branches \
		--txt - \
		--html-details $(BUILD_DIR)/coverage/index.html \
		--xml $(BUILD_DIR)/coverage/coverage.xml \
		--fail-under-line 100 --fail-under-branch 100 --print-summary

# Coverage already executes every Python and C test.  Follow it with the
# non-test release validator and a clean build/install from the tarball instead
# of running the identical suites two more times on every platform.
platform-verify:
	$(MAKE) coverage
	$(MAKE) validate-release
	$(MAKE) distcheck DISTCHECK_TEST_TARGET=

docs:
	mkdir -p $(BUILD_DIR)
	rm -f $(BUILD_DIR)/doxygen-warnings.log
	$(DOXYGEN) Doxyfile
	test ! -s $(BUILD_DIR)/doxygen-warnings.log
	$(PYTHON) -m pytest -q tests_docs

ci: lint static-analysis check coverage docs distcheck

install: all
	$(INSTALL) -d $(DESTDIR)$(asteriskmoduledir) $(DESTDIR)$(agcplugindir) $(DESTDIR)$(sbindir) \
		$(DESTDIR)$(docdir) $(DESTDIR)$(mandir)/man5 \
		$(DESTDIR)$(mandir)/man7 $(DESTDIR)$(mandir)/man8 \
		$(DESTDIR)$(sysconfdir)/asterisk
	$(INSTALL_DATA) $(MODULE) $(DESTDIR)$(asteriskmoduledir)/chan_usbradioplus.so
	$(INSTALL_DATA) $(AGC_PLUGIN) $(DESTDIR)$(agcplugindir)/usbradioplus_agc.so
	$(INSTALL_PROGRAM) scripts/usbradioplus-tune $(DESTDIR)$(sbindir)/usbradioplus-tune
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
	strip $(DESTDIR)$(agcplugindir)/usbradioplus_agc.so

uninstall:
	rm -f $(DESTDIR)$(asteriskmoduledir)/chan_usbradioplus.so \
		$(DESTDIR)$(agcplugindir)/usbradioplus_agc.so \
		$(DESTDIR)$(sbindir)/usbradioplus-tune \
		$(DESTDIR)$(mandir)/man5/usbradioplus.conf.5 \
		$(DESTDIR)$(mandir)/man7/usbradioplus.7 \
		$(DESTDIR)$(mandir)/man8/usbradioplus-tune.8 \
		$(DESTDIR)$(docdir)/usbradioplus.conf.sample \
		$(DESTDIR)$(docdir)/agc.md

dist: $(TARBALL)

$(TARBALL): $(DIST_FILES)
	rm -rf $(BUILD_DIR)/$(DISTNAME)
	mkdir -p $(BUILD_DIR)/$(DISTNAME) $(DIST_DIR)
	cp -a $(DIST_TOP) $(DIST_DIRS) $(BUILD_DIR)/$(DISTNAME)/
	rm -rf $(BUILD_DIR)/$(DISTNAME)/tests_py/__pycache__
	find $(BUILD_DIR)/$(DISTNAME) -type d -exec chmod 0755 {} +
	find $(BUILD_DIR)/$(DISTNAME) -type f -exec chmod 0644 {} +
	chmod 0755 $(BUILD_DIR)/$(DISTNAME)/scripts/* \
		$(BUILD_DIR)/$(DISTNAME)/install.sh \
		$(BUILD_DIR)/$(DISTNAME)/tests/run_c_tests.sh \
		$(BUILD_DIR)/$(DISTNAME)/tests/run-in-quality-container.sh \
		$(BUILD_DIR)/$(DISTNAME)/tests/container-smoke-test.sh \
		$(BUILD_DIR)/$(DISTNAME)/tests/fixtures/asterisk-dev/fake-cc
	$(TAR) --sort=name --mtime=@$(SOURCE_DATE_EPOCH) --owner=0 --group=0 \
		--numeric-owner -C $(BUILD_DIR) -cJf $@ $(DISTNAME)

DISTCHECK_TEST_TARGET ?= check

distcheck: dist
	set -eu; tmp=$$(mktemp -d "$(CURDIR)/build/distcheck.XXXXXX"); \
		trap 'rm -rf "$$tmp"' EXIT; \
		$(TAR) -C "$$tmp" -xf $(TARBALL); \
		if test -n "$(DISTCHECK_TEST_TARGET)"; then \
			$(MAKE) -C "$$tmp/$(DISTNAME)" $(DISTCHECK_TEST_TARGET); \
		fi; \
		$(MAKE) -j$(PARALLEL_JOBS) -C "$$tmp/$(DISTNAME)" \
			DESTDIR="$$tmp/stage" prefix=/usr install

install-from-dist: dist
	set -eu; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
		$(TAR) -C "$$tmp" -xf $(TARBALL); \
		$(MAKE) -C "$$tmp/$(DISTNAME)" all; \
		$(MAKE) -C "$$tmp/$(DISTNAME)" DESTDIR="$(DESTDIR)" prefix="$(prefix)" \
			asteriskmoduledir="$(asteriskmoduledir)" install

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)
