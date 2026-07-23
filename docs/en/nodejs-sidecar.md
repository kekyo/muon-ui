# Using the Node.js sidecar

muon's Node.js feature is optional and runs Node.js in a process separate from the muon core. Only applications with `node.project` in `muon.json` load the Node plugin.

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

A relative `project` is resolved from the directory containing `muon.json`; an absolute path is also accepted. It points to an ordinary Node.js project containing its `package.json`, source files, and installed runtime dependencies. The MVP exposes its descriptor facade through `window.muon.node`, so operational use requires `plugin.mode: "simple"`.

The project's `package.json` is copied without conversion. The Node.js module loader interprets `type`, `main`, `exports`, and `imports`; the developer's package manager owns `dependencies`; and the muon Node bridge validates `engines.node`. Keep muon-specific settings under `node` in `muon.json`.

muon does not run a package manager. Prepare dependencies in the Node project before `muon build` or `muon pack`. The complete project is staged as `node-project/` outside `assets.zip`; symbolic links are dereferenced. The staging boundary is the configured project directory, so dependencies hoisted into an ancestor workspace are not copied unless the project contains a link to them. Ensure that every runtime dependency is represented inside `node.project`. Symbolic-link cycles are unsupported and fail the build. Configurations where the Node project overlaps the asset source, the Vite `outDir` or `publicDir`, an output directory, or a pack work directory are rejected before Vite copies or deletes files, preventing source deletion and duplicate packaging.

The same Node project is copied to every target in a multi-target build. If it uses native add-ons or platform-specific optional dependencies, prepare dependencies compatible with each target or build targets separately. The MVP does not rebuild dependencies for a target.

## Calling modules

`importModule()` returns a frozen renderer-local facade built from export descriptors, not the Node module object itself.

```ts
const fs = await window.muon.node?.importModule("node:fs/promises");
if (fs !== undefined) {
  const text = await fs.readFile("foobar.txt", "utf8");
  await fs.$release();
}
```

Built-ins require the explicit `node:` prefix. Project-relative specifiers and bare packages resolvable from the configured project are also accepted. `importModule(".")` loads the project's `package.json` `main` entry, or Node.js's directory-entry default when `main` is omitted.

Only `undefined`, `null`, booleans, finite numbers other than negative zero, strings, signed or unsigned 64-bit `bigint` values, copied `ArrayBuffer` and `ArrayBufferView` values, and temporary callback functions can cross the process boundary. `undefined` represents both an explicit value and a `void` result. Only the selected view range is copied, and binary results are normalized to `Uint8Array` in the renderer. Arbitrary objects, class instances, Node handles, and pointers are rejected. A callback remains valid until the exported Node function call settles. `$release` is reserved for facade lifecycle control, and `then` is reserved to prevent promise thenable assimilation, so a module exporting either name cannot be imported.

`$release()` releases only the descriptor, proxy, and remote-callback references maintained by the bridge. It does not stop or dispose of resources created inside the Node module, such as servers, timers, or watchers.

Each encoded IPC frame is limited to 16 MiB. JSON framing and base64 expansion count toward the limit, so the usable binary payload is smaller and depends on the other arguments. One sidecar accepts at most 1,024 pending requests; additional calls are rejected until earlier requests settle.

## Node.js executable

The MVP does not bundle Node.js. At runtime muon selects:

1. the absolute path in `MUON_NODE_EXECUTABLE`, when set; or
2. `node` from `PATH` (`node.exe` on Windows).

A relative override is rejected without a `PATH` fallback, and no shell is used. The sidecar itself requires Node.js `^20.19.0 || >=22.12.0` and validates that range before project initialization. When `package.json` contains `engines.node`, the sidecar also validates the project's range.

The sidecar starts lazily on the first import, and concurrent imports coalesce onto one process per app. Before project code is loaded, its working directory is changed to the configured project root. A runtime failure is sticky until app shutdown; the MVP does not restart the process.

## Validation and permissions

Validate mode checks configuration, plugin metadata, exposed functions, and allow-policy consistency. It neither exposes `window.muon.node`, starts Node.js, nor executes project code. Use simple mode to call the MVP API. Missing system Node.js, engine mismatches, module resolution, and runtime code errors are checked only after switching to simple mode and making the first import.

The muon allow policy controls the renderer-to-plugin boundary. It does not restrict filesystem, network, or child-process operations performed inside Node.js. The application developer is responsible for trusting the Node project and its dependencies.

## Packaging and shutdown

For an opted-in app, `muon build` and `muon pack` include the platform `node.so` or `node.dll`, `node-bridge.mjs`, and staged `node-project/`. The framework does not add a Node executable, distribution archive, or download cache; a file deliberately placed inside the developer's Node project is still copied as ordinary project content.

During app shutdown, muon requests sidecar shutdown and waits asynchronously before unloading the plugin. After closing the bridge and transport, the sidecar exits explicitly even when Node.js still has active handles. muon does not discover, call, or await project-specific cleanup APIs; application code must call and await any such API when graceful resource cleanup is required. An unresponsive sidecar is terminated after a grace period and then forcibly killed if necessary. On Linux, asynchronous child-exit observation uses `pidfd_open` and therefore requires Linux kernel 5.3 or later. POSIX termination targets the sidecar process itself; project code is responsible for stopping any child processes that it creates.
