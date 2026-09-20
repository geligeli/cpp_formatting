# syntax=docker/dockerfile:1.7

# Pinned tool versions; override with `--build-arg NAME=value`.
ARG UBUNTU_VERSION=26.04
ARG BAZEL_VERSION=8.6.0
ARG BUILDIFIER_VERSION=8.2.1
ARG IBAZEL_VERSION=0.29.0
ARG BAZEL_COMPILE_COMMANDS_VERSION=0.20.1

# ---------------------------------------------------------------------------
# Stage 1 -- fetch the pinned release binaries.
#
# Nothing from this stage reaches the final image except the binaries
# themselves, so curl/unzip, the downloaded archives and the ~320 MB Bazel
# install base that `bazel help completion` extracts never cost a layer there.
# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION} AS tools

ARG TARGETARCH
ARG BAZEL_VERSION
ARG BUILDIFIER_VERSION
ARG IBAZEL_VERSION
ARG BAZEL_COMPILE_COMMANDS_VERSION
ARG DEBIAN_FRONTEND=noninteractive

# Keep the downloaded .debs and the package lists in BuildKit caches instead of
# in a layer: `docker-clean` has to go for apt to leave anything behind.
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    rm -f /etc/apt/apt.conf.d/docker-clean \
    && apt-get update \
    && apt-get install -y --no-install-recommends ca-certificates curl unzip

# `curl -f` is load-bearing: without it a 404 response body is written to the
# output path and chmod +x'd, and the failure only surfaces at runtime.
RUN set -eux; \
    case "${TARGETARCH}" in \
      amd64) BAZEL_ARCH=x86_64 ;; \
      arm64) BAZEL_ARCH=arm64 ;; \
      *) echo "unsupported TARGETARCH=${TARGETARCH}" >&2; exit 1 ;; \
    esac; \
    dl() { curl -fsSL --retry 3 --retry-delay 2 "$1" -o "$2"; }; \
    mkdir -p /out/usr/local/bin /out/usr/local/share; \
    dl "https://github.com/bazelbuild/bazel/releases/download/${BAZEL_VERSION}/bazel-${BAZEL_VERSION}-linux-${BAZEL_ARCH}" \
       /out/usr/local/bin/bazel; \
    dl "https://github.com/bazelbuild/buildtools/releases/download/v${BUILDIFIER_VERSION}/buildifier-linux-${TARGETARCH}" \
       /out/usr/local/bin/buildifier; \
    dl "https://github.com/bazelbuild/bazel-watcher/releases/download/v${IBAZEL_VERSION}/ibazel_linux_${TARGETARCH}" \
       /out/usr/local/bin/ibazel; \
    dl "https://github.com/kiron1/bazel-compile-commands/releases/download/v${BAZEL_COMPILE_COMMANDS_VERSION}/bazel-compile-commands_${BAZEL_COMPILE_COMMANDS_VERSION}-linux_${TARGETARCH}.zip" \
       /tmp/bazel-compile-commands.zip; \
    unzip -q /tmp/bazel-compile-commands.zip -d /tmp/bcc; \
    for t in bazel-compile-commands bazel-clangd-wrapper; do \
      src="$(find /tmp/bcc -type f -name "$t" -print -quit)"; \
      [ -n "$src" ] || { echo "missing $t in the release archive" >&2; exit 1; }; \
      install -m 0755 "$src" "/out/usr/local/bin/$t"; \
    done; \
    chmod 0755 /out/usr/local/bin/*; \
    /out/usr/local/bin/bazel help completion bash > /out/usr/local/share/bazel-complete.bash

# ---------------------------------------------------------------------------
# Stage 2 -- the devcontainer image.
# ---------------------------------------------------------------------------
FROM ubuntu:${UBUNTU_VERSION}

ARG DEBIAN_FRONTEND=noninteractive

# One apt layer, no recommends, lists/debs kept in BuildKit caches rather than
# in the image.  `xxd` is gone: the `xxd -i` genrule it served was replaced by
# //cpp_formatting:embed_file.
#
# No compiler is installed for *this* repository: its C/C++ toolchain is the
# hermetic one Bazel downloads (`@llvm`, see MODULE.bazel), so `clang` and `lld`
# -- which the release config needed while it said `--action_env=CC=clang` --
# are gone.  `g++` stays for the e2e suite only: e2e/run_e2e.sh builds *other*
# repositories (googletest, abseil, re2, ...) in a second workspace, and those
# register no toolchain, so Bazel auto-detects the machine's.  `clang-format`
# and `clangd` are tools for the pre-commit hook and the editor, not compilers.
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    rm -f /etc/apt/apt.conf.d/docker-clean \
    && apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates \
        clang-format \
        clangd \
        curl \
        g++ \
        git \
        less \
        locales \
        openssh-client \
        python3-pip \
        sudo \
    && locale-gen en_US.UTF-8 \
    && echo '%sudo ALL=(ALL) NOPASSWD:ALL' > /etc/sudoers.d/99-nopasswd \
    && chmod 0440 /etc/sudoers.d/99-nopasswd

# update-locale only writes /etc/default/locale, which `docker exec` never
# reads; set the environment directly instead.
ENV LANG=en_US.UTF-8 \
    LC_ALL=en_US.UTF-8

RUN --mount=type=cache,target=/root/.cache/pip,sharing=locked \
    pip install --break-system-packages pre-commit

COPY --from=tools /out/ /

RUN echo '[ -f /usr/local/share/bazel-complete.bash ] && source /usr/local/share/bazel-complete.bash' \
    >> /etc/bash.bashrc

USER ubuntu
