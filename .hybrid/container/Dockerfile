FROM ghcr.io/cpeter1207/usbradioplus-quality-debian13@sha256:29b4e2111b8d8e48b4ec63a12902428405828cad546e45c2df1a99402e752328

RUN apt-get update && apt-get install -y --no-install-recommends libclang-dev cppcheck clang-tidy doxygen shellcheck groff jq && rm -rf /var/lib/apt/lists/*

# Cache locked dependencies only; no project implementation is built or vendored.
COPY manifests/ /opt/usb-cache/
RUN for dir in /opt/usb-cache/rust/*/; do mkdir -p "$dir/src" && touch "$dir/src/lib.rs" "$dir/src/main.rs"; done \
    && cargo fetch --locked --manifest-path /opt/usb-cache/Cargo.toml \
    && rm -rf /opt/usb-cache
