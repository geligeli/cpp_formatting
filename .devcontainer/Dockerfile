FROM ubuntu:26.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update -y
RUN apt install curl unzip git sudo g++ python3-pip clang-format clangd clang -y
# clang lld
RUN pip install pre-commit --break-system-packages --no-cache-dir
RUN curl -L https://github.com/bazelbuild/bazel/releases/download/8.6.0/bazel-8.6.0-linux-x86_64 -o /usr/local/bin/bazel
RUN chmod +x /usr/local/bin/bazel
RUN curl -L https://github.com/bazelbuild/buildtools/releases/download/v8.2.1/buildifier-linux-amd64 -o /usr/local/bin/buildifier
RUN chmod +x /usr/local/bin/buildifier
RUN curl -L https://github.com/kiron1/bazel-compile-commands/releases/download/v0.20.1/bazel-compile-commands_0.20.1-linux_amd64.zip -o /tmp/bazel-compile-commands.zip
RUN curl -L https://github.com/bazelbuild/bazel-watcher/releases/download/v0.29.0/ibazel_linux_amd64 -o /usr/local/bin/ibazel
RUN chmod +x /usr/local/bin/ibazel
RUN cd / && unzip /tmp/bazel-compile-commands.zip
RUN rm -rf /var/lib/apt/lists/* /tmp/bazel-compile-commands.zip

# RUN adduser ubuntu sudo
RUN echo '%sudo ALL=(ALL) NOPASSWD:ALL' >> /etc/sudoers

USER ubuntu

RUN /usr/local/bin/bazel help completion bash > /home/ubuntu/.bazel-complete.bash
RUN echo 'source /home/ubuntu/.bazel-complete.bash' >> /home/ubuntu/.bashrc
