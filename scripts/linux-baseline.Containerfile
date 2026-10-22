# Build native Linux payloads against the oldest tested host ABI.
ARG BASE_IMAGE
FROM ${BASE_IMAGE} AS baseline
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    binutils build-essential bzip2 ca-certificates cmake coreutils curl file \
    gcc-10 g++-10 gettext libasound2-dev libatk-bridge2.0-dev libdrm-dev \
    libffi-dev libgbm-dev libglib2.0-dev libgtk-3-dev libnss3-dev libpcre2-dev \
    libssl-dev libx11-dev libxcb1-dev libxcomposite-dev libxdamage-dev \
    libxext-dev libxfixes-dev libxkbcommon-dev libxrandr-dev libxrender-dev \
    libxss-dev libxtst-dev libwayland-dev wayland-protocols libxml2-utils \
    make ninja-build patch pkg-config python3-pip \
    tar xz-utils zlib1g-dev xvfb xauth fonts-dejavu-core dbus-x11 \
    && rm -rf /var/lib/apt/lists/*
ENV CC=gcc-10 CXX=g++-10

FROM baseline AS toolchain
ARG BUILD_JOBS=8
RUN case "$(dpkg --print-architecture)" in \
      amd64) cmake_archive=cmake-3.25.3-linux-x86_64.tar.gz; cmake_sha256=d4d2ba83301b215857d3b6590cd4434a414fa151c5807693abe587bd6c03581e ;; \
      arm64) cmake_archive=cmake-3.25.3-linux-aarch64.tar.gz; cmake_sha256=cd6e853639f858569e22e6298f009eb24b4702c51c0d5bc1cb36c688f7ce246d ;; \
      armhf) cmake_archive=cmake-3.25.3.tar.gz; cmake_sha256=cc995701d590ca6debc4245e9989939099ca52827dd46b5d3592f093afe1901c ;; \
      *) exit 1 ;; \
    esac \
    && curl -fsSL -o /tmp/cmake.tar.gz https://github.com/Kitware/CMake/releases/download/v3.25.3/${cmake_archive} \
    && echo "${cmake_sha256}  /tmp/cmake.tar.gz" | sha256sum -c - \
    && if [ "$(dpkg --print-architecture)" = armhf ]; then \
      tar -xzf /tmp/cmake.tar.gz -C /tmp \
      && mkdir /tmp/cmake-build && cd /tmp/cmake-build \
      && CFLAGS=-D_FILE_OFFSET_BITS=64 CXXFLAGS=-D_FILE_OFFSET_BITS=64 \
        /tmp/cmake-3.25.3/bootstrap --parallel=${BUILD_JOBS} --prefix=/usr/local -- \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF -DBUILD_CursesDialog=OFF \
      && make -j${BUILD_JOBS} && make install; \
    else \
      tar -xzf /tmp/cmake.tar.gz -C /usr/local --strip-components=1; \
    fi \
    && cd / && rm -rf /tmp/cmake.tar.gz /tmp/cmake-3.25.3 /tmp/cmake-build
ARG GLIB_VERSION=2.74.7
ARG GLIB_SHA256=196ab86c27127a61b7a70c3ba6af7b97bdc01c07cd3b21abd5e778b955eccb1b
RUN pip3 install --no-cache-dir meson==1.3.2 \
    && curl -fsSL -o /tmp/glib.tar.xz https://download.gnome.org/sources/glib/2.74/glib-${GLIB_VERSION}.tar.xz \
    && echo "${GLIB_SHA256}  /tmp/glib.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/glib.tar.xz -C /tmp \
    && meson setup /tmp/glib-build /tmp/glib-${GLIB_VERSION} \
      --prefix=/opt/muon-baseline --libdir=lib --buildtype=release \
      -Dtests=false -Dgtk_doc=false -Dman=false \
    && meson compile -C /tmp/glib-build -j ${BUILD_JOBS} \
    && meson install -C /tmp/glib-build \
    && mkdir -p /opt/muon-baseline/share/licenses/glib \
    && cp /tmp/glib-${GLIB_VERSION}/COPYING /opt/muon-baseline/share/licenses/glib/ \
    && rm -rf /tmp/glib.tar.xz /tmp/glib-${GLIB_VERSION} /tmp/glib-build
ENV PKG_CONFIG_PATH=/opt/muon-gtk/lib/pkgconfig:/opt/muon-baseline/lib/pkgconfig
ENV LD_LIBRARY_PATH=/opt/muon-gtk/lib:/opt/muon-baseline/lib
ENV PATH=/opt/muon-gtk/bin:/opt/muon-baseline/bin:${PATH}
ARG GTK_VERSION=3.24.52
ARG GTK_SHA256=80931fa472a77b9a164f6740e3c0b444fac6770054632d35a7ff9d679e5e7b9f
# Preserve the baseline ABI while using upstream's current Wayland settings support.
RUN apt-get update && apt-get install -y --no-install-recommends libcups2-dev libcolord-dev \
    && rm -rf /var/lib/apt/lists/* \
    && curl -fsSL -o /tmp/gtk.tar.xz https://download.gnome.org/sources/gtk/3.24/gtk-${GTK_VERSION}.tar.xz \
    && echo "${GTK_SHA256}  /tmp/gtk.tar.xz" | sha256sum -c - \
    && tar -xJf /tmp/gtk.tar.xz -C /tmp \
    && meson setup /tmp/gtk-build /tmp/gtk-${GTK_VERSION} \
      --prefix=/opt/muon-gtk --libdir=lib --buildtype=release \
      -Dx11_backend=true -Dwayland_backend=true -Dbuiltin_immodules=none \
      -Dprint_backends=cups,file,lpr -Dcolord=yes \
      -Dtests=false -Dinstalled_tests=false -Ddemos=false -Dexamples=false \
      -Dintrospection=false -Dgtk_doc=false -Dman=false \
    && meson compile -C /tmp/gtk-build -j ${BUILD_JOBS} \
    && meson install -C /tmp/gtk-build \
    && mkdir -p /opt/muon-gtk/share/licenses/gtk \
    && cp /tmp/gtk-${GTK_VERSION}/COPYING /opt/muon-gtk/share/licenses/gtk/ \
    && rm -rf /tmp/gtk.tar.xz /tmp/gtk-${GTK_VERSION} /tmp/gtk-build
# Keep external input methods and non-GTK module caches from this same baseline.
RUN apt-get update && apt-get install -y --no-install-recommends \
    ibus-gtk3 fcitx-frontend-gtk3 fcitx5-frontend-gtk3 \
    && rm -rf /var/lib/apt/lists/*
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
RUN node --input-type=module -e 'import {writeFileSync} from "node:fs"; const [glibVersion,glibSha256,gtkVersion,gtkSha256]=process.argv.slice(1); writeFileSync("/opt/muon-baseline/source.json", JSON.stringify({glib:{name:"GLib",version:glibVersion,prefix:"/opt/muon-baseline",url:`https://download.gnome.org/sources/glib/2.74/glib-${glibVersion}.tar.xz`,sha256:glibSha256,license:"/opt/muon-baseline/share/licenses/glib/COPYING"},gtk:{name:"GTK",version:gtkVersion,prefix:"/opt/muon-gtk",url:`https://download.gnome.org/sources/gtk/3.24/gtk-${gtkVersion}.tar.xz`,sha256:gtkSha256,license:"/opt/muon-gtk/share/licenses/gtk/COPYING"}},null,2)+"\n");' "${GLIB_VERSION}" "${GLIB_SHA256}" "${GTK_VERSION}" "${GTK_SHA256}"
