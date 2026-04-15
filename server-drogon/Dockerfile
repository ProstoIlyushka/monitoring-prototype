FROM ubuntu:24.04

RUN apt-get update && apt-get install -y \
    libpq-dev \
    libssl-dev \
    libjsoncpp-dev \
    uuid-dev \
    zlib1g-dev \
    libbrotli-dev \
    libyaml-cpp-dev \
    libhiredis-dev \
    libsqlite3-dev \
    libmariadb-dev \
    libdrogon-dev \
    cmake \
    make \
    g++ \
    && rm -rf /var/lib/apt/lists/*

COPY . /app
WORKDIR /app

RUN rm -rf build && \
    mkdir build && \
    cd build && \
    cmake .. && \
    make -j$(nproc)

WORKDIR /app/build
EXPOSE 8080
CMD ["./monitoring-server"]
