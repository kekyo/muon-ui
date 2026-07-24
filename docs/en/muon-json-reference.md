# muon.json reference

`muon.json` determines muon's behavior, and some features can only be decided by this file.
In particular, whitelists that permit behavior cannot be changed programmatically.

Configuration files are searched in the order `muon.json5`, `muon.jsonc`, and `muon.json`.

> Note: Regardless of filename extension, all of these files are parsed as JSON5.

When launching from the muon Vite plugin (`vite dev`), if the configuration file does not exist or cannot be parsed, a warning is shown and the project starts with all settings at their defaults.
With `muon run`, if the configuration file does not exist, muon starts with only the generated development settings.
If an existing file cannot be read or parsed, it is an error.

On the other hand, `muon build` treats a missing configuration file as all defaults, but if an existing file cannot be read or parsed, it is a build error.
It is also an error when a configuration file explicitly specified by `--config` does not exist.

The following is an example of `muon.json`:

```json
{
  "iconPath": "icons/app.png",
  "config": {
    "apiBaseUrl": "https://api.example.com"
  },
  "browser": {
    "initialWindowState": "normal",
    "backgroundColor": "system",
    "titleBarType": "muon",
    "initialTitleBarVisibility": true,
    "keybinds": {
      "devtools": "f12",
      "zoomIn": "ctrl+plus",
      "zoomOut": "ctrl+minus"
    }
  },
  "network": {
    "allow": [
      "asset://main/**",
      "https://img.examples.com/images/**"
    ]
  },
  "plugin": {
    "mode": "validate",
    "pages": ["asset://main/**"],
    "plugins": [
      {
        "name": "internal",
        "config": {
          "fs.readFile.maxBytes": "67108864",
          "fs.readTextFile.maxBytes": "67108864"
        },
        "imports": [
          {
            "sources": ["src/native/**"],
            "allow": [
              "muon.environments.getConfigValues",
              "muon.environments.getCommandLine"
            ]
          },
          {
            "packages": ["@example/trusted-muon-helper"],
            "allow": ["muon.fs.readTextFile"]
          }
        ]
      },
      {
        "name": "foobar",
        "config": {
          "foobar.mode": "strict"
        },
        "imports": [
          {
            "sources": ["src/native/**"],
            "allow": ["foobar.native.*"]
          }
        ]
      }
    ]
  },
  "cdp": {
    "enable": true,
    "port": 9222
  }
}
```

## Top-level keys

| Key | Type | Default | Summary |
| :-- | :--- | :------ | :------ |
| `iconPath` | `string` | muon default icon | PNG file used as the static application icon. |
| `config` | `readonly object` | `{}` | Application string key-value settings available to renderer code. |
| `node` | `object` | none | Optional Node.js sidecar project. |

- `iconPath` accepts only `.png`. Relative paths are resolved from the directory where `muon.json` is located.
- `iconPath` is the shared source for Windows PE/NSIS, Linux desktop, and the startup title-bar icon.
  Internally, it is placed at `asset://main/.muon/app-icon.png"`.
  Therefore, if an existing asset has the same entry, the build fails.
- To use separate icons only for Windows or only for Linux, specify `windows.resource.iconPath` or `linux.desktop.iconPath` respectively as overrides.
- To change only the title-bar icon of a normal browser window at runtime, use `window.muon.browser.setTitleBarIcon()`.

## config key

`config` stores application settings as string key-value pairs. Keys and values must both be strings, and muon does not assign meaning to their contents.
The muon app reads the effective object with `muon.environments.getConfigValues()`.

When muon app starts with multiple configuration files, their `config` objects are merged in command-line order.
A later value replaces an earlier value with the same key, and keys that do not conflict are added.
There is no deletion syntax, so a later configuration cannot remove an earlier key.

```json
{
  "config": {
    "apiBaseUrl": "https://api.example.com",
    "environmentName": "production"
  }
}
```

Because all values are available to renderer code, do not store passwords, tokens, or other secrets in `config`.
`plugin.plugins[].config` is a separate setting table passed only to the corresponding native plugin during initialization.

## node key

Set `node.project` to the directory of an ordinary Node.js project to enable the out-of-process Node.js sidecar. A relative path is resolved from the directory of the configuration file that defines it; an absolute path is also accepted.

