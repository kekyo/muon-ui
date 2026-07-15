# muon-ui self-build (Advanced topic)

Install the required packages:

```bash
apt-get update
apt-get install -y \
  build-essential ca-certificates cmake curl dbus file g++-mingw-w64 \
  git libasound2-dev libdrm-dev libgbm-dev libgtk-3-dev \
  libnss3-dev libxss-dev ninja-build wine xvfb
apt-get install -y \
  nodejs npm
```

- Installing Node.js through [nvm](https://github.com/nvm-sh/nvm) may be preferable. Use version 20 or later.

## Build and test

```bash
npm install
npm run build
npm run test
```

To launch muon with the debug page:

```bash
npm run dev
```

## Windows binary e2e tests

To run Windows binary e2e tests, you need a Windows 11 (amd64) machine running the [agent-rover](https://github.com/kekyo/agent-rover/) remote agent.
This can also be a virtual machine instance.
Then launch the tests as follows:

```bash
export AGENT_ROVER_WIN11_HOST=<agent-host-address>
export AGENT_ROVER_WIN11_TOKEN=<agent-token>

npm run test:windows-e2e --workspace muon-core-tester
```

Alternatively, if the environment variables are defined, `npm run test` includes the Windows e2e tests in the full test run.

## Package generation

```bash
# Prerequisities
sudo apt-get install -y podman
sudo podman run --rm --privileged docker.io/multiarch/qemu-user-static --reset -p yes

# Verify QEMU is working:
podman run --rm --platform linux/arm64 docker.io/library/debian:trixie-slim uname -m
# Should output: aarch64
```

Before package generation, prepare the container images for builds.
This procedure installs dependencies required for native builds and platform validation into target-specific Podman images, reducing the time spent installing apt packages inside each container on every package generation run.

```bash
# Build prerequisite images
./prereq.sh
```

Then build binaries for all platforms and generate the NPM package with:

```bash
npm run pack
```

This package script delegates to `build_package.sh`.
If you want to pass package generation options directly, you can run `build_package.sh` directly.

- Because native code is built and tested for all supported architectures, this takes a very long time and may take more than 30 minutes.
