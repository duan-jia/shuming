FROM mirror.ccs.tencentyun.com/library/ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        default-libmysqlclient-dev \
        git \
        gfortran \
        libboost-all-dev \
        libopenblas-dev \
        libssl-dev \
        pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY engine ./engine

RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DGGML_CUDA=OFF \
        -DFASTLLM_BUILD_TESTS=OFF \
    && cmake --build build --target fast-llm-reasoner --parallel 1

# Keep the runtime image small without guessing Ubuntu package names for every
# transitive C++ library. Both stages share the same Ubuntu base.
RUN set -eux; \
    mkdir -p /runtime-libs; \
    ldd /src/build/bin/fast-llm-reasoner \
      | awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^\//) { print $i; break } }' \
      | sort -u \
      | xargs -r -I '{}' cp --parents -L '{}' /runtime-libs

FROM mirror.ccs.tencentyun.com/library/ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install --yes --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home --home-dir /app fastllm

COPY --from=build /runtime-libs/ /
COPY --from=build /src/build/bin/fast-llm-reasoner /usr/local/bin/fast-llm-reasoner

WORKDIR /app
USER fastllm
EXPOSE 9006
ENTRYPOINT ["/usr/local/bin/fast-llm-reasoner"]
