# Developing and distributing external plugins

Add `muon-plugin-sdk` as a development dependency to build C/C++ plugins without obtaining the muon-ui repository. CEF on Linux and Windows and the Android runtime share the public C ABI. Applications control plugin registration and permissions in `muon.json`.

## Create a plugin

Run these commands in your application directory. The defaults are C++20 with cardio, Make, and pkg-config. Use the SDK from the same release as muon-ui.

```bash
npm install -D muon-plugin-sdk
npx muon-plugin init native/counter --name counter
npx muon-plugin doctor --config native/counter/muon-plugin.build.json --target linux-amd64
npx muon-plugin build native/counter/muon-plugin.build.json --target linux-amd64
```

The generated `plugin.cpp` exposes `muon.counter.increment`, returning `Promise<number>` to JavaScript. It demonstrates asynchronous completion and shutdown with cardio. Select C99 with `--language c`; that template uses completion callbacks without linking cardio. See the [plugin ABI guide](muon-plugin-develop.md) for initialization, type metadata, and ownership.

## Diagnose and install prerequisites

The standard development host is Linux. Windows targets use MinGW and Android targets use the NDK. `doctor` checks tools, target-specific SDK link inputs, sysroots, and pkg-config development libraries for the selected language and build system. It reports missing requirements together with installation commands or official documentation. Builds run the same checks first.

| Requirement | Debian/Ubuntu installation or setup |
| --- | --- |
| Node.js | Use a supported version listed in the SDK package.json. Node.js 24 is supported. |
| Make, pkg-config, GCC | `sudo apt install build-essential pkg-config` |
| Linux development libraries for C++/cardio | `sudo apt install libglib2.0-dev`; GIO 2.74 or later is required. |
| Independent CMake builds | `sudo apt install cmake`; use CMake 3.24 or later. |
| Windows i686/amd64 | `sudo apt install mingw-w64` |
| Linux armhf/arm64 cross compilers | `sudo apt install g++-arm-linux-gnueabihf g++-aarch64-linux-gnu` |
| Android | Install command-line tools, run `sdkmanager "ndk;29.0.14206865"`, and set `ANDROID_HOME`. |

