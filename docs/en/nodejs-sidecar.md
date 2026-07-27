# Using the Node.js sidecar

muon's Node.js feature is optional and runs Node.js in a process separate from the muon core. Only applications with `node.project` in `muon.json` load the Node plugin.

```json
{
  "node": {
    "project": "./backend"
  }
}
```

A relative `project` is resolved from the directory containing `muon.json`; an absolute path is also accepted. It points to an ordinary Node.js project containing its `package.json`, source files, and installed runtime dependencies. Node.js integration is enabled solely by `node.project`; it does not depend on `plugin.mode` or allow settings in `plugin.plugins[]`.

The project's `package.json` is copied without conversion. The Node.js module loader interprets `type`, `main`, `exports`, and `imports`, while the developer's package manager owns `dependencies`. Specify `engines.node` in the same place as in an ordinary Node.js project. muon validates that this range overlaps the Node bridge compatibility range and normalizes their intersection as the runtime requirement. When `engines.node` is omitted, only the Node bridge compatibility range constrains the runtime. Keep muon-specific settings under `node` in `muon.json`.

muon does not run a package manager. Prepare dependencies in the Node project before `muon build` or `muon pack`. The complete project is staged as `node-project/` outside `assets.zip`; symbolic links are dereferenced. The staging boundary is the configured project directory, so dependencies hoisted into an ancestor workspace are not copied unless the project contains a link to them. Ensure that every runtime dependency is represented inside `node.project`. Symbolic-link cycles are unsupported and fail the build. Configurations where the Node project overlaps the asset source, the Vite `outDir` or `publicDir`, an output directory, or a pack work directory are rejected before Vite copies or deletes files, preventing source deletion and duplicate packaging.

The same Node project is copied to every target in a multi-target build. If it uses native add-ons or platform-specific optional dependencies, prepare dependencies compatible with each target or build targets separately. The MVP does not rebuild dependencies for a target.

## Calling modules

In the default `validate` mode, import `createNode()` from the `muon:node` virtual module provided by Vite. Using `muon:node` does not require an entry in `plugin.plugins[].imports` or an allow setting.

```ts
import { createNode } from "muon:node";

const node = await createNode();
try {
  const fs = await node.importModule("node:fs/promises");
  try {
    const text = await fs.readFile("foobar.txt", "utf8");
  } finally {
    await fs.$release();
  }
} finally {
  await node.release();
}
```

In `simple` mode, access the same functionality through `window.muon.node`.

```ts
const node = await window.muon.node.createNode();
try {
  const backend = await node.importModule("./backend.mjs");
  await backend.run();
} finally {
  await node.release();
}
```

`createNode()` starts one Node.js process and waits for bridge initialization to complete before returning a frozen instance facade. Each call creates an independent sidecar with its own module cache, global state, active handles, and process ID. Releasing one instance does not affect another. muon neither pools nor automatically restarts instances.

An instance's `importModule()` returns a frozen renderer-local facade built from export descriptors, not the Node module object itself. muon does not generate module-specific types, so the default export type is `Record<string, any>`. Existing or developer-authored TypeScript types can be applied with the `importModule<TExports>()` type parameter or another normal TypeScript assertion.

Built-ins require the explicit `node:` prefix. Project-relative specifiers and bare packages resolvable from the configured project are also accepted. `importModule(".")` loads the project's `package.json` `main` entry, or Node.js's directory-entry default when `main` is omitted.

Only `undefined`, `null`, booleans, finite numbers other than negative zero, strings, signed or unsigned 64-bit `bigint` values, copied `ArrayBuffer` and `ArrayBufferView` values, and temporary callback functions can cross the process boundary. `undefined` represents both an explicit value and a `void` result. Only the selected view range is copied, and binary results are normalized to `Uint8Array` in the renderer. Arbitrary objects, class instances, Node handles, and pointers are rejected. A callback remains valid until the exported Node function call settles. `$release` is reserved for facade lifecycle control, and `then` is reserved to prevent promise thenable assimilation, so a module exporting either name cannot be imported.

A module facade's `$release()` releases only the descriptor, proxy, and remote-callback references maintained by the bridge. It does not stop or dispose of resources created inside the Node module, such as servers, timers, or watchers, and it does not stop the sidecar process.

An instance's `release()` is idempotent; concurrent calls wait for the same shutdown operation. Once shutdown begins, every module facade obtained from that instance becomes invalid, and pending requests are rejected with `Node instance was released before the request completed`. New operations after shutdown begins are rejected with `Node instance is being released or has been released`. `release()` resolves after the transport is closed and the sidecar process and OS handles have been reaped. Calling a module's `$release()` after its parent instance has begun shutdown joins the completed or in-progress instance shutdown.