```json
{
  "node": {
    "project": "./backend"
  },
  "plugin": {
    "mode": "simple"
  }
}
```

When `node` is present, `project` must be a non-empty string. It refers to an ordinary Node.js project: manage `type`, `main`, `exports`, `imports`, `dependencies`, and `engines.node` in `package.json` using normal Node.js conventions. No muon-specific extension is required in the Node project's `package.json`.

When `engines.node` is specified, muon normalizes its intersection with the Node bridge compatibility range and embeds it in the artifact as the internal `launcher.nodeRuntime` value. When it is omitted, the Node bridge compatibility range is used and the newest matching LTS release is selected. If none matches, preparation fails without falling back to a non-LTS release. When `engines.node` is specified, muon selects the greatest matching LTS release, or the greatest matching release of any kind when no LTS release matches. The launcher uses the official Node.js `https://nodejs.org/dist/index.json` and the selected version's `SHASUMS256.txt`, verifies the archive with SHA-256, and prepares it under `runtimes/node/`.

`muon build` and `muon pack` include the Node project, `node.so` or `node.dll`, `node-bridge.mjs`, and the normalized runtime requirement. They do not include a Node.js executable, distribution archive, or download cache. When the runtime requirement is required for execution, the executable is prepared before development startup, by `muon run`, or by the distribution launcher. Only `runtimes/node/LICENSE` and `runtimes/node/bin/node` (`node.exe` on Windows) are installed; npm and Corepack are omitted. The Node plugin uses only the absolute path to this executable, with no environment-variable or `PATH` fallback.

The MVP's `window.muon.node` API requires `plugin.mode: "simple"`. Validate mode loads metadata without exposing the API, downloading or installing a Node.js runtime, starting the Node sidecar, or executing project code. The reserved `node` plugin cannot be configured directly through `plugin.plugins[]`. See [Using the Node.js sidecar](./nodejs-sidecar.md) for development, build, distribution, runtime selection, API, and value restrictions.

## browser key

| Key | Type | Default | Summary |
| :-- | :--- | :------ | :------ |
| `startPage` | `string` | `"asset://main/index.html"` | URL loaded first at startup. |
| `profile` | `string` | state profile | Directory where the Chromium profile is saved. |
| `initialWindowState` | `string` | `"normal"` | Window state at startup. |
| `backgroundColor` | `string` | `"system"` | Browser background color before page load or when the page does not specify a background color. |
| `titleBarType` | `string` | `"muon"` | Title-bar implementation for normal browser windows. |
| `contextMenu` | `object` | `{"mode":"standard"}` | Native context menu settings for the page area. |
| `initialTitleBarVisibility` | `boolean` | `true` | Whether to show the muon custom title bar at startup. |
| `keybind` | `object` | `{}` | Keyboard shortcuts assigned to browser operations. |
| `allowUnsafeJavaScriptParentAccess` | `readonly string[]` | `[]` | List of URLs that allow JavaScript access from popup pages to their parent page. |

- When `profile` is a relative path, it is resolved relative to `muon.json`.
  If `profile` is not explicitly set, `<appId>/profile/` under the user state directory is used.
- `initialWindowState` can be `"normal"`, `"hidden"`, `"minimized"`, `"maximized"`, or `"fullscreen"`.
  However, the final display state may be adjusted by the OS or window manager.
- `backgroundColor` can be `"system"` or an RGB hexadecimal value in `"RRGGBB"` / `"#RRGGBB"` format.
  `"system"` is applied as black or white when the OS light/dark setting can be read, and otherwise uses CEF's default value.
- `titleBarType` can be `"muon"` or `"native"`.
  `"muon"` uses the theme-aware title bar provided by libmuon-ui, while `"native"` uses OS/window-manager native decorations.
  On Linux, when `"native"` is specified, X11 delegates decoration to the window manager.
  If muon decides that native decorations cannot be used, such as on Wayland, it logs a warning and falls back to behavior equivalent to `"muon"`.
  With the `"muon"` title bar, specifying `-webkit-app-region: drag` in CSS for any element in the page lets that area drag the window.
  Specify `-webkit-app-region: no-drag` for elements that should receive normal page operations, such as links, buttons, and input fields.
