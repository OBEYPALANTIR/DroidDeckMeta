# xrizer's build needs two host tools the guest image lacks: a native `cc` to link Rust build
# scripts and libclang for the openvr crate's bindgen. Ubuntu's clang provides both (`cc` through
# update-alternatives); xrizer's own CI image (.github/workflows/ubuntu.dockerfile) uses it too.
FROM droiddeck-guest-cross:24.04-v1
RUN apt-get update \
    && apt-get install -y --no-install-recommends clang \
    && rm -rf /var/lib/apt/lists/*
