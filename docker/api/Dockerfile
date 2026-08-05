# syntax=docker/dockerfile:1.7
#
# Multi-stage build for the launcher API.
#
# The dependency install is a separate layer keyed only on vcpkg.json, so editing source
# does not rebuild Drogon. The first build is slow (tens of minutes); every later one is
# incremental. A BuildKit cache mount keeps vcpkg's binary cache across builds.

ARG UBUNTU_VERSION=24.04
ARG VCPKG_COMMIT=39344dff01c5a5a0134caf2624cdd492f05d30ea

# ---------------------------------------------------------------------------
# builder — also used directly as the test image (see docker-compose.override.yml)
# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS builder

ARG VCPKG_COMMIT
ENV DEBIAN_FRONTEND=noninteractive

# Beyond the obvious toolchain, two groups of packages are needed by vcpkg ports rather
# than by our own code, and neither is named anywhere in vcpkg.json:
#   bison, flex                             -> libpq (pulled in by Drogon's postgres feature)
#   autoconf, autoconf-archive, automake,
#   libtool, gettext                        -> libsodium's autotools build
RUN apt-get update && apt-get install -y --no-install-recommends \
        autoconf \
        autoconf-archive \
        automake \
        bison \
        build-essential \
        ca-certificates \
        clang-format \
        cmake \
        curl \
        flex \
        gettext \
        git \
        libtool \
        linux-libc-dev \
        ninja-build \
        pkg-config \
        python3 \
        tar \
        unzip \
        zip \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --filter=blob:none https://github.com/microsoft/vcpkg /opt/vcpkg \
    && git -C /opt/vcpkg checkout ${VCPKG_COMMIT} \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

# VCPKG_FORCE_SYSTEM_BINARIES is deliberately NOT set: it would make vcpkg use the distro's
# CMake, which is older than the port scripts require and fails while configuring zlib.
ENV VCPKG_ROOT=/opt/vcpkg \
    VCPKG_DEFAULT_TRIPLET=x64-linux

WORKDIR /src

# Dependencies first: this layer is invalidated only by vcpkg.json.
COPY vcpkg.json ./
RUN --mount=type=cache,target=/root/.cache/vcpkg,sharing=locked \
    ${VCPKG_ROOT}/vcpkg install --x-feature=tests --triplet x64-linux

COPY CMakeLists.txt CMakePresets.json ./
COPY cmake ./cmake
COPY src ./src
COPY tests ./tests
COPY migrations ./migrations
COPY config ./config
COPY .clang-format ./

# VCPKG_INSTALLED_DIR must be given explicitly: the CLI `vcpkg install` above wrote to the
# manifest directory (/src/vcpkg_installed), whereas the CMake toolchain defaults to
# ${CMAKE_BINARY_DIR}/vcpkg_installed in manifest mode. Without this, every find_package
# fails even though the dependencies are right there.
ARG BUILD_TYPE=Release
RUN cmake -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=${BUILD_TYPE} \
        -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake \
        -DVCPKG_INSTALLED_DIR=/src/vcpkg_installed \
        -DVCPKG_MANIFEST_INSTALL=OFF \
        -DVCPKG_MANIFEST_FEATURES=tests \
        -DLAUNCHER_BUILD_TESTS=ON \
    && cmake --build build --parallel

# ---------------------------------------------------------------------------
# runtime
# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS runtime

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        tzdata \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --uid 10001 --user-group --no-create-home launcher

WORKDIR /app

COPY --from=builder /src/build/launcher-api /app/launcher-api
COPY migrations /app/migrations
COPY config /app/config
COPY docker/api/entrypoint.sh /app/entrypoint.sh

# Every path a named volume is mounted on has to exist here and be owned by `launcher`.
# Docker creates a missing mount point as root, and the API runs unprivileged, so a directory
# left out of this line is one the server cannot write to — which is how artwork uploads failed
# on every fresh deployment with "cannot create the media directory: Permission denied".
RUN chmod +x /app/entrypoint.sh \
    && mkdir -p /data/blobs /data/media /var/log/launcher \
    && chown -R launcher:launcher /app /data/blobs /data/media /var/log/launcher

USER launcher

EXPOSE 8080 9090

HEALTHCHECK --interval=15s --timeout=5s --start-period=20s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/api/v1/health || exit 1

ENTRYPOINT ["/app/entrypoint.sh"]
