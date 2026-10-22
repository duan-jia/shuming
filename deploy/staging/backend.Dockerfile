ARG UBUNTU_IMAGE=ubuntu:24.04
FROM ${UBUNTU_IMAGE} AS build
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install --yes --no-install-recommends \
        build-essential ca-certificates cmake default-libmysqlclient-dev git \
        gfortran libboost-all-dev libopenblas-dev libssl-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY engine ./engine
# The image runs on a different CPU from the GitHub-hosted builder.
# Keep the embedding backend at its explicit AVX2 baseline, without native AMX.
RUN cmake -S . -B build \
        -DCMAKE_BUILD_TYPE=Release \
        -DGGML_CUDA=OFF \
        -DGGML_NATIVE=OFF \
        -DFASTLLM_BUILD_TESTS=OFF \
    && cmake --build build --target fast-llm-reasoner --parallel 2
RUN set -eux; \
    mkdir -p /runtime-libs; \
    ldd /src/build/bin/fast-llm-reasoner \
      | awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^\//) { print $i; break } }' \
      | sort -u \
      | xargs -r -I '{}' cp -L '{}' /runtime-libs/

FROM ${UBUNTU_IMAGE} AS runtime
ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install --yes --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --create-home --home-dir /app fastllm \
    && mkdir -p /models
COPY --from=build /runtime-libs/ /opt/fast-llm-reasoner/lib/
COPY --from=build /src/build/bin/fast-llm-reasoner /usr/local/bin/fast-llm-reasoner
COPY .artifacts/embedding/bge-small-zh-v1.5-q4_k_m.gguf /models/bge-small-zh-v1.5-q4_k_m.gguf
RUN chmod 0444 /models/bge-small-zh-v1.5-q4_k_m.gguf
ENV LD_LIBRARY_PATH=/opt/fast-llm-reasoner/lib
WORKDIR /app
USER fastllm
EXPOSE 9006
ENTRYPOINT ["/usr/local/bin/fast-llm-reasoner"]
