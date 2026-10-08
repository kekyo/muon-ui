# muon-plugin-sdk

Build native C/C++ plugins for muon-ui on CEF/Linux, CEF/Windows, and Android. Use the public headers and target-specific link libraries without obtaining or rebuilding muon-ui's source repository.

```bash
npm install -D muon-plugin-sdk
npx muon-plugin init native/counter --name counter
npx muon-plugin doctor --config native/counter/muon-plugin.build.json --target linux-amd64
npx muon-plugin build native/counter/muon-plugin.build.json --target linux-amd64
```

Defaults are C++20 with cardio, Make, and pkg-config. Select C99 with `--language c` or an independent CMake template with `--build-system cmake`. Existing build systems can use the same SDK environment and artifact format.

`doctor` checks tools, the NDK, target development libraries, and SDK inputs, reporting missing requirements and installation guidance together. Applications consuming prebuilt plugins do not need the authoring SDK or a native compiler.

See the [development and NPM distribution guide](https://github.com/kekyo/muon-ui/blob/main/docs/en/muon-plugin-sdk.md) and [independent examples](examples/README.md). Use the SDK from the same release as muon-ui. Supported targets are Linux amd64/armhf/arm64, Windows i686/amd64, and Android arm64-v8a/x86_64.

[日本語](README_ja.md)
