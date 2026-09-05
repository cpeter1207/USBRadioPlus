# syntax=docker/dockerfile:1.7
ARG DEBIAN_VERSION=12
FROM debian:${DEBIAN_VERSION} AS asl3-clean

ARG DEBIAN_VERSION
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates wget && \
    wget -qO /tmp/asl-repo.deb \
      "https://repo.allstarlink.org/public/asl-apt-repos.deb${DEBIAN_VERSION}_all.deb" && \
    dpkg -i /tmp/asl-repo.deb && \
    apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      asl3-asterisk && \
    rm -rf /var/lib/apt/lists/* /tmp/asl-repo.deb

FROM asl3-clean AS quality
ARG DEBIAN_VERSION
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      asl3-asterisk-dev build-essential clang clang-format clang-tidy cppcheck doxygen git graphviz \
      gcovr pkg-config python3 python3-pytest python3-venv shellcheck \
      libasound2-dev libusb-dev libusb-1.0-0-dev libsamplerate0-dev \
      libavfilter-dev libavutil-dev portaudio19-dev autoconf automake libtool \
      patch xz-utils && \
    rm -rf /var/lib/apt/lists/* && \
    python3 -m venv /opt/usbradioplus-quality && \
    /opt/usbradioplus-quality/bin/pip install --no-cache-dir \
      coverage==7.10.6 pytest==8.4.2 pytest-cov==7.0.0 pytest-xdist==3.8.0 ruff==0.16.5

ENV PATH="/opt/usbradioplus-quality/bin:${PATH}"

COPY scripts/install-rnnoise.sh /tmp/usbradioplus/scripts/
COPY packaging/rnnoise/debian/patches/ \
  /tmp/usbradioplus/packaging/rnnoise/debian/patches/
RUN sh /tmp/usbradioplus/scripts/install-rnnoise.sh && rm -rf /tmp/usbradioplus

WORKDIR /usr/src/usbradioplus

FROM quality AS staged
COPY . .
RUN make clean all && make DESTDIR=/stage prefix=/usr install

FROM asl3-clean AS usbradioplus-installed
ARG DEBIAN_VERSION
ARG RUN_SMOKE_TEST=0
RUN if [ "$DEBIAN_VERSION" = 12 ]; then av_packages="libavfilter8 libavutil57"; \
    else av_packages="libavfilter10 libavutil59"; fi && \
    apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libasound2 $av_packages libportaudio2 libsamplerate0 \
      libusb-0.1-4 libusb-1.0-0 python3 whiptail && \
    rm -rf /var/lib/apt/lists/*
COPY --from=quality /usr/local/lib/librnnoise.so.0.4.1 /usr/local/lib/
COPY --from=staged /stage/ /
COPY --chmod=0755 tests/container-smoke-test.sh /usr/local/libexec/usbradioplus/
RUN ln -s librnnoise.so.0.4.1 /usr/local/lib/librnnoise.so.0 && \
    ln -s librnnoise.so.0 /usr/local/lib/librnnoise.so && \
    ldconfig && \
	 test "$(find /usr/lib -path '*/asterisk/modules/chan_usbradioplus.so' -print -quit)" != "" && \
	 test -x /usr/sbin/usbradioplus-tune && \
	 ldd "$(find /usr/lib -path '*/asterisk/modules/chan_usbradioplus.so' -print -quit)" | \
      tee /tmp/module-libraries && \
    ! grep -q 'not found' /tmp/module-libraries && \
    if [ "$RUN_SMOKE_TEST" = 1 ]; then \
      /usr/local/libexec/usbradioplus/container-smoke-test.sh; \
    fi
