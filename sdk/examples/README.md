# Independent plugin and application examples

Copy this directory outside the muon-ui repository. Run `npm install` in `application`, then `npx muon-plugin doctor --target linux-amd64` to check prerequisites. Before publication, install the SDK and muon-ui tarballs with `npm install -D /path/to/muon-plugin-sdk.tgz /path/to/muon-ui.tgz`.

| Directory       | Workflow                                                                                          |
| --------------- | ------------------------------------------------------------------------------------------------- |
| `local`         | Application-local C++20/cardio plugin built with Make and pkg-config.                             |
| `vendor-plugin` | C99 plugin importing a separate library through CMake.                                            |
| `vendor-source` | A small educational C library standing in for a vendor checkout, independent of muon and its SDK. |
| `application`   | Vite application referencing both source plugins; its button calls both.                          |
| `npm-package`   | package.json for distributing the `local` plugin's artifacts through NPM.                         |

Run `npm run dev` in `application` to launch the development application. Build Linux with `npm run build`, Windows with `npx muon build --target windows-amd64`, or Android with `npx muon build --target android`. Install the corresponding application toolchain too.

For a real vendor or submodule checkout, adjust `VENDOR_SOURCE_DIR` in `vendor-plugin/muon-plugin.build.json` and the CMake target being linked. Keep the external source unchanged and place adapter code in `vendor-plugin`. Generated outputs go into the directory selected by the SDK.

To try NPM distribution, run these commands from `application`. Repeat the build for every target you intend to distribute before packaging.

```bash
npx muon-plugin build ../local/muon-plugin.build.json --target linux-amd64 --out ../npm-package
npx muon-plugin verify ../npm-package/muon-plugin.json --target linux-amd64
cd ../npm-package
npm pack
```

Install that tarball in the application and change counter's reference to `"native": { "package": "muon-example-counter" }` to consume a prebuilt NPM package. Package `vendor_counter` the same way. Once every plugin comes from prebuilt packages, the application can remove the authoring SDK dependency.

See the [guide](https://github.com/kekyo/muon-ui/blob/main/docs/en/muon-plugin-sdk.md) for dependency libraries, public types, permissions, and custom build configuration.
