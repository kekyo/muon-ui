# muon CLI (Advanced topics)

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