See the installation documentation for [Node.js](https://nodejs.org/en/download), [GLib](https://docs.gtk.org/glib/building.html), [CMake](https://cmake.org/download/), [MinGW-w64](https://www.mingw-w64.org/getting-started/), and the [NDK](https://developer.android.com/studio/projects/install-ndk). Diagnostics do not install tools or accept licenses.

```bash
npx muon-plugin doctor --target windows-amd64 --language c
npx muon-plugin doctor --target android-arm64-v8a --android-sdk-path "$ANDROID_HOME"
npx muon-plugin doctor --config native/counter/muon-plugin.build.json --target linux-amd64 --json
```

Host GLib packages do not satisfy a cross target's requirements. Install development libraries for the target and provide `--sysroot` or `--pkg-config-directories`. Override compiler executables with `--cc` and `--cxx` when needed; these options do not accept compiler arguments.

```bash
npx muon-plugin doctor --target linux-arm64 --sysroot /opt/sysroots/arm64
npx muon-plugin build native/counter/muon-plugin.build.json --target linux-arm64 --sysroot /opt/sysroots/arm64
```

To diagnose vendor libraries, add `packages` to the build configuration. Authors supply a pkg-config expression and installation guidance appropriate to the target. This checks availability; configure actual linking separately in Make, CMake, or your build system.

```json
{
  "packages": [
    {
      "name": "libexample >= 1.0",
      "install": "Install libexample-dev for the selected target architecture.",
      "documentation": "https://example.org/libexample/install"
    }
  ]
}
```

Applications consuming only prebuilt plugins need neither the authoring SDK nor a plugin compiler or NDK. Android APK creation still requires the JDK and Android SDK components specified by muon-ui's [application toolchain](android.md).

## Select targets

| Platform | SDK targets |
| --- | --- |
| CEF / Linux | `linux-amd64`, `linux-armhf`, `linux-arm64` |
| CEF / Windows | `windows-i686`, `windows-amd64` |
| Android | `android-arm64-v8a`, `android-x86_64` |

List supported outputs in the source configuration's `targets`. An application build fails if its requested target is absent. Repeated `build` calls for different targets can collect outputs in one directory and `muon-plugin.json`. Use a fresh output directory after changing the plugin name or function catalog.

C++ plugins must use the target's matching cardio headers, definitions, and shared library. The SDK's cardio binary is a link input; the application uses muon's runtime. Do not bundle replacements for cardio or the C++ runtime. The SDK checks the plugin ABI and cardio header identifier, but does not promise binary compatibility across arbitrary compilers or future SDK versions.

Android uses API 24 and `c++_shared`. Dependency libraries must also support 16 KiB pages. See [Android C++ runtimes](https://developer.android.com/ndk/guides/cpp-support) and [page-size support](https://developer.android.com/guide/practices/page-sizes).

## Keep a plugin inside an application

No plugin NPM package is necessary. Reference the source configuration from `muon.json`. CLI and Vite finish its build before reading the function catalog, placing generated outputs in the application's `.muon/plugins/` directory.

```json
{
  "plugin": {
    "mode": "validate",
    "plugins": [
      {
        "name": "counter",
        "native": { "source": "native/counter/muon-plugin.build.json" },
        "imports": [{ "sources": ["src/**"], "allow": ["muon.counter.*"] }]
      }
    ]
  }
}
```

```typescript
import { increment } from 'muon:counter';
const value = await increment(41);
```

Choose exactly one of `native.source` (source configuration), `native.manifest` (an existing `muon-plugin.json`), or `native.package` (NPM package name). The entry's `name` must match the artifact's registration name. Registration names, NPM package names, and library filenames are distinct.

Source references also accept `native.sysroot` and `native.androidSdkPath`. For projects with complex cross toolchains, authors can run `build` themselves and let the application reference `native.manifest`. Omit application `signature` and `salt` on native references: they are supplied per target from the artifact.

## Make, CMake, and custom builds

The standard Make build compiles `sources` from `muon-plugin.build.json`. Supply values such as `CPPFLAGS` and `LDLIBS` through `build.args`. This minimal build recompiles on each invocation so header and flag changes take effect. Provide your own Makefile or CMake project for incremental object builds. Standard Make paths support letters, digits, and `_ . / + -`; use CMake or a custom build for paths containing spaces or other characters.

Generate an independent CMake project as follows. C++ uses `find_package(MuonPlugin CONFIG REQUIRED COMPONENTS Cardio)` and `Muon::Cardio`. C uses `find_package(MuonPlugin CONFIG REQUIRED)` and `Muon::Plugin`.

```bash
npx muon-plugin init native/counter --name counter --build-system cmake
npx muon-plugin build native/counter/muon-plugin.build.json --target linux-amd64
```

Existing build systems can obtain SDK settings directly. `env --shell` emits POSIX shell exports; plain `env` emits JSON. Set this environment when invoking a generated Makefile manually as well.

```bash
eval "$(npx muon-plugin env --target linux-amd64 --shell)"
pkg-config --cflags --libs muon-plugin-cardio
make -C native/counter
cmake -S native/counter -B .build/counter -DMuonPlugin_DIR="$MUON_SDK/cmake" -DMUON_PLUGIN_TARGET=linux-amd64 -DMUON_PLUGIN_OUTPUT_DIRECTORY="$PWD/.build/plugin" -DMUON_PLUGIN_OUTPUT_NAME=counter
```

Use `build.system: "custom"` and `commands` for explicit build commands. Commands execute without a shell. Arguments and dependency paths support funcity variables `{{source}}`, `{{build}}`, `{{output}}`, `{{sdk}}`, and `{{target}}`. `build.directory` is relative to the source configuration. Write generated files to `{{build}}` and the completed plugin to `{{output}}`. Add other prerequisite executables with `build.tools`.

```json
{
  "build": {
    "system": "custom",
    "tools": ["ninja"],
    "commands": [
      { "command": "cmake", "args": ["-S", "{{source}}", "-B", "{{build}}", "-G", "Ninja", "-DMuonPlugin_DIR={{sdk}}/cmake", "-DMUON_PLUGIN_TARGET={{target}}"] },
      { "command": "cmake", "args": ["--build", "{{build}}"] }
    ]
  }
}
```

This example assumes your CMakeLists.txt uses the `MUON_OUTPUT` environment variable to select its final output. Relative paths such as `./compile` in `commands[].command` and `tools` are checked and executed relative to `build.directory`. Custom builds use the same public headers and definitions and pass the same artifact validation.

## Vendor sources and ordinary shared libraries

Vendor or submodule sources may retain their own build system. Keep the muon adapter and configuration in your own directory and write no generated outputs into external sources or node_modules. The [independent examples](../../sdk/examples/README.md) use CMake `add_subdirectory` to build a separate C library and call it from a C plugin.

When invoking a vendor or installed package configuration directly from the CLI, set `--out` to a directory you own. Otherwise, output defaults to `.muon-plugin/` beside the configuration. Application `native.source` references automatically use an application-owned output directory.

List target-specific shared dependencies in `dependencies`. Ordinary dependencies need no `muon_init_plugin` entry point. Put development-only files, such as Windows import libraries, in `linkLibraries`.

```json
{
  "dependencies": {
    "linux-amd64": ["{{build}}/libexample.so"],
    "windows-amd64": ["{{build}}/example.dll"],
    "android-arm64-v8a": ["{{build}}/libexample.so"]
  },
  "linkLibraries": { "windows-amd64": ["{{build}}/libexample.dll.a"] }
}
```

Validation reads ELF DT_NEEDED and PE imports to check that ordinary dependencies are available. It rejects conflicting filenames and replacement runtime libraries. Linux libraries importing adjacent dependencies need a `$ORIGIN` RPATH or RUNPATH; the Make and CMake templates include this setting. See the [Linux dynamic linker](https://man7.org/linux/man-pages/man8/ld.so.8.html) and [Windows DLL search](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw).

## Publish through NPM

Add `headers`, `types`, and `licenses` to the source configuration to include public files in `include/`, `types/`, and `licenses/`. Files are copied directly into those directories using their basenames. Avoid duplicate filenames and make header includes agree with this layout. Authors maintain type declarations and function catalogs.

```bash
npx muon-plugin build native/counter/muon-plugin.build.json --target linux-amd64 --out packages/counter
npx muon-plugin build native/counter/muon-plugin.build.json --target windows-amd64 --out packages/counter
npx muon-plugin verify packages/counter/muon-plugin.json --target linux-amd64
```

Add this package.json to the completed directory and inspect the result with `npm pack`. Include prebuilt .so/.dll files and dependencies under `native`. No postinstall compilation is required.

```json
{
  "name": "my-muon-counter",
  "version": "1.0.0",
  "types": "./types/index.d.ts",
  "exports": {
    ".": { "types": "./types/index.d.ts" },
    "./muon-plugin.json": "./muon-plugin.json"
  },
  "files": ["muon-plugin.json", "native", "include", "types", "licenses"]
}
```

```bash
cd packages/counter
npm pack
```

For existing .so/.dll outputs, use the Node.js `packagePlugin` API or `muon-plugin package <configuration.json>`. The JSON matches `PackagePluginOptions`: `outputDirectory`, `name`, `functions`, `targets`, and optional public files. Finish stripping and signing before packaging. Packaging computes SHA-256 hashes, so subsequent binary changes fail verification.

## Consume an NPM plugin

```bash
npm install my-muon-counter
```

Change the application's native reference to `{ "package": "my-muon-counter" }`. The build selects artifacts for the output target and generates Vite virtual modules from the producer's catalog, without executing cross-target binaries on the build host. At runtime, muon checks actual registration against the catalog as well.

Authors may supply virtual-module declarations, or applications can connect the published interface themselves. The SDK template generates the `counterPlugin` interface.

```typescript
// src/plugins.d.ts
// Keep this file free of top-level imports so this declares an ambient module.
declare module 'muon:counter' {
  export const increment: import('my-muon-counter').counterPlugin['increment'];
}
```

Referencing a native artifact does not grant JavaScript additional permissions. Applications still specify `mode`, `imports`, `allow`, and plugin-specific `config`. See [using plugins](muon-plugins.md).
