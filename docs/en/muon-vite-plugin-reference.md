# muon Vite plugin reference

The muon Vite plugin argument `options` is optional.
When omitted, both development launch and distribution builds use their default behavior.

```ts
import { defineConfig } from 'vite';
import muon from 'muon-ui/vite';

export default defineConfig({
  plugins: [
    muon({  // Options for the muon Vite plugin
      build: {
        targets: ['linux-amd64', 'windows-amd64'],
        outputRoot: 'release',
        appName: 'my-app',
      },
    }),
  ],
});
```

## root key

| Key | Type | Default | Summary |
| :-- | :--- | :------ | :------ |
| `muonPath` | `string` | bundled muon runtime | muon-core runtime directory used for development launch. |
| `cefPath` | `string` | automatic acquisition by muon-builder | CEF directory or CEF archive root used for development launch. |
| `stagePath` | `string` | `".muon/<public-target>"` | Directory where the muon runtime is staged for development launch. |
| `enableDebugger` | `boolean` | `true` | Enables CDP, the `F12` muon DevTools keybind, and the `Ctrl+F12` recycle keybind during development launch. |
| `allowInsecureLocalhost` | `boolean` | `false` | Ignores invalid localhost HTTPS certificate errors during development launch. |
| `exitWithServer` | `boolean` | `true` | Whether to stop the Vite dev server when muon-core exits normally. |
| `dev` | `object` | `{}` | Overrides applied only to the muon process launched by Vite development. |
| `pluginAccess` | `false \| object` | `plugin` setting from `muon.json` | Override settings for plugin API exposure and virtual module imports. |
| `build` | `boolean \| object` | `true` | Whether to generate distribution directories after `vite build`, or options used during generation. |

- `muonPath`, `cefPath`, `stagePath`, `open`, `enableDebugger`, `allowInsecureLocalhost`, and `exitWithServer` affect `vite dev`.
  `muon run` reads `muonPath`, `cefPath`, `stagePath`, `enableDebugger`, `allowInsecureLocalhost`, and `build.configPath`, and ignores `open` and `exitWithServer`.
  `vite build` ignores these development-launch options.
- `build: false` is an error for Vite-backed `muon run` launches when `--assets` is omitted.
  When `--assets` is specified explicitly, Vite-backed launch is not used and the specified assets are launched as before.
- When `muonPath`, `cefPath`, or `stagePath` is a relative path, it is resolved relative to the Vite project root.
- When `muonPath` is omitted, the bundled `runtime/<public-target>` from the installed muon package is used.
- When `cefPath` is omitted, muon-builder downloads and caches the tested CEF artifact based on the runtime information in `muonPath`.
- When `node.project` is configured, `vite dev` passes the project's normalized Node runtime requirement to muon-builder regardless of plugin mode.
  muon-builder prepares CEF and the required official Node.js runtime, placing Node.js at `runtimes/node/bin/node` under the stage directory (`runtimes/node/bin/node.exe` on Windows). When both downloads are needed, they run concurrently.
  There is no Vite plugin option for a Node.js executable path. The Node plugin starts this staged executable by its absolute path and does not fall back to an environment variable or `PATH`.
- When `stagePath` is omitted, `.muon/<public-target>` under the Vite project root is used.
- When `enableDebugger` is enabled, CDP is enabled through development-launch override settings, muon DevTools can be opened with `F12`, and muon can be recycle-restarted with `Ctrl+F12`.
  To enable muon DevTools in distribution builds, configure `cdp` and `browser.keybind` in `muon.json` instead of the Vite plugin argument.
- When `allowInsecureLocalhost` is enabled, Chromium's `--allow-insecure-localhost` switch is passed to the launched muon-core process.
  This is a localhost-only development workaround and cannot be enabled through a distribution launcher or `muon.json`.
- When `exitWithServer` is omitted or `true`, the Vite dev server also exits when muon-core exits normally.
  muon recycle restart does not stop the Vite dev server.

## dev key

`dev.config` supplies application `config` values only to the muon process launched with `vite dev`.
These values are written to the generated development configuration, which is loaded after the project `muon.json`.
Matching keys therefore override the project values, and other keys are added.

```ts
import { defineConfig } from "vite";
import muon from "muon-ui/vite";

export default defineConfig({
  server: {
    proxy: {
      "/api": {
        target: "http://127.0.0.1:5000",
      },
    },
  },
  plugins: [
    muon({
      dev: {
        config: {
          apiBaseUrl: "/api",
        },
      },
    }),
  ],
});
```

