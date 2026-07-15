# CEF download and update

CEF binary asset files are known for being very large.
When a vulnerability is found in CEF, the CEF binaries need to be updated.
If a muon app directly included those binaries, updating CEF would require updating the entire muon app distribution.

To reduce muon app distribution size and simplify CEF binary updates, muon downloads the required CEF binaries when the muon app starts and prepares the runtime environment.

When you run `npm run dev`, build `muon-core`, or distribute generated build output and an end user starts the muon app, required CEF binaries are automatically downloaded from the official CEF distribution site if they are not present locally.
This takes some time, but downloaded CEF tarballs are cached locally, so later launches use the cache.

- The local cache is placed under `~/.cache/muon/` on Linux and `$HOME\.cache\muon\` on Windows.
- The `MUON_CACHE_DIR` environment variable can override the cache directory.

Only the catalog and downloaded CEF tarballs are placed in the cache directory:

```text
~/.cache/muon/
+-- catalog.json
+-- artifacts
    +-- cef_binary_<version>_<target>_minimal.tar.bz2
```

For `muon build` output and normal installer runtimes, the distributed `dist-muon/<target>` directory is treated as read-only source data.
When an end user starts the application, `muon-launcher` stages the whole dist directory into `runtime/` under the user state directory before execution, extracts the CEF binaries there, and then launches `muon-core`.
The CEF profile is placed in `profile/` under the same application state root:

- Linux: `$XDG_STATE_HOME/<appId>/runtime/` and `$XDG_STATE_HOME/<appId>/profile/`, or `$HOME/.local/state/<appId>/runtime/` and `$HOME/.local/state/<appId>/profile/`
- Windows: `%LOCALAPPDATA%\<appId>.<arch>\runtime\` and `%LOCALAPPDATA%\<appId>.<arch>\profile\`. `<arch>` is `amd64` or `i686`.

During launch preparation, the CEF version and catalog refresh are decided according to `muon-launcher.ini` in the user state directory.
See the separate chapter for details.

Portable distributions from `muon pack --type zip` and `muon pack --type tgz` / `tar.gz` are exceptions.
When the app launcher directly under the extracted `<packageName>/<target>` directory starts, `muon-launcher` prepares CEF in place under the same directory and launches `muon-core` with that directory as the current working directory.
Because portable distributions fix `browser.profilePath` to `profile` at build time, the CEF profile is created as `profile/` directly under the extracted directory.
The user state directory is not used for runtime or profile data.
However, downloaded CEF tarball caches are still stored under `MUON_CACHE_DIR` or the default `~/.cache/muon/`-style directory.

> Note: The explanation above excludes launches from Linux deb packages.
> With Linux deb packages, the muon app is installed into a directory managed with administrator privileges, and CEF download and placement also use directories that require administrator privileges.
> In exchange, `cef-sandbox`, which is required by CEF on Linux, is fully enabled.
