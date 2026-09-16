# Build environment for the swift-toolchain-wasm pipeline.
#
# Mirrors .github/workflows/build-toolchain.yml's "Install build dependencies" /
# "Install Swift" steps exactly, on the same base image the CI job uses, so a local
# Docker build hits the same toolchain versions the CI-produced artifacts were built
# with. This image only sets up the environment; it does not COPY the repo or run the
# pipeline. src/, build/ and out/ are meant to be bind-mounted so ninja's hundreds of
# thousands of build products land on the host filesystem, not in a container overlay
# layer, and so an interrupted run resumes on the next `docker run` instead of starting
# over.
FROM ubuntu:24.04

ARG SWIFT_VERSION=6.3.3

ENV DEBIAN_FRONTEND=noninteractive \
    TZ=Etc/UTC

# The base CI package list, plus what `swiftly install` itself reports as required to
# actually run the toolchain it downloads (libedit2, libpython3-dev, libz3-dev, etc.) —
# the CI runner ships those already; a bare ubuntu:24.04 container does not.
RUN apt-get update && apt-get install -y --no-install-recommends \
      cmake ninja-build clang lld python3 \
      binutils libcurl4-openssl-dev libxml2-dev libncurses-dev \
      libsqlite3-0 zlib1g-dev tzdata \
      git curl ca-certificates xz-utils gnupg2 unzip \
      libc6-dev libedit2 libgcc-13-dev libpython3-dev libstdc++-13-dev \
      libz3-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*

# A standalone WASI runtime, so the artifacts are tested the way they are used.
RUN curl -fsSL https://wasmtime.dev/install.sh | bash \
    && ln -s "$HOME/.wasmtime/bin/wasmtime" /usr/local/bin/wasmtime

# The native toolchain Stage 30 cross-compiles against (SWIFT_NATIVE_CLANG_TOOLS_PATH)
# and Stage 50's smoke test builds its example program with. Pinned to the same release
# the pipeline targets, passed as a build arg so it can't silently drift from env.sh.
#
# Explicitly invoked with bash -c rather than relying on a SHELL directive: swiftly's
# env.sh uses bash's `[[`, and SHELL is silently ignored by builders that default to the
# OCI image format (e.g. podman without --format docker), which would otherwise fall
# back to /bin/sh (dash) and fail on it.
RUN bash -c '\
    curl -fsSL -o /tmp/swiftly.tar.gz https://download.swift.org/swiftly/linux/swiftly-x86_64.tar.gz && \
    tar -xzf /tmp/swiftly.tar.gz -C /tmp && \
    /tmp/swiftly init -y --skip-install && \
    rm -f /tmp/swiftly.tar.gz /tmp/swiftly && \
    . "$HOME/.local/share/swiftly/env.sh" && \
    swiftly install "${SWIFT_VERSION}" --use'

# Loaded by every RUN/CMD below and by an interactive shell in the container, so the
# installed swiftc/swift-frontend and wasmtime are on PATH without re-sourcing by hand.
RUN echo '. "$HOME/.local/share/swiftly/env.sh"' >> /root/.bashrc
ENV BASH_ENV=/root/.local/share/swiftly/env.sh

WORKDIR /workspace

CMD ["bash"]
