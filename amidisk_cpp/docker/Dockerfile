# Build and test amidisk C++ on Linux
#
# Usage:
#   cd amidisk_cpp
#   docker build -f docker/Dockerfile -t amidisk-linux .
#   docker run --rm amidisk-linux              # run tests
#   docker run --rm amidisk-linux ./build/bin/amidisk --help
#
# For interactive shell:
#   docker run --rm -it amidisk-linux /bin/bash

FROM ubuntu:22.04

# Install build dependencies
# git is required for CMake FetchContent to download CLI11 and GoogleTest
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

# Copy only what's needed for build (respects .dockerignore)
COPY . /src/

# Build with Release optimizations
RUN cmake -B build -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j$(nproc)

# Run tests by default
CMD ["./build/bin/amidisk_tests"]
