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

- Builds are tested with Node.js 24. You can install it through [nvm](https://github.com/nvm-sh/nvm).

Building Muon itself and running the full suite, including Android components, also requires JDK 17–25 and the Android SDK. JDK 25.0.3 is tested. Follow [Android application setup](./android.md#requirements), then install the NDK and CMake for manufacturing native components. Ordinary Muon application developers do not need this addition:

```bash
android --sdk="$ANDROID_HOME" sdk install ndk/29.0.14206865 cmake/4.1.2
```

## Source layout

Runtime code lives in `core/`. The CLI, Vite integration, application builders, and packaging code live in `ui/`. In each component, `common/` contains shared code, while `cef/` and `android/` contain backend implementations. Tests sit beside those directories in `common-test/`, `cef-test/`, and `android-test/`.

`core/android-poc/` contains the QuickJS experiment and its own tests. Native build tools live in `builder/`, and the Node.js host lives in `node/`. The public npm package names remain `muon-ui` for `ui/` and `muon-node` for `node/`.

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

## Android device tests

The full suite builds APKs and performs static checks. After connecting through ADB, run device tests separately. The emulator profile is API 37, x86_64, and 16 KiB pages; Pixel 6 uses API 37, arm64-v8a, and 4 KiB pages:

```bash
ANDROID_SERIAL=emulator-5556 npm run test:android --workspace muon-android-tester
ANDROID_SERIAL=your-pixel6-serial npm run test:android:pixel6 --workspace muon-android-tester
```

Run the QuickJS experiment in its separate workspace:

```bash
ANDROID_SERIAL=emulator-5556 npm run test:android --workspace muon-android-poc
ANDROID_SERIAL=your-pixel6-serial npm run test:android:pixel6 --workspace muon-android-poc
```

The independent npm consumer test covers the public CLI/Vite integration, signing, storage, restart, and updates preserving data. It recreates its dedicated device fixture, `dev.muon.e2e.publicconsumer`:

```bash
npm run build --workspace muon-ui
mkdir -p .run/android-e2e
npm pack --workspace muon-ui --pack-destination .run/android-e2e
ANDROID_SERIAL=your-device-serial node ui/android-test/test-packaged-application.mjs .run/android-e2e/muon-ui-0.0.1.tgz
```

Use the actual tgz version. Before passing `--plugins`, run `npm test --workspace muon-android-tester` to generate test plugins in `core/android-test/.build/plugins/<abi>/`. Add `--validate` to check validate-mode permissions. CI runs the same independent application on a 16 KiB emulator.

## Windows binary e2e tests

Windows binary e2e tests require a Windows 11 (amd64) machine running the [agent-rover](https://github.com/kekyo/agent-rover/) remote agent. A virtual machine is also supported. Node.js must be on the remote agent's PATH; Node.js 24 is tested. Restart agent-rover after installing Node.js to pick up the updated PATH.

Connections use agent-rover 0.10.0's standard configuration. Follow the [official connection configuration guide](https://github.com/kekyo/agent-rover/blob/0.10.0/README.md#connection-configuration) to register test machines in `~/.agent-rover` in the home directory of the user running the tests. If they are already registered, there is no need to copy hosts or tokens into environment variables.

Example `~/.agent-rover`:

```ini
[default]
test-agent.example.com=<access-token>
```

Run the Windows i686/amd64 screen tests and the installer tests with:

```bash
npm run test:windows-e2e --workspace muon-core-tester
npm run test:windows-nsis-e2e --workspace muon-ui
```

Tests use the `default` section unless you specify another section, for example by prefixing the command with `MUON_E2E_AGENT_SECTION=vm`. The SDK selects a machine and retries when machines are busy. It prioritizes `.agent-rover` in the current working directory, so account for this precedence if you place a configuration file inside an npm workspace.

Regular `npm test` runs skip tests on remote machines. The dedicated commands above set `MUON_E2E_REMOTE_WINDOWS=1` to enable them. Each suite releases its connection when it finishes. Set `MUON_E2E_REMOTE_WORK_DIR` to change the remote working directory. Screen tests discover the IPv4 routes for CDP and HTTP automatically. Networks requiring a different return address for the HTTP fixture server can override it with `MUON_E2E_REMOTE_HTTP_HOST`.

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
