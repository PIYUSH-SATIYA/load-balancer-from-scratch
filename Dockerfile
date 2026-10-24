# === Stage 1: Development Environment ===
FROM ubuntu:24.04 AS dev

RUN apt-get update && apt-get install -y \
    build-essential cmake git ca-certificates \
    netcat-openbsd curl iproute2 python3 \
    && rm -rf /var/lib/apt/lists/*

RUN apt-get update && apt-get install -y unzip \
    && rm -rf /var/lib/apt/lists/*

# wrk isn't in Ubuntu's default repos — build it once, bake into image
RUN git clone --depth 1 https://github.com/wg/wrk.git /tmp/wrk \
    && cd /tmp/wrk && make -j"$(nproc)" && cp wrk /usr/local/bin/ \
    && rm -rf /tmp/wrk

WORKDIR /app
# We don't COPY source here; docker-compose will volume-mount it.
# We sleep infinity so the container stays alive for dev usage.
CMD ["sleep", "infinity"]

# === Stage 2: Build ===
FROM dev AS build
COPY . /app
RUN mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)
