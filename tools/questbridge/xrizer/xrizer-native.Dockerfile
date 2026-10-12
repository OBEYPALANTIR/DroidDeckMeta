# Native twin of xrizer-cross.Dockerfile for arm64 hosts: the guest image already has gcc, cmake,
# glslc, libx11-dev and Rust 1.90; xrizer adds libclang for the openvr crate's bindgen.
FROM droiddeck-guest-native:24.04-v1
RUN apt-get update \
    && apt-get install -y --no-install-recommends clang \
    && rm -rf /var/lib/apt/lists/*
