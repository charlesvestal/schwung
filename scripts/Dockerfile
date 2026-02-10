# Dockerfile for building SEQOMD module for Move Anything (ARM64)
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y \
    build-essential \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    && rm -rf /var/lib/apt/lists/*

ENV CROSS_PREFIX=aarch64-linux-gnu-

WORKDIR /build