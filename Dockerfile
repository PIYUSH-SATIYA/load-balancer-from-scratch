# The dev stage has tools and runtime dependencies for interactive work. Source
# is bind-mounted by Compose so edits on the host are immediately visible.
FROM ubuntu:24.04 AS dev

RUN apt-get update && apt-get install -y \
    build-essential cmake git ca-certificates \
    netcat-openbsd curl iproute2 python3 \
    && rm -rf /var/lib/apt/lists/*

RUN apt-get update && apt-get install -y unzip \
    && rm -rf /var/lib/apt/lists/*

# wrk is built into the image so the throughput harness can run without a
# separate host install. Pin a revision separately for strictly repeatable runs.
RUN git clone --depth 1 https://github.com/wg/wrk.git /tmp/wrk \
    && cd /tmp/wrk && make -j"$(nproc)" && cp wrk /usr/local/bin/ \
    && rm -rf /tmp/wrk

WORKDIR /app
# We don't COPY source here; docker-compose will volume-mount it.
# We sleep infinity so the container stays alive for dev usage.
CMD ["sleep", "infinity"]

# Optional self-contained production-style binary build. Compose targets dev
# for iteration; this stage is not the container used by the final host run.
FROM dev AS build
COPY . /app
RUN mkdir -p build && cd build && cmake -DCMAKE_BUILD_TYPE=Release .. && make -j$(nproc)