```json
{
  "config": {
    "apiBaseUrl": "https://api.example.com"
  }
}
```

Application code reads the selected value through `muon.environments.getConfigValues()`.
`dev.config` is ignored by `vite build`, `muon run`, distribution output, and installed packages; those launch forms use the regular `muon.json` value.
All `dev.config` values must be strings and are visible to renderer code, so they must not contain secrets.

## Starting only the Vite server

To start the normal Vite dev server without launching muon, specify `--no-muon` after `--`, Vite's end-of-options separator.

```console
vite dev -- --no-muon
```

Specify Vite options such as the port before `--`. Arguments after `--` are not interpreted as Vite options.

```console
vite dev --port 3000 -- --no-muon
```

`--no-muon` disables only development runtime preparation and the muon launch for `vite dev`, and takes precedence over `muon({ open: true })`. It therefore does not download or install a Node.js runtime or start the sidecar. Other muon Vite plugin behavior, including virtual modules, watch settings, and muon configuration loading, remains enabled, and the muon output entries in `.gitignore` are still updated. It does not affect `vite build`.

When the `package.json` script is `"dev": "vite dev"`, pass the end-of-options separator through npm as follows.

```console
npm run dev -- -- --no-muon
```

For a script dedicated to starting only the Vite server, the end-of-options separator can be included in the script itself.

```json
{
  "scripts": {
    "dev:vite": "vite dev --"
  }
}
```

```console
npm run dev:vite -- --no-muon
```

## pluginAccess key

`pluginAccess` has the same shape as the `plugin` setting in `muon.json`, and is used to override some settings from the Vite side.
When omitted, the `plugin` setting from `muon.json` is used as-is, and an omitted `plugin.mode` is treated as `"validate"`.
In `validate` mode, `window.muon` is not exposed, and plugin functions can be called only from permitted virtual module imports.
When `node.project` is configured, validate mode also prepares the Node.js runtime and starts a sidecar when `createNode()` is called through the `muon:node` virtual module. `muon:node` requires no `pluginAccess.plugins[].imports` entry or allow setting.
For the development server, the Node project and `engines.node` range are preflighted during Vite config resolution, and a failure may be reported as a warning. During a build, validation failures are errors.

```ts
muon({
  pluginAccess: {
    mode: "validate",
    pages: ["asset://main/**"],
    plugins: [
      {
        name: "internal",
        imports: [
          {
            sources: ["src/native/**"],
            allow: ["muon.executor.spawn"],
          },
          {
            packages: ["@example/trusted-muon-helper"],
            allow: ["muon.executor.spawn"],
          },
        ],
      },
    ],
  },
});
```

| Key | Type | Default | Summary |
| :-- | :--- | :------ | :------ |
| `mode` | `"validate" \| "simple"` | `plugin.mode` | Exposure method for plugin namespaces and functions. |
| `pages` | `readonly string[]` | `plugin.pages` | List of URLs where plugin namespaces and functions can be referenced from pages. |
| `plugins` | `readonly object[]` | `plugin.plugins` | List of plugins to enable and import permissions. |
| `plugins[].name` | `string` | none | Name of the plugin to enable. |
| `plugins[].allow` | `readonly string[]` | none | Allow list of function paths exposed in `simple` mode. |
| `plugins[].imports` | `readonly object[]` | none | Capability import allow list by importer used in `validate` mode. |
| `plugins[].imports[].sources` | `readonly string[]` | none | Importer path globs relative to the Vite project root. |
| `plugins[].imports[].packages` | `readonly string[]` | none | Exact list of NPM package names that the importer belongs to. |
| `plugins[].imports[].allow` | `readonly string[]` | none | Plugin function path globs allowed for that importer. |

- Each item is the same as `plugin` in `muon.json`. Defaults for undefined values fall back to each corresponding item in `muon.json`.

## build key

When `build` is `false`, only the normal Vite build is run and generation of muon distribution directories is disabled.
In this state, `muon build` and `muon pack` also fail, and distribution builds are not performed.
When `build` is an object, additional options are passed to the muon distribution build after `vite build`.
When `build` is `true` or omitted, it is treated as equivalent to `{}`.

