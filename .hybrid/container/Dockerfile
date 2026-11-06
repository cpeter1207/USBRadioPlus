FROM ghcr.io/cpeter1207/usbradioplus-quality-debian13@sha256:4f00ad3dd621dcdfcdcc901fffae9e73fe0fc1f0002aa3fd808df26cc31afc8d

RUN apt-get update && apt-get install -y --no-install-recommends libclang-dev cppcheck clang-tidy doxygen shellcheck groff jq ffmpeg && rm -rf /var/lib/apt/lists/*

# Cache locked dependencies only; no project implementation is built or vendored.
COPY manifests/ /opt/usb-cache/
RUN for dir in /opt/usb-cache/rust/*/; do mkdir -p "$dir/src" && touch "$dir/src/lib.rs" "$dir/src/main.rs"; done \
    && cargo fetch --locked --manifest-path /opt/usb-cache/Cargo.toml \
    && rm -rf /opt/usb-cache
