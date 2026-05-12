# ============================================================
# Stage 1: Build the analyzer
# ============================================================
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    llvm-16-dev \
    libzstd-dev \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

COPY CMakeLists.txt ./
COPY include/ include/
COPY src/ src/

RUN cmake -S . -B out -DCMAKE_BUILD_TYPE=Release \
    && cmake --build out -j"$(nproc)"

# ============================================================
# Stage 2: Minimal runtime image
# ============================================================
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    clang-16 \
    libzstd1 \
    zlib1g \
    libtinfo6 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=builder /build/out/analyzer /usr/local/bin/analyzer

WORKDIR /work

# Default: read conf.json from /work, write result to stdout
ENTRYPOINT ["analyzer"]
CMD ["conf.json", "--stdout"]