For an application with `node.project`, `muon build` and `muon pack` embed the target platform's `node.so` or `node.dll`, `node-bridge.mjs`, the Node project, and the normalized Node runtime requirement into the output.
They do not include a Node.js executable, distribution archive, or download cache.
The distribution `muon-launcher` prepares CEF and the Node.js runtime required by `node.project` at startup.
For a portable distribution, preparation occurs directly in the extracted directory, and runtime Node.js is always started from `runtimes/node/bin/node` (`.exe` on Windows). There is no hot replacement of a running Node.js runtime and no fallback to an environment variable or `PATH`.

| Key | Type | Default | Summary |
| :-- | :--- | :------ | :------ |
| `targets` | `readonly string[]` | all supported targets | List of public target IDs to build. |
| `allTargets` | `boolean` | equivalent to `true` when `targets` is omitted | Whether to build all targets supported by the installed package. |
| `appName` | `string` | `name` from `package.json` | Filename of the application launcher. |
| `appId` | `string` | `name` from `package.json` | Base ID of the runtime application identifier. Windows targets embed `<appId>.<arch>`. |
| `outputRoot` | `string` | `"."` | Parent directory where target-specific output directories such as `dist-muon/linux-amd64/` are created. |
| `configPath` | `string` | automatic search | muon configuration file embedded into the runtime and launcher. |
| `iconPath` | `string` | `muon.json` or muon default icon | PNG file used as the static application icon. |
| `distributionFiles` | `readonly string[]` | `files` from `package.json` | File list additionally copied to the root of the distribution directory. |
| `windowsResource` | `object` | `windows.resource` | Resource metadata embedded into the Windows launcher and NSIS installer/uninstaller. |
| `windowsCodeSigning` | `object \| false` | `windows.codeSigning` | External code-signing command for Windows executables. |
| `linuxDesktop` | `object` | `linux.desktop` | Metadata for Linux desktop entries and icons. |
| `packageDirectory` | `string` | installed muon package | muon package directory containing `runtime/` and `native/`. |

- When both `targets` and `allTargets` are omitted, all targets supported by the installed muon package are generated.
  If `allTargets` is `true`, it takes precedence over `targets`.
  `targets` can specify one of `linux-amd64`, `linux-armhf`, `linux-arm64`, `windows-i686`, or `windows-amd64`.
- When `appName` is omitted, it is generated from `name` in `package.json`.
  If `name` does not exist, `muon-app` is used.
  For scoped package names, the scope is removed, and characters that cannot be used in launcher names are normalized to `-`.
  On Windows targets, `.exe` is added automatically.
- When `appId` is omitted, the base ID is also generated from `name` in `package.json`.
  `@scope/name` becomes `scope.name`, and other unusable characters are normalized to `-`.
  On Linux targets, the generated value is embedded into `muon-core` and the launcher as `launcher.appId`.
  On Windows targets, `windows-amd64` embeds `<appId>.amd64`, and `windows-i686` embeds `<appId>.i686`.
- When `outputRoot` or `configPath` is a relative path, it is resolved relative to the Vite project root.
  When `configPath` is omitted, `muon.json5`, `muon.jsonc`, and `muon.json` are searched from the Vite project root in that order.
  If no configuration file exists, it is treated as equivalent to `{}`.
- `iconPath` is resolved relative to the Vite project root.
  `windowsResource.iconPath` and `linuxDesktop.iconPath` are overrides applied only to their respective targets.
- When `distributionFiles` is omitted, `files` from `package.json` is used as the candidate list.
  When specified, it takes precedence over `files` in `package.json`, and an empty array is treated as no additional files.
  Candidate paths must be relative paths. Absolute paths cannot be used.
  Only regular files are copied, and the copy destination is the `basename` directly under the target-specific distribution directory.
  Directories, globs, and npm implicit include/exclude rules are not expanded.
  Paths under the resolved `asset.sourcePath`, `node_modules`, `.git`, and output distribution directories are excluded.
  It is an error when multiple candidates would be copied to the same `basename`.
- In builds through the Vite plugin, Vite's `build.outDir` is used as the asset source, and assets inside the ZIP use the `main/` prefix.
  Therefore, built assets can be referenced from `asset://main/`.
- `windowsResource` accepts the same keys as `windows.resource` in `muon.json` and is handled with the same priority as CLI `--windows-*` options.
- `windowsCodeSigning` accepts the same keys as `windows.codeSigning` in `muon.json`. Specifying `false` disables the signing settings from `muon.json`.
- `linuxDesktop` accepts the same keys as `linux.desktop` in `muon.json` and is handled with the same priority as CLI `--linux-*` options.

> Note: `packageDirectory` is an argument intended for tests and package verification.
> Its explanation is omitted.
