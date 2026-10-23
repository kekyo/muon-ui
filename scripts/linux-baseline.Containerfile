# Build native Linux payloads against Debian 12 system GTK and GLib.
ARG BASE_IMAGE
FROM ${BASE_IMAGE} AS baseline
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    binutils build-essential bzip2 ca-certificates cmake coreutils curl file \
    gcc-12 g++-12 gettext libasound2-dev libatk-bridge2.0-dev libdrm-dev \
    libffi-dev libgbm-dev libglib2.0-dev libgtk-3-dev libnss3-dev libpcre2-dev \
    libssl-dev libx11-dev libxcb1-dev libxcomposite-dev libxdamage-dev \
    libxext-dev libxfixes-dev libxkbcommon-dev libxrandr-dev libxrender-dev \
    libxss-dev libxtst-dev libwayland-dev wayland-protocols libxml2-utils \
    make ninja-build patch pkg-config python3-pip \
    tar xz-utils zlib1g-dev xvfb xauth fonts-dejavu-core dbus-x11 \
    && rm -rf /var/lib/apt/lists/*
ENV CC=gcc-12 CXX=g++-12

# Node runs the static dependency analysis inside this exact baseline.
RUN case "$(dpkg --print-architecture)" in \
      amd64) node_arch=x64; node_sha256=dafe2e8f82cb97de1bd10db9e2ec4c07bbf53389b0799b1e095a918951e78fd4 ;; \
      armhf) node_arch=armv7l; node_sha256=998da599863b8104992472ca4e738842b5e9b76b3665ea8bc13bd1ca52019426 ;; \
      arm64) node_arch=arm64; node_sha256=d68adf72c531f1118bee75b20ffbc5911accfda5e73454a798625464b40a4adf ;; \
      *) exit 1 ;; \
    esac \
    && curl -fsSL -o /tmp/node.tar.xz https://nodejs.org/dist/v22.15.0/node-v22.15.0-linux-${node_arch}.tar.xz \
    && echo "${node_sha256}  /tmp/node.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/node.tar.xz -C /usr/local --strip-components=1 \
    && rm /tmp/node.tar.xz