- `contextMenu.mode` can be `"standard"`, `"disabled"`, or `"custom"`.
  `"standard"` shows CEF's standard menu items such as copy and paste, and adds registered muon custom items.
  `"disabled"` does not show the native context menu, including both standard and custom items.
  `"custom"` removes CEF standard items and shows only registered muon custom items.
- `initialTitleBarVisibility` specifies whether to show the normal browser window title bar initially.
  If `false`, the title bar is hidden immediately after startup.
- The startup title-bar icon is generated from `iconPath` during the distribution build.
  If the page specifies a favicon, muon tries favicon URLs reported by CEF in order and applies the first image that can be fetched and converted.
  On page navigation, if no favicon exists or if fetching/conversion fails, it returns to the generated initial title-bar icon or the built-in muon icon.
  Fetching favicon URLs is subject to the same network restrictions as normal page requests.
  Favicon responses are limited to 1 MiB regardless of image format. PNG images must also have width and height of 256 px or less and a total pixel count of 65,536 or less. Candidates exceeding these limits are not used, and muon falls back to the next candidate or the initial icon.
  These limits apply only to icon decoding paths for title-bar, application, and tray icons. Normal page `<img>` elements and similar content are left to CEF image processing.
  The `"muon"` title bar can use image formats the browser can display, such as SVG, while the `"native"` title bar can use only images loadable as PNG.
  On Linux with `"native"`, title-bar-related settings may not be reflected correctly.
- `keybind` can specify `devtools`, `reload`, `hardReload`, `fullscreen`, `zoomIn`, `zoomOut`, `resetZoom`, and `recycle`.
  Values are strings that connect modifiers and keys with `+`, such as `"ctrl+shift+i"`.
  Modifiers can be `shift`, `ctrl`/`control`, `alt`, and `meta`/`cmd`/`command`/`super`.
  Keys can be `f1` through `f24`, `a` through `z`, `0` through `9`, `plus`, `equal`, `minus`, `backspace`, `tab`, `enter`/`return`, `escape`/`esc`, `space`, `insert`, `delete`/`del`, `home`, `end`, `pageup`, `pagedown`, `left`, `right`, `up`, and `down`.
  Empty strings, modifier-only definitions, and duplicate shortcuts are configuration errors.
- `allowUnsafeJavaScriptParentAccess` is a compatibility setting that reduces safety. Usually, you should not specify this item.
  When omitted or an empty array, JavaScript access from popup pages to parent pages is not allowed.
  In this case, popups are opened as independent windows equivalent to `noopener`, and `window.open()` returns `null`.
  If the page specifies `noopener` or `noreferrer`, `window.opener` is also `null` regardless of the allow list.

## linux key

`linux.desktop` is Linux desktop entry metadata for distribution builds.
This setting is used only at build time by `muon build` and `muon pack`, and is excluded from runtime settings embedded into `muon-core` or the launcher.

```json
{
  "linux": {
    "desktop": {
      "desktopId": "com.example.my-app",
      "name": "My App",
      "comment": "Example app",
      "iconPath": "icons/app.png",
      "categories": ["Utility"],
      "startupNotify": true
    }
  }
}
```

| Key | Type | Default | Summary |
| :-- | :--- | :------ | :------ |
| `desktop.desktopId` | `string` | `appId` | `.desktop` filename, `StartupWMClass`, Wayland app ID, and X11 WM_CLASS. |
| `desktop.name` | `string` | `package.json` name | Application name displayed in the launcher. |
| `desktop.comment` | `string` | `package.json.description` | `Comment` in the desktop entry. |
| `desktop.iconPath` | `string` | `iconPath` or muon default icon | PNG icon override used only for Linux targets. |
| `desktop.categories` | `string[]` | `["Utility"]` | Desktop menu category. |
| `desktop.startupNotify` | `boolean` | `true` | `StartupNotify` in the desktop entry. |

- `desktop.iconPath` accepts only `.png`. The input PNG is normalized during the build and placed as `muon-desktop-icon.png` in the Linux distribution directory.
- When launched from a portable distribution (`.tar.gz`), `muon-launcher` prepares CEF directly under the extracted `<packageName>/<target>` directory, places the CEF profile in `profile/` under the same directory, and creates or updates `~/.local/share/applications/<desktopId>.desktop`.
  The `Exec`, `TryExec`, and `Icon` values in this desktop entry point to absolute paths under the extracted directory.
