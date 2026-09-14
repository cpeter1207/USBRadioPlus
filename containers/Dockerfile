# syntax=docker/dockerfile:1.7
ARG DEBIAN_VERSION=13
ARG RUSTUP_INIT_VERSION=1.28.2
ARG RUST_STABLE=1.85.0
ARG RUST_NIGHTLY=nightly-2025-02-20
ARG CARGO_LLVM_COV_VERSION=0.6.21
FROM debian:${DEBIAN_VERSION} AS asl3-clean

ARG DEBIAN_VERSION
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      ca-certificates wget && \
    wget -qO /tmp/asl-repo.deb \
      "https://repo.allstarlink.org/public/asl-apt-repos.deb${DEBIAN_VERSION}_all.deb" && \
    dpkg -i /tmp/asl-repo.deb && \
    apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      "asl3-asterisk=2:22.9.0+asl3-3.9.3-1.deb${DEBIAN_VERSION}" && \
    rm -rf /var/lib/apt/lists/* /tmp/asl-repo.deb

FROM asl3-clean AS quality
ARG DEBIAN_VERSION
ARG TARGETARCH
ARG RUSTUP_INIT_VERSION
ARG RUST_STABLE
ARG RUST_NIGHTLY
ARG CARGO_LLVM_COV_VERSION
ENV RUSTUP_HOME=/opt/rustup
ENV CARGO_HOME=/opt/cargo
ENV PATH=/opt/cargo/bin:${PATH}
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      "asl3-asterisk-dev=2:22.9.0+asl3-3.9.3-1.deb${DEBIAN_VERSION}" build-essential clang clang-format clang-tidy cppcheck doxygen git graphviz \
      gcovr pkg-config python3 python3-pytest python3-venv shellcheck \
      libasound2-dev libusb-1.0-0-dev libsamplerate0-dev \
      libavfilter-dev libavutil-dev ladspa-sdk portaudio19-dev autoconf automake libtool \
      ca-certificates cargo curl patch rustc xz-utils && \
    rm -rf /var/lib/apt/lists/* && \
    python3 -m venv /opt/usbradioplus-quality && \
    /opt/usbradioplus-quality/bin/pip install --no-cache-dir \
      coverage==7.10.6 pytest==8.4.2 pytest-cov==7.0.0 pytest-xdist==3.8.0 ruff==0.16.5 && \
    /opt/usbradioplus-quality/bin/ruff --version >/dev/null

# Pin the toolchains for owned Rust code and native amd64 branch coverage.
# Separately released components still enter only through shared_packages.
RUN case "${TARGETARCH}" in \
      amd64) rustup_host=x86_64-unknown-linux-gnu; rustup_sha=20a06e644b0d9bd2fbdbfd52d42540bdde820ea7df86e92e533c073da0cdd43c ;; \
      arm64) rustup_host=aarch64-unknown-linux-gnu; rustup_sha=e3853c5a252fca15252d07cb23a1bdd9377a8c6f3efa01531109281ae47f841c ;; \
      *) echo "unsupported architecture: ${TARGETARCH}" >&2; exit 1 ;; \
    esac; \
    curl --proto '=https' --tlsv1.2 --fail --silent --show-error \
      "https://static.rust-lang.org/rustup/archive/${RUSTUP_INIT_VERSION}/${rustup_host}/rustup-init" \
      --output /tmp/rustup-init; \
    echo "${rustup_sha}  /tmp/rustup-init" | sha256sum --check --status; \
    chmod 0755 /tmp/rustup-init; \
    /tmp/rustup-init -y --no-modify-path --default-toolchain none; \
    rm -f /tmp/rustup-init && \
    rustup toolchain install "${RUST_STABLE}" --profile minimal --component clippy --component rustfmt && \
    rustup toolchain install "${RUST_NIGHTLY}" --profile minimal --component llvm-tools-preview && \
    rustup default "${RUST_STABLE}" && \
    cargo +"${RUST_NIGHTLY}" install cargo-llvm-cov --version "${CARGO_LLVM_COV_VERSION}" --locked

ENV PATH="/opt/usbradioplus-quality/bin:${PATH}"

COPY scripts/install-rnnoise.sh /tmp/usbradioplus/scripts/
COPY packaging/rnnoise/debian/patches/ \
  /tmp/usbradioplus/packaging/rnnoise/debian/patches/
RUN sh /tmp/usbradioplus/scripts/install-rnnoise.sh && rm -rf /tmp/usbradioplus

WORKDIR /usr/src/usbradioplus

# CI provides the digest-verified, released Debian packages as an explicit
# context. The consumer never builds or vendors a shared implementation.
FROM shared_packages AS released-shared-packages

FROM quality AS staged
COPY . .
COPY --from=released-shared-packages / /tmp/shared-packages/
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
      /tmp/shared-packages/*.deb && \
    ldconfig && make clean all && make DESTDIR=/stage prefix=/usr install

FROM asl3-clean AS usbradioplus-installed
ARG DEBIAN_VERSION
ARG RUN_SMOKE_TEST=0
RUN if [ "$DEBIAN_VERSION" = 12 ]; then av_packages="libavfilter8 libavutil57"; \
    else av_packages="libavfilter10 libavutil59"; fi && \
    apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      libasound2 $av_packages libportaudio2 libsamplerate0 \
      libusb-1.0-0 whiptail && \
    rm -rf /var/lib/apt/lists/*
COPY --from=quality /usr/local/lib/librnnoise.so.0.4.1 /usr/local/lib/
COPY --from=released-shared-packages / /tmp/shared-packages/
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y \
      /tmp/shared-packages/*[0-9]_*.deb && \
    rm -rf /var/lib/apt/lists/* /tmp/shared-packages
COPY --from=staged /stage/ /
COPY --chmod=0755 tests/container-smoke-test.sh /usr/local/libexec/usbradioplus/
# Shared-package installation can already create the SONAME link via ldconfig.
RUN ln -sf librnnoise.so.0.4.1 /usr/local/lib/librnnoise.so.0 && \
    ln -sf librnnoise.so.0 /usr/local/lib/librnnoise.so && \
    ldconfig && \
	 test "$(find /usr/lib -path '*/asterisk/modules/chan_usbradioplus.so' -print -quit)" != "" && \
	 test -x /usr/sbin/usbradioplus-tune && \
	 ldd "$(find /usr/lib -path '*/asterisk/modules/chan_usbradioplus.so' -print -quit)" | \
      tee /tmp/module-libraries && \
	! grep -q 'not found' /tmp/module-libraries && \
	grep -Fq 'librate_adjusting_pcm_ring2.so.2' /tmp/module-libraries && \
	grep -Fq 'librptadvradio.so.3' /tmp/module-libraries && \
	grep -Fq 'librptadv_samplerate_adapter.so.1' /tmp/module-libraries && \
	grep -Fq 'librptadv_ffmpeg_adapter.so.1' /tmp/module-libraries && \
	grep -Fq 'librptadv_portaudio_alsa_adapter.so.2' /tmp/module-libraries && \
	grep -Fq 'librptadv_gpio_adapter.so.1' /tmp/module-libraries && \
	grep -Fq 'librptadv_rnnoise_adapter.so.1' /tmp/module-libraries && \
    if [ "$RUN_SMOKE_TEST" = 1 ]; then \
      /usr/local/libexec/usbradioplus/container-smoke-test.sh; \
    fi
