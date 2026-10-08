# Build and distribute Android applications

The muon-ui npm package can build Android APKs containing your web assets. Use Vite and the public CLI without editing Gradle or JNI sources. Installed applications do not need a development server.

This release supports debug APKs and signed release APKs. FCM, QuickJS, the Node.js sidecar, and AAB generation are not publicly available.

## Requirements

| Component | Requirement |
| --- | --- |
| Build host | Linux x64 |
| Node.js/npm | Tested with Node.js 24. Meet the runtime requirements of your dependencies, including Vite |
| JDK | 17–25; execution tested with 25.0.3 |
| Android SDK | Platform `platforms/android-37.0`, Build Tools `build-tools/36.0.0` |
| Gradle/AGP | The included Wrapper uses Gradle 9.4.1 and AGP 9.2.1 |
| Application ABIs | `arm64-v8a` and `x86_64`; both included by default |
| Devices | Tested on API 37: Pixel 6 and an x86_64 emulator with 16 KiB pages |

Ordinary application builds need neither NDK/CMake nor a Muon source checkout. The npm package includes prebuilt Java and native components in an AAR. Initial Gradle and AndroidX downloads need a network connection. Android Studio and a connected device are optional for building.

The APK declares minimum installation API 24; older OS versions remain unverified. WebView must provide `WEB_MESSAGE_LISTENER`, `WEB_MESSAGE_ARRAY_BUFFER`, and `DOCUMENT_START_SCRIPT`. Missing features produce an on-screen startup diagnostic. Tested WebView versions are 153.0.8010.36 on Pixel 6 and 149.0.7827.5 on the emulator. arm64 alignment has been inspected for 16 KiB compatibility; runtime testing covers Pixel 6 with 4 KiB pages and x86_64 with 16 KiB pages. See [WebView feature detection](https://developer.android.com/reference/androidx/webkit/WebViewFeature) and [16 KiB page support](https://developer.android.com/guide/practices/page-sizes).

Install a JDK and [Android CLI](https://developer.android.com/tools/agents/android-cli), review the SDK licenses, and install the required packages:

```bash
export JAVA_HOME=/path/to/jdk-25
export ANDROID_HOME="$HOME/Android/Sdk"
export PATH="$JAVA_HOME/bin:$ANDROID_HOME/platform-tools:$PATH"
android --sdk="$ANDROID_HOME" sdk install platforms/android-37.0 build-tools/36.0.0 platform-tools
```

Adjust the paths for your environment. `platform-tools` is needed for device installation. Muon's `prepare` diagnoses missing or incompatible SDK/JDK components and prepares the pinned Gradle version. It does not install SDK/JDK packages or accept licenses.

## Add Muon to a Vite project

Add muon-ui as a development dependency in an existing Vite/TypeScript project:

```bash
npm install -D muon-ui
```

Configure the Muon plugin in `vite.config.ts`, keeping your existing React or other plugins. This example uses simple mode. Validate mode with virtual modules and import permissions is described below:

```ts
import { defineConfig } from 'vite';
import muon from 'muon-ui/vite';

export default defineConfig({
  plugins: [muon({
    pluginAccess: false,
    build: { targets: ['android'] },
  })],
});
```

Add application settings to `muon.json` in the project root. Replace `icons/app.png` with your PNG image, or omit `icon` to use the default icon:

```json
{
  "android": {
    "applicationId": "com.example.notes",
    "label": "Muon Notes",
    "versionCode": 1,
    "versionName": "1.0.0",
    "abis": ["arm64-v8a", "x86_64"],
    "icon": "icons/app.png"
  },
  "config": { "channel": "production" }
}
```

Check the SDK/JDK installation and prepare Gradle:

```bash
npx muon prepare --target android
```

With the Vite configuration above, `muon build` builds the web assets and generates a debug APK:

```bash
npx muon build --target android
```

Alternatively, `vite build` generates the same debug APK. Either `muon build` or `vite build` is sufficient:

```bash
npx vite build
```

The APK is written to `dist-muon/android/com.example.notes-1-debug.apk`. Its adjacent `.json` file records the variant, signing state, ABIs, application ID, and version. CLI results are also available with `--json`. `.muon/android` contains generated files; the next build replaces manual edits there.

Select Android explicitly with `--target android` or the Vite configuration. Default targets and `--all` remain desktop targets. Automatic Android launch from the Vite development server and HMR are outside this release's scope.

In a project without the Muon Vite plugin configured, use `--assets` to supply prebuilt web assets:

```bash
npx muon build --target android --assets ./dist
```

## Configuration and Muon APIs

You can also provide Android settings through Vite's `build.android`. Precedence is explicit Vite/API settings, `muon.json`, then defaults. CLI `--app-id` and `--name` override the configured application ID and label; explicitly supplied `build.android.applicationId` and `label` take precedence over those common options.

| Setting | Meaning / default |
| --- | --- |
| `applicationId` | Installation/update identity; defaults to `dev.muon.…` derived from the package name |
| `label` | Launcher label; defaults to the package name |
| `versionCode` | Integer from 1 to 2100000000; defaults to 1. Increase for each release |
| `versionName` | Display version; defaults to `package.json` version |
| `abis` | Included ABIs; both supported ABIs by default |
| `icon` | PNG image path |
| `permissions` | Array of `android.permission.…` manifest entries; empty by default |
| `sdkPath` | SDK directory; otherwise detected from `ANDROID_HOME`, `ANDROID_SDK_ROOT`, or adb on PATH |
| `plugins` | Prebuilt plugins described below; empty by default |
| `signing` | Release signing configuration described below |
| `fcm`, `quickjs` | Unavailable; must be omitted or false |

Relative paths in `muon.json` resolve from that file's directory. Paths supplied by Vite/API resolve from the project root. To select an SDK for `prepare`, use the environment, `muon.json`, or `--sdk-path`.

The default web asset URL is `https://main.asset.muon.invalid/index.html`. A subpath in Vite's `base` is preserved. Set `browser.startPage` to a URL on this origin or `asset://main/…`. External start pages, ZIP assets, and multiple asset hosts are unsupported.

Muon APIs initialize before your application JavaScript. No custom adapter import is needed. In simple mode:

```ts
import type {} from 'muon-ui';

const runtime = await window.muon.environments.getRuntimeInfo();
const config = await window.muon.environments.getConfigValues();
await window.muon.fs.writeTextFile('note.txt', 'Saved on Android', 'utf8');
const note = await window.muon.fs.readTextFile('note.txt', 'utf8');
```

In simple mode, omitting `plugin.plugins` exposes the 34 supported builtin functions. An explicit list loads only its entries: an empty list or a list without `internal` exposes no builtins. Each entry's `allow` restricts functions. Native policy checks also apply to directly constructed RPC calls.

### Import with validate mode

Remove `pluginAccess: false` from Vite and configure allowed importers and functions in `muon.json`:

```json
{
  "plugin": {
    "mode": "validate",
    "plugins": [{
      "name": "internal",
      "imports": [{
        "sources": ["src/**"],
        "allow": ["muon.environments.*", "muon.fs.readTextFile", "muon.fs.writeTextFile"]
      }]
    }]
  }
}
```

```ts
import type {} from 'muon-ui';
import { getRuntimeInfo } from 'muon:environments';
import { readTextFile, writeTextFile } from 'muon:fs';

const runtime = await getRuntimeInfo();
await writeTextFile('note.txt', 'Saved on Android', 'utf8');
const note = await readTextFile('note.txt', 'utf8');
```

`sources` selects paths relative to the project root; `packages` selects npm package names. Vite checks direct imports, and the native host checks each generated capability ID against its allowed functions. Validate mode does not expose `window.muon`. Unsupported functions and unmatched patterns fail at build time. Build separate Android and desktop JavaScript bundles with their respective targets.

Omitting `plugin.pages` accepts RPC from the trusted asset origin's main frame. Explicit values are limited to `asset://main/**` and `https://main.asset.muon.invalid/**`. An empty list disables the bridge. Unsupported conditions, including path-specific filters, fail the build.

`sources` and `packages` are build-time import checks. Code in the same page that obtains a valid capability can use its allowed functions. Runtime authentication of individual JavaScript files and isolation from same-origin iframes accessing the parent page are not guaranteed. WebMessage supplies origin and main-frame status, without the caller's JavaScript file or full document URL. See [WebMessageListener](https://developer.android.com/reference/androidx/webkit/WebViewCompat.WebMessageListener) and [Android limitations](./limitation.md#android-webview-backend).

Relative `muon.fs` paths refer to the application's private files directory. Data survives updates with the same application ID and signing key, and is removed by uninstalling. `content://` URIs and file dialogs are unavailable. See the [Android API compatibility policy](../../android-api-compatibility.md) for browser, environments, and fs support. Its QuickJS entries apply only to the prototype.

For external networking, add `android.permission.INTERNET` to `permissions`. Android does not enforce CEF's destination policies such as `network.allow`. Cleartext HTTP is disabled. Manifest permissions alone do not request runtime permissions from the user. Features requiring permission dialogs, including local network access, are not available through the public build path. See [Android limitations](./limitation.md#android-webview-backend) and [Android local network permission](https://developer.android.com/privacy-and-security/local-network-permission).

## Sign a release APK

With the Vite configuration above, `muon pack` builds the web assets, generates a release APK, signs it, and verifies the signature. You do not need to run `muon build` or `vite build` first.

Keep your keystore outside web assets and Vite's `public` directory. To create a new key, run the following and enter its password and certificate details interactively:

```bash
mkdir -p "$HOME/.local/share/notes-signing"
keytool -genkeypair -keystore "$HOME/.local/share/notes-signing/release.p12" \
  -alias release -keyalg RSA -keysize 2048 -validity 10000
```

Add this object as `android.signing` in `muon.json`. Specify the actual keystore path; `$HOME` is not expanded inside configuration strings:

```json
{
  "keystore": "/home/me/.local/share/notes-signing/release.p12",
  "keyAlias": "release",
  "storePasswordEnv": "NOTES_STORE_PASSWORD",
  "keyPasswordEnv": "NOTES_KEY_PASSWORD"
}
```

Omit `keyPasswordEnv` if the key and keystore passwords are the same. Store environment variable names, never password values, in configuration. For example, in Bash:

```bash
read -rs -p 'Keystore password: ' NOTES_STORE_PASSWORD
export NOTES_STORE_PASSWORD
npx muon pack --target android --type apk
unset NOTES_STORE_PASSWORD
```

If you configured `NOTES_KEY_PASSWORD`, set that variable too. In CI, populate these variables from your secret store.

The verified release APK is written to `artifacts/apk/com.example.notes-1-release.apk`. Its adjacent `.json` includes the public certificate's SHA-256 digest, without credentials. Missing signing configuration and the default Android debug certificate are rejected. See [Android app signing](https://developer.android.com/studio/publish/app-signing).

For updates, preserve `applicationId` and the signing key, increase `versionCode`, and run pack again. Debug and release APKs normally use different keys, so switching from debug to release requires uninstalling the debug app, which also removes its data.

## Install and inspect the application

Enable USB or wireless debugging and connect with adb. Set `ANDROID_SERIAL` when multiple devices are connected:

```bash
adb devices
export ANDROID_SERIAL=your-device-serial
adb install -r artifacts/apk/com.example.notes-1-release.apk
adb shell am start -W -n com.example.notes/dev.muon.runtime.MuonAppActivity
adb logcat -s MuonActivity AndroidRuntime chromium
```

On startup failures, inspect the on-screen diagnostic and logcat. For build failures, inspect CLI output and generated files under `.muon/android`. Rebuild using the public CLI.

## Prebuilt native plugins

Supply a `.so` for each selected ABI through `android.plugins`. Put permissions and plugin-specific settings in a matching `plugin.plugins` entry. Compiling plugin sources is not provided:

```json
{
  "android": {
    "plugins": [{
      "name": "calculator",
      "soname": "libcalculator.so",
      "libraries": {
        "arm64-v8a": "plugins/arm64-v8a/libcalculator.so",
        "x86_64": "plugins/x86_64/libcalculator.so"
      },
      "metadata": "plugins/calculator.json"
    }]
  },
  "plugin": {
    "mode": "validate",
    "plugins": [{
      "name": "calculator",
      "imports": [{ "sources": ["src/**"], "allow": ["muon.calculator.*"] }],
      "config": { "precision": "double" }
    }]
  }
}
```

This example uses `import { add } from 'muon:calculator'`. The plugin producer supplies its TypeScript declarations. Add an `internal` entry to use builtins too. In simple mode, place `allow` directly on the entry instead of using `imports`.

The producer supplies the following JSON file as `metadata`. Replace each hash with the library's SHA-256 after stripping and any other post-processing:

```json
{
  "schemaVersion": 1,
  "functions": ["muon.calculator.add"],
  "sha256": {
    "arm64-v8a": "<64 lowercase hexadecimal digits>",
    "x86_64": "<64 lowercase hexadecimal digits>"
  }
}
```

Metadata is required in validate mode. The build checks ABI hashes and resolves exact names and wildcards against each plugin's own function catalog. Startup also checks allowed functions against actual registrations, reporting the plugin name on mismatch. Metadata is optional in simple mode; when supplied, the same checks apply. Android binaries are never executed on the build host.

The builder also validates registration names, SONAME, ABI, `muon_init_plugin`, dependencies, and 16 KiB alignment. Plugin-specific configuration values must be strings. Common `signature` and `salt` settings are rejected on Android; use the metadata's ABI-specific SHA-256 hashes. Buffers, callbacks in both directions, returned native function proxies, and their release follow the shared plugin ABI. See [plugin development](./muon-plugin-develop.md).

## Follow-up scope

FCM notification delivery and background processing remain follow-up work. Current APKs run without Firebase configuration.

The intended QuickJS deployment model bundles JavaScript into a single ESM file on the build host, keeping builtin modules external. Code, its hash, and required feature metadata will be included in APK assets and updated with the application. Persistent data stays separate. Arbitrary npm packages, full Node.js compatibility, and independent JavaScript updates are outside this initial model. See the [plan6 deployment policy](../ja/plans/plan6.md#8-quickjsのjavascriptデプロイメント方針).
