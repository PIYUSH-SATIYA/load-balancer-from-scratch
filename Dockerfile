# === Stage 1: Development Environment ===
FROM ubuntu:24.04 AS dev

RUN apt-get update && apt-get install -y \
    build-essential cmake git \
    libssl-dev unzip libpcre3-dev zlib1g-dev \
    netcat-openbsd curl iproute2 \
    && rm -rf /var/lib/apt/lists/*

# wrk isn't in Ubuntu's default repos — build it once, bake into image
RUN git clone https://github.com/wg/wrk.git /tmp/wrk \
    && cd /tmp/wrk && make && cp wrk /usr/local/bin/ \
    && rm -rf /tmp/wrk

WORKDIR /app
# We don't COPY source here; docker-compose will volume-mount it.
# We sleep infinity so the container stays alive for dev usage.
CMD ["sleep", "infinity"]

# === Stage 2: Build ===
FROM dev AS build
COPY . /app
RUN mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)

# === Stage 3: Production (Lean) ===
FROM ubuntu:24.04 AS prod
COPY --from=build /app/build/loadbalancer /usr/local/bin/
COPY --from=build /app/build/dummy_backend /usr/local/bin/
CMD ["loadbalancer"]
