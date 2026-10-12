FROM --platform=linux/arm64 ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Native twin of guest-cross.Dockerfile for arm64 hosts, where qemu-emulated cross compilers crash.
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        curl \
        glslc \
        ninja-build \
        pkg-config \
        python3 \
        libvulkan-dev \
        libx11-dev \
        libxrandr-dev \
        libxxf86vm-dev \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /src

# Ubuntu 24.04's cargo is 1.75; xrizer needs Rust 1.85 (edition 2024).
ARG RUSTUP_VERSION=1.28.2
ARG RUSTUP_SHA256=e3853c5a252fca15252d07cb23a1bdd9377a8c6f3efa01531109281ae47f841c
ARG RUST_TOOLCHAIN=1.90.0
ENV RUSTUP_HOME=/opt/rustup \
    CARGO_HOME=/opt/cargo \
    PATH=/opt/cargo/bin:$PATH \
    CARGO_TARGET_AARCH64_UNKNOWN_LINUX_GNU_LINKER=gcc
# chmod: builds run as the host uid, which must be able to fill the cargo registry cache.
RUN curl -fsSL --retry 3 -o /tmp/rustup-init \
        "https://static.rust-lang.org/rustup/archive/${RUSTUP_VERSION}/aarch64-unknown-linux-gnu/rustup-init" \
    && echo "${RUSTUP_SHA256}  /tmp/rustup-init" | sha256sum -c - \
    && chmod +x /tmp/rustup-init \
    && /tmp/rustup-init -y --no-modify-path --profile minimal \
        --default-toolchain "${RUST_TOOLCHAIN}" --target aarch64-unknown-linux-gnu \
    && rm /tmp/rustup-init \
    && chmod -R a+rwX /opt/cargo

WORKDIR /src