- When restarted from the same extracted location, the prepared CEF is reused. When launched from a distribution extracted to another directory, CEF preparation and desktop entry update are performed for that location.
- `muon pack --type deb` generates `/usr/share/applications/<desktopId>.desktop` and `/usr/share/icons/hicolor/256x256/apps/<desktopId>.png`.
  Runtimes installed by deb contain `muon-install.json`, and `muon-launcher` does not create a new desktop entry in the user's home directory.
  Only when an existing muon-managed user desktop entry exists, it is updated to a deb-aware entry with `TryExec=/usr/bin/<packageName>`.
- Relative paths are resolved from the directory of the file where the value is defined.
  CLI/Vite options use the project root, and `muon.json` uses the configuration file directory.
- Linux icon resolution order is CLI/Vite option `linuxDesktop.iconPath` or `--linux-icon`, `linux.desktop.iconPath` in `muon.json`, unified `iconPath`, `project.json.iconPath`, then the muon default icon.
- Linux desktop metadata resolution order is, for each field, CLI/Vite option, `linux.desktop` in `muon.json`, `package.json`, then the default value.

## windows key

`windows.resource` is Windows PE/NSIS resource metadata for distribution builds.
This setting is used only at build time by `muon build` and `muon pack`, and is excluded from runtime settings embedded into `muon-core` or the launcher.
`windows.codeSigning` is also used only at build time and is not embedded into runtime settings.

```json
{
  "windows": {
    "resource": {
      "iconPath": "icons/app.png",
      "productName": "My App",
      "fileDescription": "My App",
      "companyName": "Example Inc.",
      "version": "1.2.3",
      "copyright": "Copyright Example Inc."
    },
    "codeSigning": {
      "command": "signtool",
      "args": ["sign", "/fd", "SHA256", "{path}"],
      "targets": ["runtime", "launcher", "nsisInstaller", "nsisUninstaller"]
    }
  }
}
```

| Key | Type | Default | Summary |
| :-- | :--- | :------ | :------ |
| `resource.iconPath` | `string` | `iconPath` or muon default icon | PNG icon override used only for Windows targets. |
| `resource.productName` | `string` | `package.json` name | `ProductName` in the Windows version resource. |
| `resource.fileDescription` | `string` | `package.json.description` | `FileDescription` in the Windows version resource. |
| `resource.companyName` | `string` | `package.json.author` | `CompanyName` in the Windows version resource. |
| `resource.version` | `string` | `package.json.version` | `FileVersion`/`ProductVersion`. Fixed values are normalized to four elements. |
| `resource.copyright` | `string` | `package.json.copyright` | `LegalCopyright` in the Windows version resource. |
| `resource.language` | `number` | `1033` | Language ID for the version resource and icon resource. |
| `resource.codePage` | `number` | `1200` | Code page for the version resource. |
| `codeSigning.command` | `string` | none | External signing command executed for each signing target file. |
| `codeSigning.args` | `string[]` | none | Arguments for the signing command. `{path}` is required. |
| `codeSigning.targets` | `string[]` | all signing targets | Select from `runtime`, `launcher`, `nsisInstaller`, and `nsisUninstaller`. |

- `resource.iconPath` accepts only `.png`. muon automatically generates the `.ico` file required by Windows PE/NSIS during the build.
- Relative paths are resolved from the directory of the file where the value is defined.
  CLI/Vite options use the project root, `muon.json` uses the configuration file directory, and `project.json` uses the project root.
- Windows icon resolution order is CLI/Vite option `windowsResource.iconPath` or `--windows-icon`, `windows.resource.iconPath` in `muon.json`, unified `iconPath`, `project.json.iconPath`, then the muon default icon.
- Windows resource metadata resolution order is, for each field, CLI/Vite option, `windows.resource` in `muon.json`, `project.json`, `package.json`, then the default value.
  However, when `--package-version` is specified for `muon pack`, `resource.version` uses the `--package-version` value in the position of `package.json.version`.
  Explicit Windows resource versions from `--windows-version`, `muon.json`, or `project.json` still take precedence over `--package-version`.
