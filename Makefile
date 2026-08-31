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
MULTIARCH ?= $(strip $(shell dpkg-architecture -qDEB_HOST_MULTIARCH 2>/dev/null))
asteriskmoduledir ?= $(libdir)$(if $(MULTIARCH),/$(MULTIARCH))/asterisk/modules
DESTDIR ?=

CC ?= cc
PKG_CONFIG ?= pkg-config
INSTALL ?= install
INSTALL_PROGRAM ?= $(INSTALL)
INSTALL_DATA ?= $(INSTALL) -m 0644
PYTHON ?= python3
TAR ?= tar

CPPFLAGS ?=
CFLAGS ?= -O2 -g
LDFLAGS ?=
WARNFLAGS ?= -Wall -Wextra -Werror -Wno-old-style-declaration
ASTERISK_INCLUDEDIR ?= /usr/include
BUILD_DIR ?= build
DIST_DIR ?= dist
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
RADIO_PACKAGES := portaudio-2.0 libusb-1.0
RADIO_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(RADIO_PACKAGES))
RADIO_LIBS := $(shell $(PKG_CONFIG) --libs $(RADIO_PACKAGES))
else ifeq ($(ASL_RADIO_API),legacy)
CHANNEL_SOURCE := src/chan_usbradioplus.c
RADIO_CFLAGS :=
RADIO_LIBS := -lusb
else
$(error ASL_RADIO_API must be legacy or modern)
endif
COMMON_CPPFLAGS := -I$(ASTERISK_INCLUDEDIR) -Isrc
MODULE := $(BUILD_DIR)/chan_usbradioplus.so
TUNE := $(BUILD_DIR)/usbradioplus-tune
TARBALL := $(DIST_DIR)/$(DISTNAME).tar.xz

