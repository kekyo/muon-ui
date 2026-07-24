# CEF download and update

CEF binary asset files are known for being very large.
When a vulnerability is found in CEF, the CEF binaries need to be updated.
If a muon app directly included those binaries, updating CEF would require updating the entire muon app distribution.

To reduce muon app distribution size and simplify CEF binary updates, muon downloads the required CEF binaries when the muon app starts and prepares the runtime environment.
For an app that configures a Node.js project and whose effective plugin mode is `simple`, the same preparation process also downloads the required Node.js runtime.
`muon build` and `muon pack` include the Node.js project, the muon Node bridge, and the runtime requirement in their output, but do not include the Node.js executable itself.

When you run `npm run dev`, build `muon-core`, or distribute generated build output and an end user starts the muon app, required CEF binaries are automatically downloaded from the official CEF distribution site if they are not present locally.
This takes some time, but downloaded CEF tarballs are cached locally, so later launches use the cache.
When a Node.js runtime is required, it is also obtained from the official Node.js distribution site during development or a distributed launcher startup, and the same muon cache is reused.
If both CEF and Node.js are required, they are prepared in parallel.
Notifications to the progress display are serialized, so concurrent downloads can safely update a single progress display.

- The local cache is placed under `~/.cache/muon/` on Linux and `$HOME\.cache\muon\` on Windows.
- The `MUON_CACHE_DIR` environment variable can override the cache directory.

The cache directory contains catalogs, verification data, and downloaded archives:

```text
~/.cache/muon/
+-- cef-catalog.json
+-- node-catalog.json
+-- node-checksums
|   +-- <version>
|       +-- SHASUMS256.txt
+-- artifacts
    +-- cef
    |   +-- cef_binary_<version>_<target>_minimal.tar.bz2
    +-- node
        +-- <version>
            +-- node-<version>-<target>.<archive-extension>
```

Node.js release information comes from the official `https://nodejs.org/dist/index.json`.
The selected archive is verified with the SHA-256 value in that release's `SHASUMS256.txt`.
The Node.js version is selected from the intersection of the project's `package.json` `engines.node` range and the muon Node bridge compatibility range.
When `engines.node` is omitted, muon selects the latest compatible LTS release. If none matches, preparation fails without falling back to a non-LTS release.
When it is specified, muon prefers the greatest matching LTS version and otherwise selects the greatest matching version from all releases.

For `muon build` output and normal installer runtimes, the distributed `dist-muon/<target>` directory is treated as read-only source data.
When an end user starts the application, `muon-launcher` stages the whole dist directory into `runtime/` under the user state directory before execution, extracts the CEF binaries and, when required, the Node.js runtime there, and then launches `muon-core`.
The Node.js runtime is kept separate from the application root under `runtimes/node/`.
It contains only `LICENSE` and `bin/node` (`bin/node.exe` on Windows); npm, Corepack, and other files from the official archive are not installed into the runtime.
The CEF profile is placed in `profile/` under the same application state root:

- Linux: `$XDG_STATE_HOME/<appId>/runtime/` and `$XDG_STATE_HOME/<appId>/profile/`, or `$HOME/.local/state/<appId>/runtime/` and `$HOME/.local/state/<appId>/profile/`
- Windows: `%LOCALAPPDATA%\<appId>.<arch>\runtime\` and `%LOCALAPPDATA%\<appId>.<arch>\profile\`. `<arch>` is `amd64` or `i686`.

During launch preparation, the CEF version and catalog refresh are decided according to `muon-launcher.ini` in the user state directory.
The CEF version policy in this file applies only to CEF; the Node.js version is determined from the embedded runtime requirement.
See the separate chapter for details.

Portable distributions from `muon pack --type zip` and `muon pack --type tgz` / `tar.gz` are exceptions.
When the app launcher directly under the extracted `<packageName>/<target>` directory starts, `muon-launcher` prepares CEF and any required Node.js runtime in place under the same directory and launches `muon-core` with that directory as the current working directory.
Because portable distributions fix `browser.profilePath` to `profile` at build time, the CEF profile is created as `profile/` directly under the extracted directory.
The user state directory is not used for runtime or profile data.
However, downloaded CEF and Node.js archive caches are still stored under `MUON_CACHE_DIR` or the default `~/.cache/muon/`-style directory.

Normal Linux deb packages also use a runtime in the user state directory, just like the normal installer flow described above.
Only a deb package generated with `muon pack --type deb --linux-sandbox=setuid` is an exception: a root-owned privileged helper prepares CEF and any required Node.js runtime in a system runtime.
Installing this system runtime requires administrator privileges, and in exchange fully enables `cef-sandbox`, which CEF requires on Linux.