- Windows code signing resolution order is CLI/API option, Vite plugin `build.windowsCodeSigning`, then `windows.codeSigning` in `muon.json`.
  `command` is not a signing implementation provided by muon, but a `signtool` or signing wrapper script supplied by CI or the developer environment.
  In `args`, `{path}` is replaced with the signing target file path, `{target}` with a muon target such as `windows-amd64`, and `{kind}` with the signing target kind.
- If `version` is `1.2.3`, the PE fixed value and NSIS `VIProductVersion` / `VIFileVersion` become `1.2.3.0`.
  The original `1.2.3` is used for the string versions of `FileVersion` / `ProductVersion`.
- `muon build` directly updates PE file resources with `muon-builder resource` after embedding the launcher configuration.
  Code-signing signatures for `runtime` and `launcher` are executed after this PE update.
- `muon pack --type nsis` outputs `Icon`, `UninstallIcon`, `VIProductVersion`, `VIFileVersion`, and `VIAddVersionKey` to the NSIS script from the same resolved metadata.
  To align display information for the setup executable and `Uninstall.exe`, NSIS directives are used instead of PE post-processing for NSIS.
  Code signing for `nsisInstaller` and `nsisUninstaller` is also executed through NSIS `!finalize` / `!uninstfinalize`.
  NSIS `Name` / `DisplayName` / installer path / uninstall registry key / state deletion target are separated by Windows architecture, while Windows resource metadata such as `ProductName` and artifact filenames keep the same metadata rules.

## asset key

| Key | Type | Default | Summary |
| :-- | :--- | :------ | :------ |
| `sourcePath` | `string` | `assets` | Asset directory or ZIP file exposed as `asset://` URLs. |

- When `sourcePath` is omitted, `assets` in the same directory as the executable is used.
  Relative paths are resolved relative to `muon.json`.
  If a directory is specified, its contents can be referenced as `asset://main/...`.
  If a ZIP file is specified, the contents inside the ZIP can be referenced as `asset://main/...`.

> Note: `signature` and `salt`, which are not listed here, are values automatically calculated and inserted during `muon build` or `muon pack`.
> Their explanation is omitted.

## network key

| Key | Type | Default | Summary |
| :-- | :--- | :------ | :------ |
| `allow` | `readonly string[]` | `["asset://**", "data:image/**"]` | List of URL patterns allowed to load. |
| `authorizedOrigin` | `readonly object[]` | `[]` | Allows additional network access for requests originating from specified origins. |
| `localAccess.loopbackOrigins` | `readonly object[]` | `[]` | Origins granted permission to access the loopback network. |
| `localAccess.localNetworkOrigins` | `readonly object[]` | `[]` | Origins granted permission to access the local network. |

- `allow` is a whitelist.
  If an empty array is specified, all network access, including local assets, is disallowed.
  `data:` protocol URLs are also subject to this list. The default permits image data through `data:image/**`; an explicit `allow` array replaces that default, so include the pattern when it is still needed.
  `data:**` also includes `data:text/html,...`, so if HTML generated from untrusted input is loaded, it may lead to arbitrary HTML or JavaScript execution, UI spoofing, or access to allowed networks.
  URL patterns can use `*` and `**`.
  `*` does not cross `:`, `/`, `?`, or `#` separators, while `**` matches all following characters.
  Pattern matching is case-sensitive.
- Each `authorizedOrigin` element must specify `scheme` and `domain`, and should specify `port` only when necessary.
  - `scheme` and `domain` are normalized to lowercase.
  - `domain` cannot contain `:`, `/`, `?`, `#`, or `*`.
  - If `port` is specified, it must be an integer from `1` through `65535`.
  - This setting behaves similarly to privilege delegation, so specify only origins that can be trusted as request origins, such as trusted authentication providers.
- Elements of `localAccess.loopbackOrigins` and `localAccess.localNetworkOrigins` use the same exact `scheme`, `domain`, and `port` matching.
  These lists control only the Local Network Access permission requested by CEF, so destination URLs must also be allowed by `allow`.
  The combined legacy Local Network Access permission is accepted only when the same origin is present in both lists.

## plugin key

