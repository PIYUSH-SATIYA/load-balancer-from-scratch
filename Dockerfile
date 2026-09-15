FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    build-essential cmake git \
    libssl-dev unzip libpcre3-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# wrk isn't in Ubuntu's default repos — build it once, bake into image
RUN git clone https://github.com/wg/wrk.git /tmp/wrk \
    && cd /tmp/wrk && make && cp wrk /usr/local/bin/ \
    && rm -rf /tmp/wrk

WORKDIR /app
COPY . .
RUN mkdir -p build && cd build && cmake .. && make -j$(nproc)

CMD ["./build/loadbalancer"]
