# Derived from the guest-native image (cmake, g++ already present): adds the
# mingw-w64 x86_64 C++ cross compiler for the d3d11-probe Windows PE build.
# Native arm64 Ubuntu container; the toolchain target is Windows x86_64.
FROM droiddeck-guest-native:24.04-v1

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        binutils-mingw-w64-x86-64 \
        g++-mingw-w64-x86-64 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