| Key | Type | Default | Summary |
| :-- | :--- | :------ | :------ |
| `path` | `string` | `"./plugins"` | Directory searched for external plugin files. |
| `mode` | `"validate" \| "simple"` | `"validate"` | Exposure method for plugin namespaces and functions. |
| `pages` | `readonly string[]` | `["asset://main/**"]` | List of URLs where plugin namespaces and functions can be referenced from pages. |
| `plugins` | `readonly object[]` | `[]` | List of plugins to enable. |
| `plugins[].name` | `string` | none | Name of the plugin to enable. |
| `plugins[].config` | `readonly object` | `{}` | String key-value settings passed during plugin initialization. |
| `plugins[name="internal"].config["fs.readFile.maxBytes"]` | `string` | `"67108864"` | Read limit in bytes per `muon.fs.readFile` call. |
| `plugins[name="internal"].config["fs.readTextFile.maxBytes"]` | `string` | `"67108864"` | Raw byte read limit per `muon.fs.readTextFile` call. |
| `plugins[].allow` | `readonly string[]` | none | Allow list of function paths exposed in `simple` mode. |
| `plugins[].imports` | `readonly object[]` | none | Allow list by import source used in `validate` mode. |
| `plugins[].imports[].sources` | `readonly string[]` | none | Importer path globs relative to the project root. |
| `plugins[].imports[].packages` | `readonly string[]` | none | Exact list of NPM package names that the importer belongs to. |
| `plugins[].imports[].allow` | `readonly string[]` | none | Plugin function path globs allowed by that import rule. |

- When `path` is a relative path, it is resolved relative to `muon.json`.
- When `mode` is `"validate"`, plugin functions can be called only from capability-bearing virtual module imports generated by a bundler such as Vite.
  `"simple"` exposes `window.muon` and the namespace objects under it to pages as before.
- `pages` controls only whether the plugin API bridge is injected into a page.
  In `"validate"`, the non-enumerable bridge for capability calls is injected. In `"simple"`, the `window.muon` hierarchy is injected.
  Whether pages and subresources can actually be loaded must be separately allowed with `network.allow`.
- When `plugins` is omitted, no plugin functions are exposed.
  To enable built-in plugins, specify `"internal"` for `name`.
  To enable external plugins, specify the filename without extension as `name`.
  For example, when using `muon_fs_dialogs_gtk3.so`, the `name` is `"muon_fs_dialogs_gtk3"`.
  `"internal"` is reserved and cannot be used as an external plugin name.
  The same `name` cannot be specified more than once.
- `plugins[].config` is the setting table passed to that plugin during initialization.
  When omitted or `{}`, it is treated as an empty setting.
  Keys cannot be empty strings, and neither keys nor values can contain NUL characters.
  Values can only be strings. Empty strings and strings containing newlines can be specified, but objects, arrays, numbers, booleans, and null are configuration errors.
  The meaning and interpretation of multi-line values, glob/regular-expression separators, and similar formats are each plugin's responsibility.
- The built-in plugin settings `fs.readFile.maxBytes` and `fs.readTextFile.maxBytes` specify the raw byte read limit per call for `muon.fs.readFile` and `muon.fs.readTextFile`, respectively.
  Values are unsigned decimal integer strings consisting only of ASCII digits, and the entire string must fit within `uint64_t`. Leading zeros are allowed.
  Empty strings, signs, whitespace, decimal points, exponential notation, unit suffixes, and overflow are invalid.
  Both default to 64 MiB (`67108864` bytes) when omitted. `"0"` is valid: `readFile()` allows only empty ranges, and `readTextFile()` allows only empty sources.
  Invalid values do not fall back to defaults. They fail built-in plugin initialization with `must be an unsigned decimal byte count` after the relevant key name.
  Settings are fixed during plugin initialization and cannot be changed while running. They are reflected after the next launch or recycle.
  The two settings are independent and apply only to their corresponding functions. They are not quotas accumulated across multiple operations. `readTextFile()` allows sources exactly at the limit, rejects oversized sources without truncation, and performs this check before UTF-8/NUL validation.
- `plugins[].allow` is a whitelist for exposing the plugin's function paths to the `window` hierarchy in `simple` mode.
  It is required in `simple` mode and cannot be specified in `validate` mode.
  Patterns such as `muon.fs.*` can be specified.
  `*` does not cross `.` separators, while `**` matches all following characters.
  Pattern matching is case-sensitive.
  To make functions callable from a page, API bridge injection for the target page must also be allowed with `plugin.pages`.
