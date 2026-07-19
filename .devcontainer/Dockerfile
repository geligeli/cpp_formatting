FROM ubuntu:26.04

ARG TARGETARCH

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -y
RUN apt install curl unzip git sudo g++ python3-pip clang-format clangd clang lld xxd locales -y
RUN locale-gen en_US.UTF-8 && update-locale LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
RUN pip install pre-commit --break-system-packages --no-cache-dir
RUN if [ "$TARGETARCH" = "amd64" ]; then BAZEL_ARCH=x86_64; else BAZEL_ARCH=arm64; fi \
    && curl -L "https://github.com/bazelbuild/bazel/releases/download/8.6.0/bazel-8.6.0-linux-${BAZEL_ARCH}" -o /usr/local/bin/bazel
RUN chmod +x /usr/local/bin/bazel
RUN curl -L "https://github.com/bazelbuild/buildtools/releases/download/v8.2.1/buildifier-linux-${TARGETARCH}" -o /usr/local/bin/buildifier
RUN chmod +x /usr/local/bin/buildifier
RUN curl -L "https://github.com/kiron1/bazel-compile-commands/releases/download/v0.20.1/bazel-compile-commands_0.20.1-linux_${TARGETARCH}.zip" -o /tmp/bazel-compile-commands.zip
RUN curl -L "https://github.com/bazelbuild/bazel-watcher/releases/download/v0.29.0/ibazel_linux_${TARGETARCH}" -o /usr/local/bin/ibazel
RUN chmod +x /usr/local/bin/ibazel
RUN cd / && unzip /tmp/bazel-compile-commands.zip
RUN rm -rf /var/lib/apt/lists/* /tmp/bazel-compile-commands.zip

# RUN adduser ubuntu sudo
RUN echo '%sudo ALL=(ALL) NOPASSWD:ALL' >> /etc/sudoers

USER ubuntu

RUN /usr/local/bin/bazel help completion bash > /home/ubuntu/.bazel-complete.bash
RUN echo 'source /home/ubuntu/.bazel-complete.bash' >> /home/ubuntu/.bashrc