When the environment provides `Symbol.asyncDispose`, an instance can also be released as an `AsyncReleaseable`.

```ts
await using node = await createNode();
const backend = await node.importModule(".");
await backend.run();
```

Each encoded IPC frame is limited to 16 MiB. JSON framing and base64 expansion count toward the limit, so the usable binary payload is smaller and depends on the other arguments. One sidecar accepts at most 1,024 pending requests; additional calls are rejected until earlier requests settle.

## Managed Node.js runtime

muon does not search for a system-installed Node.js executable. Instead, it uses a Node.js runtime managed for each application. When `node.project` is configured, runtime preparation occurs before `vite dev` and `muon run` startup in both `simple` and `validate` modes. In a distribution, `muon-launcher` prepares the runtime on the first launch and whenever else preparation is required. A prepared runtime is reused while the ready fingerprint containing the selected Node.js archive's SHA-256 matches; if a catalog refresh changes the selected version, the runtime is prepared again. `muon build` and `muon pack` do not download or install Node.js; they embed the normalized runtime requirement needed for selection into the artifact.

The runtime version is selected from the intersection of `engines.node` in the Node project's `package.json` and the Node bridge compatibility range. When `engines.node` is omitted, muon selects the newest compatible LTS release. If none matches, preparation fails without falling back to a non-LTS release. When `engines.node` is specified, muon likewise prefers the greatest matching LTS release and falls back to the greatest matching release of any kind only when no LTS release matches.

Release metadata comes from the official Node.js distribution index at `https://nodejs.org/dist/index.json`. muon obtains the artifact-specific digest from that version's `SHASUMS256.txt` and verifies the downloaded archive with SHA-256 before extracting it. Only these two files are installed; npm, Corepack, and other files in the official archive are omitted:

- `runtimes/node/LICENSE`
- `runtimes/node/bin/node` (`runtimes/node/bin/node.exe` on Windows)

The Node plugin receives an absolute path to this executable constructed from the directory containing the running `muon-core` executable. It directly starts only that path, with no environment-variable override or `PATH` fallback. If the managed runtime is missing, sidecar startup fails instead of silently using another Node.js installation.

A sidecar starts for each `createNode()` call. Before project code is loaded, its working directory is changed to the configured project root. If a Node process exits unexpectedly, only that instance fails; other instances remain usable. The failed instance is not restarted automatically, but another `createNode()` call can start a new instance.

## Validation and permissions

Validate mode does not expose `window.muon`; it creates Node instances through the `muon:node` virtual module generated by Vite. `muon:node` is available when `node.project` is configured and is handled separately from ordinary muon plugin capabilities. Consequently, the Node interoperation API is not subject to function-level filters in `plugin.plugins[].imports` or allow settings. Simple mode uses `window.muon.node.createNode()`. The application-wide page URL boundary in `plugin.pages` still applies in either mode.

In either mode, muon does not restrict or validate filesystem, network, child-process, or other operations performed by sidecar code, or their results and side effects. The application developer is responsible for the trusted Node project, its behavior, permissions, and dependencies.

The Node project's `package.json` and `engines.node` syntax, and the intersection with the Node bridge compatibility range, are validated during configuration resolution or the build. Module-resolution failures and Node code runtime errors are reported when the corresponding operation is performed on an instance.

## Packaging and shutdown

For an opted-in app, `muon build` and `muon pack` include the platform `node.so` or `node.dll`, `node-bridge.mjs`, staged `node-project/`, and the normalized runtime requirement stored in the internal `launcher.nodeRuntime` field. `launcher.nodeRuntime` is generated by muon; specifying it directly in user `muon.json` is a build error. The build/pack artifact does not contain a Node executable, distribution archive, or download cache; a file deliberately placed inside the developer's Node project is still copied as ordinary project content. The launcher later prepares the required executable from the official distribution source.

When a renderer V8 context is released, muon automatically releases every Node instance created by that context. During app shutdown, muon requests shutdown of every remaining instance and waits asynchronously before unloading the plugin. After closing each bridge and transport, the sidecar exits explicitly even when Node.js still has active handles. muon does not discover, call, or await project-specific cleanup APIs; application code must call and await any such API when graceful resource cleanup is required. An unresponsive sidecar is terminated after a grace period and then forcibly killed if necessary.

On Windows, each sidecar is assigned to its own Job Object with `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`. In addition to normal instance release, this causes all corresponding sidecars to terminate when an unexpected muon process exit closes every Job Object handle. On Linux, asynchronous child-exit observation uses `pidfd_open` and therefore requires Linux kernel 5.3 or later. POSIX termination targets the sidecar process itself; project code is responsible for stopping any child processes that it creates.