- `plugins[].imports` is import permission for `validate` mode.
  When specifying plugin entries in `validate` mode, it is required and cannot be an empty array. It cannot be specified in `simple` mode.
  `sources` is a glob for importer paths relative to the Vite project root, and `packages` is an exact match against the `name` found by searching for the nearest `package.json` from the importer.
  The project root's own `package.json` is not a target for `packages` checks and is treated as ordinary source files.
  One or both of `sources` and `packages` can be specified. If both are specified, a match in either one is allowed.
  An import rule that specifies neither is a configuration error.
- `plugins[].imports[].allow` is the function path allowed for that import source in `validate` mode.
  It is required and cannot be an empty array.
  It can use the same glob syntax as `plugins[].allow`, and the specified glob is passed directly to runtime-side permission validation.
  On the other hand, when generating named exports for virtual modules such as `muon:executor`, the Vite side expands them to concrete function paths.
  Built-in plugin function lists are expanded from the catalog held by muon-ui. External plugin wildcards are collected during Vite dev/build by loading the plugin with `muon-plugin-inspector` and reading metadata from `muon_init_plugin()`.
  The inspector uses `plugins[].config` and `signature`/`salt` for initialization and signature verification, but it does not call the bodies of exposed functions.
  If a wildcard cannot be expanded to any concrete named export, Vite reports an error.

> Note: `capabilities`, `signature`, and `salt`, which are not listed here, are values automatically calculated and inserted during `muon build` or `muon pack`.
> Their explanation is omitted.

## log key

| Key | Type | Default | Summary |
| :-- | :--- | :------ | :------ |
| `level` | `string` | `"info"` | Baseline log level for all log sources. |
| `output.type` | `string` | `"stderr"` | Log output destination. |
| `output.path` | `string` | none | Output file path used when `output.type` is `"file"`. |
| `sources.muon` | `string` | `"info"` | Log level for muon itself. |
| `sources.cef` | `string` | `"warning"` | Log level for internal CEF/Chromium logs. |
| `sources.console` | `string` | `"debug"` | Log level for JavaScript console output. |
| `sources.plugin` | `string` | `"info"` | Log level for native plugin output. |

- Log levels can be `"debug"`, `"info"`, `"warning"`/`"warn"`, `"error"`, `"fatal"`, or `"off"`.
Logs are not output for sources set to `off`.
- When `level` is specified, every log source baseline is set to that value.
  Then, if `muon`, `cef`, `console`, or `plugin` are specified in `sources`, only those sources are overridden individually.
  When only `sources` is specified, unspecified sources are first aligned to the current `level` and then overridden individually.
  Therefore, if you want to keep the defaults `cef: "warning"` or `console: "debug"`, specify them explicitly in `sources` as needed.
- `output.type` can be `"stdout"`, `"stderr"`, or `"file"`.
  On POSIX environments, `"syslog"` can also be used.
  On Windows environments, `"debug"` and `"eventlog"` can also be used.
- When `output.type` is `"file"`, `output.path` is required.
- Relative paths are resolved relative to `muon.json`, and parent directories are created as needed.
  File output appends to the file.
- When `output.type` is not `"file"`, specifying `output.path` is a configuration error.

## cdp key

| Key | Type | Default | Summary |
| :-- | :--- | :------ | :------ |
| `enable` | `boolean` | `false` | Enables Chrome DevTools Protocol. |
| `port` | `number` | `9222` | Listening port number for DevTools Protocol. |

- Setting `cdp.enable` to `true` allows external DevTools and CDP clients to connect.
  This is a development/debugging setting, so enable it in distribution builds only when necessary.
- `port` must be an integer from `1024` through `65535`.

## launcher key

| Key | Type | Default | Summary |
| :-- | :--- | :------ | :------ |
| `defaultVersionPolicy` | `string` | `"tested"` | CEF version policy used when `versionPolicy` is not saved in `muon-launcher.ini`. |

> Note: `appId`, which is not listed here, is automatically calculated and inserted during `muon build` or `muon pack`.
> When `node.project` is present, the normalized requirement generated from the Node project and Node bridge is also inserted as the internal `nodeRuntime` value. Specifying `launcher.nodeRuntime` as a user setting is a build error.