MODULE_SOURCES := $(wildcard src/*.c src/*.h src/txagc/*)
DIST_TOP := Makefile VERSION CHANGELOG.md COPYING README.md INSTALL.md RELEASE-CHECKLIST.md install.sh
DIST_DIRS := .github debian packaging src scripts examples man doc tests tests_py tools
DIST_FILES := $(DIST_TOP) $(shell find $(DIST_DIRS) -type f \
	! -name '*.pyc' ! -path '*/__pycache__/*' | LC_ALL=C sort)

.PHONY: all check clean dist distcheck install install-strip install-from-dist \
	print-asl-radio-api uninstall

print-asl-radio-api:
	@echo $(ASL_RADIO_API)

all: $(MODULE) $(TUNE)

$(BUILD_DIR):
	mkdir -p $@

$(MODULE): $(MODULE_SOURCES) | $(BUILD_DIR)
	@echo "Building $(PACKAGE) for the $(ASL_RADIO_API) ASL3 radio API"
	$(CC) $(CPPFLAGS) $(COMMON_CPPFLAGS) $(DSP_CFLAGS) $(RADIO_CFLAGS) $(CFLAGS) $(WARNFLAGS) \
		-fPIC -DAST_MODULE='"chan_usbradioplus"' \
		-DAST_MODULE_SELF_SYM=__internal_chan_usbradioplus_self \
		-shared $(LDFLAGS) -o $@ $(CHANNEL_SOURCE) $(DSP_LIBS) $(RADIO_LIBS) -lm

$(TUNE): src/usbradioplus-tune.c | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(COMMON_CPPFLAGS) $(CFLAGS) $(WARNFLAGS) \
		-DAST_MODULE_SELF_SYM=__internal_usbradioplus_tune_self \
		$(LDFLAGS) -o $@ $<

check: all
	$(PYTHON) -m pytest -q tests_py
	sh ./tests/run_c_tests.sh
	$(PYTHON) tools/validate_release.py

install: all
	$(INSTALL) -d $(DESTDIR)$(asteriskmoduledir) $(DESTDIR)$(sbindir) \
		$(DESTDIR)$(docdir) $(DESTDIR)$(mandir)/man5 \
		$(DESTDIR)$(mandir)/man7 $(DESTDIR)$(mandir)/man8
	$(INSTALL_DATA) $(MODULE) $(DESTDIR)$(asteriskmoduledir)/chan_usbradioplus.so
	$(INSTALL_PROGRAM) $(TUNE) $(DESTDIR)$(sbindir)/usbradioplus-tune
	$(INSTALL_PROGRAM) scripts/usbradioplus-rxlevel $(DESTDIR)$(sbindir)/usbradioplus-rxlevel
	$(INSTALL_PROGRAM) scripts/usbradioplus-processing-tune $(DESTDIR)$(sbindir)/usbradioplus-processing-tune
	$(INSTALL_DATA) examples/usbradioplus.conf.sample $(DESTDIR)$(docdir)/
	$(INSTALL_DATA) examples/usbradioplus-processing.conf.sample $(DESTDIR)$(docdir)/
	$(INSTALL_DATA) man/usbradioplus.conf.5 $(DESTDIR)$(mandir)/man5/
	$(INSTALL_DATA) man/usbradioplus-processing.conf.5 $(DESTDIR)$(mandir)/man5/
	$(INSTALL_DATA) man/usbradioplus.7 $(DESTDIR)$(mandir)/man7/
	$(INSTALL_DATA) man/usbradioplus-tune.8 $(DESTDIR)$(mandir)/man8/
	ln -sf usbradioplus-tune.8 $(DESTDIR)$(mandir)/man8/usbradioplus-rxlevel.8
	ln -sf usbradioplus-tune.8 $(DESTDIR)$(mandir)/man8/usbradioplus-processing-tune.8

install-strip: install
	strip $(DESTDIR)$(asteriskmoduledir)/chan_usbradioplus.so \
		$(DESTDIR)$(sbindir)/usbradioplus-tune

uninstall:
	rm -f $(DESTDIR)$(asteriskmoduledir)/chan_usbradioplus.so \
		$(DESTDIR)$(sbindir)/usbradioplus-tune \
		$(DESTDIR)$(sbindir)/usbradioplus-rxlevel \
		$(DESTDIR)$(sbindir)/usbradioplus-processing-tune \
		$(DESTDIR)$(mandir)/man5/usbradioplus.conf.5 \
		$(DESTDIR)$(mandir)/man5/usbradioplus-processing.conf.5 \
		$(DESTDIR)$(mandir)/man7/usbradioplus.7 \
		$(DESTDIR)$(mandir)/man8/usbradioplus-tune.8 \
		$(DESTDIR)$(mandir)/man8/usbradioplus-rxlevel.8 \
		$(DESTDIR)$(mandir)/man8/usbradioplus-processing-tune.8 \
		$(DESTDIR)$(docdir)/usbradioplus.conf.sample \
		$(DESTDIR)$(docdir)/usbradioplus-processing.conf.sample

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
		$(BUILD_DIR)/$(DISTNAME)/tests/fixtures/asterisk-dev/fake-cc
	$(TAR) --sort=name --mtime=@$(SOURCE_DATE_EPOCH) --owner=0 --group=0 \
		--numeric-owner -C $(BUILD_DIR) -cJf $@ $(DISTNAME)

distcheck: dist
	set -eu; tmp=$$(mktemp -d "$(CURDIR)/build/distcheck.XXXXXX"); \
		trap 'rm -rf "$$tmp"' EXIT; \
		$(TAR) -C "$$tmp" -xf $(TARBALL); \
		$(MAKE) -C "$$tmp/$(DISTNAME)" check; \
		$(MAKE) -C "$$tmp/$(DISTNAME)" DESTDIR="$$tmp/stage" prefix=/usr install

install-from-dist: dist
	set -eu; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
		$(TAR) -C "$$tmp" -xf $(TARBALL); \
		$(MAKE) -C "$$tmp/$(DISTNAME)" all; \
		$(MAKE) -C "$$tmp/$(DISTNAME)" DESTDIR="$(DESTDIR)" prefix="$(prefix)" \
			asteriskmoduledir="$(asteriskmoduledir)" install

clean:
	rm -rf $(BUILD_DIR) $(DIST_DIR)
