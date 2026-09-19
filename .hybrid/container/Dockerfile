FROM ghcr.io/cpeter1207/usbradioplus-quality-debian13@sha256:788c966667e156c7c89762a2fe87547cc104d6b681dc2a1f27574274e5811835

RUN apt-get update && apt-get install -y --no-install-recommends libclang-dev cppcheck clang-tidy doxygen shellcheck groff jq && rm -rf /var/lib/apt/lists/*

# Cache locked dependencies only; no project implementation is built or vendored.
COPY manifests/ /opt/usb-cache/
RUN for dir in /opt/usb-cache/rust/*/; do mkdir -p "$dir/src" && touch "$dir/src/lib.rs" "$dir/src/main.rs"; done \
    && cargo fetch --locked --manifest-path /opt/usb-cache/Cargo.toml \
    && rm -rf /opt/usb-cache
