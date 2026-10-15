# muon CLI (Advanced topics)

For Android `prepare`, `build`, and `pack --type apk`, see [building and distributing Android apps](./android.md). The instructions below describe desktop builds; `--all` also selects desktop targets only.

muon is basically intended to be used with the Vite plugin, but it can also be developed without the Vite plugin.
In that case, use the `muon` CLI command.

## Direct launch with muon run

If you want to open an already generated local asset directory directly with muon without using the Vite development server, use `muon run`:

```bash
npx muon run
```

- `muon run` does not start the Vite development server. Therefore, HMR does not work with `muon run`.
- When Vite `server.proxy` contains one or more entries, `muon run` writes a warning to stderr because that proxy is available only through the Vite development server and is not used by the launched app.
- When `--assets` is specified explicitly, the specified local assets are launched directly as before.
- When `--assets` is omitted and `vite.config.*` contains exactly one muon Vite plugin, `muon run` runs `vite build` before startup.
  It reads Vite's `build.outDir` and `base`, places the build output under `.muon/run/assets/main/`, and launches muon with `.muon/run/assets` as `asset.sourcePath`.
  For example, with `base: "/foo/"`, output is placed in `.muon/run/assets/main/foo/`, and if `muon.json` has no `browser.startPage`, muon opens `asset://main/foo/index.html`.
- If Vite or the muon Vite plugin is absent, assets are resolved in the order `--assets`, `asset.sourcePath` in `muon.json`, then `assets/`.
  Note that `asset://main/index.html` refers to `assets/main/index.html`. The URL host portion is treated as a subdirectory.
- When `vite.config.*` contains exactly one muon Vite plugin, `muon run` reads `muonPath`, `cefPath`, `stagePath`, `enableDebugger`, `allowInsecureLocalhost`, and `build.configPath`.
- CLI options take precedence when the same items are specified, and `open` is ignored by `muon run`.
  `build: false` is an error for Vite-backed launch when `--assets` is omitted, but when `--assets` is specified explicitly, the specified assets are launched as before.
- For `allowInsecureLocalhost`, the precedence is `--allow-insecure-localhost`, an explicitly specified Vite plugin option, `network.localAccess.allowInsecureLocalhost` in `muon.json`, then `false`.
  The CLI flag only enables the setting; there is no negative CLI form.
- Specify `--no-debugger` to disable development defaults for muon DevTools, the recycle keybind, and CDP.
- Specify `--allow-insecure-localhost` to ignore localhost HTTPS certificate errors during development.
  Passing this raw flag directly to a distribution muon-launcher is still rejected before startup.
  Configure `network.localAccess.allowInsecureLocalhost` in `muon.json` when the behavior is required in a distribution build.
- Before launch, `muon run` invokes muon-builder to prepare CEF and, when `node.project` is configured, the required official Node.js runtime. Node.js runtime preparation applies in both `simple` and `validate` modes.
  When both downloads are needed, they run concurrently, and Node.js is placed at `runtimes/node/bin/node` under the stage directory (`runtimes/node/bin/node.exe` on Windows).
  There is no CLI option for a Node.js executable. The Node plugin uses only this absolute path and does not fall back to an environment variable or `PATH`.

Unlike when using the Vite plugin, you can naturally split page management by asset host name.
For example, CEF treats `asset://main/index.html` and `asset://sub/index.html` as different origins.
This can also be applied to advanced security isolation and separation of filters through muon's whitelist.
See the local asset permissions chapter for details.

> Note: Asset host name separation also works when using the Vite plugin.
> However, by default all content files output by Vite are placed under `main/`.
> To use different asset host names, you need to configure them appropriately through the Vite build process or manually.

## Distribution builds

Distribution builds can also be generated from the `muon` CLI.

```bash
npx muon build
```

When `vite.config.*` contains the muon Vite plugin, `muon build` reads the arguments to `muon({ ... })` and uses the same build settings as `vite build`.
However, CLI options override those parameters.

