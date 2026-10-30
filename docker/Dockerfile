# Multi-arch (linux/amd64, linux/arm64) dev image.
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      clang-18 llvm-18-dev lld-18 cmake ninja-build python3 python3-pip git ca-certificates \
    && pip3 install --no-cache-dir --break-system-packages uv \
    && rm -rf /var/lib/apt/lists/*
ENV STEST_LLVM_ROOT=/usr/lib/llvm-18
WORKDIR /w
