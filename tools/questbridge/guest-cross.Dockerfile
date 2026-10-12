FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# arm64 packages only exist on ports.ubuntu.com, so the stock sources are pinned to amd64.
RUN sed -i 's/^Types: deb$/Types: deb\nArchitectures: amd64/' /etc/apt/sources.list.d/ubuntu.sources \
    && printf '%s\n' \
        'Types: deb' \
        'URIs: http://ports.ubuntu.com/ubuntu-ports' \
        'Suites: noble noble-updates noble-security' \
        'Components: main universe' \
        'Architectures: arm64' \
        'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg' \
        > /etc/apt/sources.list.d/ubuntu-ports.sources \
    && dpkg --add-architecture arm64 \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        crossbuild-essential-arm64 \
        curl \
        glslc \
        ninja-build \
        pkg-config \
        python3 \
        libvulkan-dev:arm64 \
        libx11-dev:arm64 \
        libxrandr-dev:arm64 \
        libxxf86vm-dev:arm64 \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /src

# Ubuntu 24.04's cargo is 1.75; xrizer needs Rust 1.85 (edition 2024).
ARG RUSTUP_VERSION=1.28.2
ARG RUSTUP_SHA256=20a06e644b0d9bd2fbdbfd52d42540bdde820ea7df86e92e533c073da0cdd43c
ARG RUST_TOOLCHAIN=1.90.0
ENV RUSTUP_HOME=/opt/rustup \
    CARGO_HOME=/opt/cargo \
    PATH=/opt/cargo/bin:$PATH \
    CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=aarch64-linux-gnu-gcc \
    PKG_CONFIG_LIBDIR=/usr/lib/aarch64-linux-gnu/pkgconfig:/usr/share/pkgconfig \
    PKG_CONFIG_ALLOW_CROSS=1
# chmod: builds run as the host uid, which must be able to fill the cargo registry cache.
RUN curl -fsSL --retry 3 -o /tmp/rustup-init \
        "https://static.rust-lang.org/rustup/archive/${RUSTUP_VERSION}/x86_64-unknown-linux-gnu/rustup-init" \
    && echo "${RUSTUP_SHA256}  /tmp/rustup-init" | sha256sum -c - \
    && chmod +x /tmp/rustup-init \
    && /tmp/rustup-init -y --no-modify-path --profile minimal \
        --default-toolchain "${RUST_TOOLCHAIN}" --target aarch64-unknown-linux-gnu \
    && rm /tmp/rustup-init \
    && chmod -R a+rwX /opt/cargo

WORKDIR /src
