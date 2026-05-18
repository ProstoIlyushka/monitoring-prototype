FROM ubuntu:24.04

# Переключаемся на российские зеркала
RUN sed -i 's|http://archive.ubuntu.com|http://mirror.yandex.ru/ubuntu|g' /etc/apt/sources.list.d/ubuntu.sources && \
    sed -i 's|http://security.ubuntu.com|http://mirror.yandex.ru/ubuntu|g' /etc/apt/sources.list.d/ubuntu.sources

# Устанавливаем зависимости
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
    cmake \
    make \
    g++ \
    curl \
    git \
    && rm -rf /var/lib/apt/lists/*

# Собираем Drogon из исходников
WORKDIR /tmp
RUN git clone https://github.com/drogonframework/drogon.git && \
    cd drogon && \
    git submodule update --init --recursive && \
    mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release \
          -DBUILD_REDIS=ON \
          -DREDISCPP_INCLUDE_DIRS=/usr/include/hiredis \
          .. && \
    make -j$(nproc) && \
    make install && \
    ldconfig && \
    rm -rf /tmp/drogon

WORKDIR /app
COPY . .

RUN rm -rf build && \
    mkdir build && cd build && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make -j$(nproc)

EXPOSE 8080
CMD ["./build/monitoring-server"]