When the muon Vite plugin is not configured, `muon build` does not automatically run an npm script or `vite build` for content builds.
Instead, it collects existing assets into distribution directories.
In this case, asset sources are resolved in the same way as `muon run`: `--assets`, `asset.sourcePath` in `muon.json`, then `assets/`.

`asset.sourcePath` is treated as a path relative to the directory where the configuration file is placed, or as an absolute path.
If the asset source is a directory, it is packed into `assets.zip`.
If it is a ZIP file, it is copied as `assets.zip` in the distribution destination and signed.
Also, if regular files such as `README.md` and `LICENSE` are specified in `files` in `package.json`, files that satisfy the conditions are copied directly under the distribution destination directory.
If the muon Vite plugin's `build.distributionFiles` is specified, that list is used instead of `files` in `package.json`.

For an application with `node.project`, `muon build` and `muon pack` copy the target platform's `node.so` or `node.dll`, `node-bridge.mjs`, and the Node project into the output, and embed the normalized Node runtime requirement into the launcher.
They do not include a Node.js executable, distribution archive, or download cache in build or pack output.
The distribution `muon-launcher` prepares CEF and the required Node.js runtime at startup. A normal installation places them in a runtime directory separate from the application root; a portable distribution places them directly in the extracted directory, so the prepared portable runtime also contains the required Node.js runtime.
A running Node.js runtime is not replaced; updates take effect during the next launcher startup. The sidecar starts only the prepared `runtimes/node/bin/node` (`.exe` on Windows) and does not fall back to an environment variable or `PATH`.

Specify a target such as `--target linux-amd64`, or use `--all` to generate all bundled targets.
When there is no muon Vite plugin, the default target for `muon build` is the running host target.

Examples of options that specify the name and icon for muon app binaries generated during the build:

```bash
npx muon build --icon icons/app.png --windows-version 1.2.3
npx muon build --icon icons/app.png --linux-name "My App"
```

- `--icon` specifies the static icon for the muon app. It becomes the common source for Windows PE/NSIS, Linux desktop, and the startup title-bar icon.
- On Windows targets, `--windows-icon`, `--windows-product-name`, `--windows-file-description`, `--windows-company-name`, `--windows-version`, and `--windows-copyright` can override Windows resource metadata for the launcher and NSIS installer.
  `--windows-icon` is an icon override only for Windows targets.
  The same values can also be specified in `windows.resource` in `muon.json`.
- For Windows code signing, specify `--windows-sign-command`, repeatable `--windows-sign-arg`, and `--windows-sign-target`.
  `--windows-sign-arg` must contain `{path}`, which is replaced with the signing target file. `{target}` and `{kind}` can also be used.
  Specify `--no-windows-code-signing` to disable `windows.codeSigning` from `muon.json`.
- On Linux targets, `--linux-desktop-id`, `--linux-name`, `--linux-comment`, `--linux-icon`, `--linux-categories`, and `--linux-startup-notify` can override desktop entry metadata.
  `--linux-icon` is an icon override only for Linux targets.
  The same values can also be specified in `linux.desktop` in `muon.json`.

> Note: When generated assets are collected directly by the muon CLI without the Vite plugin, the CLI itself does not resolve virtual modules for plugin references through `import`.
> Bundle code that uses the validate-mode `muon:node` module with Vite first. To access Node.js integration from assets that do not pass through Vite, use `window.muon.node.createNode()` in `simple` mode.

## Creating and running AppImages

An AppImage is a single Linux executable that runs without an installer. Create a separate file for each CPU architecture:

```bash
npx muon pack --type appimage --target linux-amd64
npx muon pack --type appimage --target linux-armhf
npx muon pack --type appimage --target linux-arm64
```

The output is `artifacts/<packageName>-<version>-<target>.AppImage`. Omitting `--type` also includes AppImage among the Linux formats. This format cannot be selected for Windows or Android.

