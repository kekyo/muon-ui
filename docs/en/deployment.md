# Application distribution

## Distribution builds

As your muon app implementation starts to take shape, you will probably want to know how to distribute it to users.
muon makes it easy to generate the files for distribution, but first you need to add the application name, credits, and other metadata.

Following NPM package conventions, write these values in `package.json` and muon will reflect them.
For example:

```json
{
  "name": "my-muon-app",
  "version": "0.1.0",
  "description": "First time muon app.",
  "files": [
    "README.md",
    "LICENSE",
    "dist"
  ],
  // :
  // :
}
```

This reflects the muon app name, version, and description.

When regular files are specified in `files`, files that satisfy the conditions are copied into the root of the distribution directory.
In the example above, `README.md` and `LICENSE` are included.
Paths resolved as asset sources, such as `dist`, are excluded from this additional copy.

After preparation, run `npm run build`.
This runs the normal Vite build and then generates the muon distribution directories.

```bash
npm run build
```

> Note: This is an alias for `vite build` defined in the `scripts` section of `package.json`.

The files output to Vite's `build.outDir` are collected into `assets.zip`.
By default, muon builds all supported targets and outputs them under the `dist-muon/` directory:

```text
dist-muon/
+-- linux-amd64/
|   +-- assets.zip
|   +-- :
|   +-- vite-project
+-- linux-arm64/
|   +-- assets.zip
|   +-- :
|   +-- vite-project
+-- linux-armhf/
|   +-- assets.zip
|   +-- :
|   +-- vite-project
+-- windows-amd64/
|   +-- assets.zip
|   +-- :
|   +-- vite-project.exe
+-- windows-i686/
    +-- assets.zip
    +-- :
    +-- vite-project.exe
```

If you want to specify build targets or the output location in detail, use the Vite plugin's `build` argument:

```ts
import { defineConfig } from 'vite';
import muon from 'muon-ui/vite';

export default defineConfig({
  plugins: [
    muon({
      build: {  // Build options
        targets: ['linux-amd64', 'windows-amd64'],
        iconPath: 'icons/app.png',
        distributionFiles: ['README.md', 'LICENSE'],
        linuxDesktop: {
          name: 'My App (linux)',
        },
        windowsResource: {
          productName: 'My App (windows)',
        }
      },
    }),
  ],
});
```

The target names that can be specified in `build.targets` are:

- Linux targets: `linux-amd64`, `linux-armhf`, `linux-arm64`
- Windows targets: `windows-i686`, `windows-amd64`

By default, the application executable name is generated from the `name` field in `package.json`.
For scoped package names, the scope is removed.
If you place a PNG image at `build.iconPath`, it is used as the muon app icon.
If `build.distributionFiles` is specified, it takes precedence over `package.json` `files`.
An empty array disables additional copying.
You can also add Linux-target-specific and Windows-target-specific settings as shown above.

## Package generation

muon can also generate distribution packages such as installers and archives.

If the distribution build described above is ready, just run the `muon pack` command:

```bash
npx muon pack
```

> Note: If you find this hard to remember, you can add a `pack` entry to `scripts` in `package.json`. I do :)

`muon pack` generates distribution directories using the same build sequence as `muon build`, then packages them into the requested formats.

- If a muon Vite plugin is present, it runs `vite build` and generates the muon app.
- If no muon Vite plugin is present, it does not run `vite build` and uses existing assets.

It then outputs the final distribution artifacts for each requested format to `artifacts/`.
Working files generated during package creation, such as the `deb` package tree and `nsis` `.nsi` scripts, are placed under `.muon/pack/`.

If no options are passed to `muon pack`, files for all targets are generated.
Examples:

```bash
npx muon pack --target windows
npx muon pack --target amd64
npx muon pack --type tgz
npx muon pack --type nsis
npx muon pack --target linux-amd64 --type tgz,deb 
npx muon pack --target windows-amd64 --type nsis
```

- Targets can be specified with `--target` or `--all`.
  When omitted, the Vite plugin `build` setting is used.
  If there is no muon Vite plugin or the setting is omitted, all supported targets become package candidates.
  In addition to complete target names, platform names `linux` and `windows`, and architecture names `amd64`, `arm64`, `armhf`, and `i686`, can also be specified.
- `--type` can specify `zip`, `tgz`, `tar.gz`, `deb`, and `nsis`, either comma-separated or as multiple options.
  When omitted, all of `zip`, `tgz`, `deb`, and `nsis` are targeted.
- `zip` is only available for Windows targets and creates a portable ZIP containing each `<packageName>/<target>` directory.
- `tgz` or `tar.gz` is only available for Linux targets and creates a portable gzip-compressed tar archive containing each `<packageName>/<target>` directory.
  `tgz` is an alias for `tar.gz`, and the output filename is always `*.tar.gz`.
  Portable distributions do not include CEF binaries. On first launch, CEF is prepared directly under the extracted `<packageName>/<target>` directory.
  The profile is also stored in `profile/` under the same directory.
- `deb` is only available for Linux targets and requires `dpkg-deb` on the runtime `PATH`.
  On Debian/Ubuntu, install it with `sudo apt install dpkg-deb`.
- `nsis` is only available for Windows targets and requires `makensis` on the runtime `PATH`.
  On Debian/Ubuntu, install it with `sudo apt install nsis`.
  On Windows, download it from [Nullsoft Scriptable Install System](https://nsis.sourceforge.io/Main_Page).
- Combinations unsupported by the requested format and target are skipped, and only valid combinations are generated.
  For example, `muon pack --type nsis` generates only NSIS packages for Windows targets and does not generate Linux targets.
- CLI options can be specified in the same way as `muon build` options such as `--icon`, `--windows-icon`, and `--linux-desktop-id`.
  `packageName`, `version`, `description`, and `author` use `package.json` as their defaults and can be overridden by CLI options.
- In `muon pack`, the value of `--package-version` is also used as the `package.json.version` fallback for the Windows resource version.
  For example, when applying a Git-derived version with [screw-up](https://github.com/kekyo/screw-up/), specify `npx muon pack --package-version "$(screw-up format -e '{version}')"`.