The packaging host requires Linux, Node.js, `readelf` from binutils, `file`, a standard shell, and coreutils. On Debian and Ubuntu, install missing tools with `apt-get install binutils file`. An amd64 host can also produce ARM AppImages. The first pack downloads fixed versions of appimagetool and the target CPU's runtime, verifying their SHA-256 hashes. Later builds reuse `$XDG_CACHE_HOME/muon/appimage`, or `~/.cache/muon/appimage` when unset. Set `MUON_APPIMAGE_TOOLS_DIR` to use another directory. See [appimagetool](https://github.com/AppImage/appimagetool/tree/1.9.1) and [the runtime](https://github.com/AppImage/type2-runtime/tree/20251108).

Distribute the file matching the user's CPU, grant execute permission, and run it:

```bash
chmod +x ./my-app-1.0.0-linux-amd64.AppImage
./my-app-1.0.0-linux-amd64.AppImage
```

CEF is not bundled. The existing launcher downloads it on first startup, along with the required Node.js runtime for apps using `node.project`. CEF selection policies are shared with deb, including policies other than `tested`. Prepared runtimes and valid download caches are reused, but selecting an uncached version or lacking a required catalog needs network access. This format does not guarantee an offline first launch.

Downloaded archives share a cache under `$XDG_CACHE_HOME/muon/`, or `~/.cache/muon/` when unset. Set `MUON_CACHE_DIR` to use another location.

The AppImage contents are read-only. CEF, Node.js, and startup state are prepared under `$XDG_STATE_HOME/<appId>/runtime/`, or `~/.local/state/<appId>/runtime/` when unset. Profile placement follows the application's `browser.profilePath`. After moving an AppImage, launch it directly once to update its menu entry and existing enabled autostart entry. This does not enable autostart implicitly. See [storage locations and removal](./package-install-location-advanced-topics.md).

Normal startup requires kernel FUSE support and permission to use it. The selected runtime statically includes the userspace FUSE library, so a separate `libfuse2` installation is unnecessary. Use extract-and-run where FUSE is unavailable. See [the type2-runtime configuration](https://github.com/AppImage/type2-runtime/blob/20251108/README.md) and [extraction](https://docs.appimage.org/user-guide/troubleshooting/fuse.html).

```bash
./my-app-1.0.0-linux-amd64.AppImage --appimage-extract-and-run
```

Menu and autostart entries retain the original AppImage path but do not store this argument. To launch through these entries in a session without FUSE, set `APPIMAGE_EXTRACT_AND_RUN=1` in the session environment. CEF setuid sandbox mode is exclusive to deb; `--linux-sandbox=setuid` is rejected when packaging an AppImage. See [sandbox limitations](./limitation.md#limitations-on-enabling-the-cef-sandbox-on-linux).

### Linux runtime environment

Distribution binaries use Ubuntu 20.04 as their build baseline. amd64 has been tested on Ubuntu 20.04 and 24.04; armhf and arm64 have been tested by running their Ubuntu 20.04 containers through QEMU. This does not include physical ARM GPU or audio device testing.

GTK, GLib, and their required modules are bundled. glibc, the C++ runtime, NSS, ALSA, X11/Wayland, GPU drivers, GBM, and other host libraries remain OS dependencies. The runtime environment needs these desktop libraries, CA certificates, and `curl`. Placing an AppImage in a minimal container does not supply all required host libraries.

Minimum requirements are derived from the CEF used to build muon and all completed native components. The current baseline build requires glibc 2.29 or later, `libstdc++6` package version 9 or later, GBM 21.1 or later, and other libraries. Meeting these numbers alone does not guarantee support for an arbitrary distribution. Details are available in the NPM package's `dist/runtime/<target>/linux-dependencies.json.txt` and adjacent `linux-dependencies.json`. `linux-bundle.json` records bundled files, their sources, and license locations.

Selecting a future CEF at runtime still uses requirements derived from the build reference CEF; additional requirements of that future version are not recalculated. Native plugins requiring newer libraries also need to be built for the intended environment. See [CEF's Linux build environment](https://chromiumembedded.github.io/cef/branches_and_building.html).
