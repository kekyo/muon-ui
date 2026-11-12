# Background services

Muon services run work that can continue after a window closes. The CEF backend on Linux and Windows and the Android backend support C/C++ services and JS/TS services hosted by QuickJS. QuickJS services can call explicitly allowed C/C++ plugins. Running Node.js as a service is not supported in this release.

Declare named services in `muon.json`. An application can have several services, and pages acquire them by name before calling methods. Ending a service does not close an open UI.

## Your first TS service

Add the SDK to an existing Muon application. Building QuickJS services requires Vite 8 or later.

```bash
npm install -D muon-plugin-sdk
```

Create `services/echo.ts`. The interface declares methods, argument and result types, and events. Each service start calls `start`, which returns the method implementations and cleanup operation.

```ts
import { defineService, defineServiceInterface } from 'muon-plugin-sdk/services';

export default defineService({
  interface: defineServiceInterface({
    schemaVersion: 1,
    methods: {
      echo: { parameters: ['string'], returns: 'string', errors: [] },
    },
    events: {},
  }),
  start: () => ({
    methods: { echo: (_call, text) => text },
    stop: async () => {},
  }),
});
```

Add the service to `muon.json`. Without `autostart`, it runs while pages hold references.

```json
{
  "services": {
    "echo": { "runtime": "quickjs", "entry": "services/echo.ts" }
  },
  "plugin": {
    "mode": "simple",
    "plugins": [{ "name": "internal", "allow": ["muon.services.*"] }]
  }
}
```

Build the application with its services, then run the generated launcher:

```bash
npx muon build --target linux-amd64
```

Select `windows-amd64` or `windows-i686` for Windows, or `android` to produce an APK to install. Currently, `muon run` and `vite dev` do not assemble service declarations. After changing a service, rebuild the application and run the generated artifact.

Acquire a reference in the page, call the service, and release it when finished.

```ts
const services = window.muon.services;
if (!services) throw new Error('Service access is not enabled');
const handle = await services.acquire('echo');
try {
  const reply = await handle.call('echo', ['Hello']);
  console.log(reply);
} finally {
  await handle.release();
}
```

In `validate` mode, grant the importing source `muon.services.*` through the normal plugin policy and import `acquire` and other operations from `muon:services`. Service declarations and page access permissions are separate settings.

## References and autostart

A service is needed while a page reference or a host resident reference exists. The host holds at most one resident reference per service.

| Operation | Effect |
| --- | --- |
| `acquire(id)` | Acquires a distinct reference and waits for startup if necessary |
| `handle.release()` | Releases only that reference; repeated calls do not release others |
| Page destruction or UI disconnection | Reclaims that page's references and pending acquisitions |
| `setAutostart(id, true)` | Saves ON and retains one resident reference |
| `setAutostart(id, false)` | Saves OFF and releases only the resident reference |
| Releasing the last reference | Begins service cleanup |

`autostart: true` in `muon.json` supplies the initial preference. A saved user preference takes precedence and survives the next launch. Repeatedly setting the same value does not change reference counts. OFF preserves other pages' references and allows later explicit acquisitions.

`setAutostart()` completes after saving the preference and updating residency; it does not await startup or shutdown. Similarly, `release()` waits for reference release. A settings panel can display the preference and actual runtime state separately:

```ts
const watcher = await services.watchState('echo', state => {
  checkbox.checked = state.autostart;
  status.textContent = `${state.state}: ${state.error}`;
});
await services.setAutostart('echo', checkbox.checked);
// When this panel is no longer used:
await watcher.close();
```

`getState(id)` and `watchState(id, listener)` do not acquire references. Observations contain `state`, `autostart`, `resident`, `clientReferences`, `generation`, `revision`, and `error`. Runtime states are `stopped`, `starting`, `running`, `stopping`, and `failed`. Keep generation identities and saved preference revisions as strings. A slow watcher receives coalesced current state, not a history of every transition.

A terminated generation invalidates its handles. Acquire a new handle to use the service again. Failed startup does not trigger endless retries; inspect the failure before acquiring again. Assigning a handle to another JS variable creates no additional reference. Cleanup does not depend solely on garbage collection.

## Methods, callbacks, and events

To share static types with a page, put a `defineServiceInterface()` declaration in a TS file separate from the implementation. `bindServiceClient(interfaceDefinition, handle)` checks the runtime interface and types calls, results, and events. The caller still owns and releases the original handle.

Use `bigint` for `int64` and `uint64`, and `Uint8Array` for `bytes`. Strings, booleans, 32-bit integers, floating-point numbers, and arrays of these types are also supported. Values must match the declared schema. Declare method-specific error codes in `errors`; JS/TS implementations throw errors with `code` and `message`.

`call(method, args, { signal })` supports cancellation. QuickJS methods receive invocation context as their first argument and can observe its `signal`. Implement cancellation for pending I/O and release resources during shutdown.

Function arguments are callbacks scoped to the parent method. They expire when the method completes or is cancelled. Use events for ongoing notifications: services call `context.emit(name, value)` on their start context, and pages call `handle.subscribe(name, listener)`.

```ts
const subscription = await handle.subscribe('progress', async value => {
  console.log(value);
});
// When the listener is no longer needed:
await subscription.close();
```

Events are delivered sequentially within each subscription. Subscriptions add no lifetime references. Release, disconnection, or delivery backlog can terminate a subscription; `subscription.closed` resolves with a failure reason or `undefined`.

## Implementing a C/C++ service

Generate a service project with the public SDK:

```bash
npx muon-plugin init native/echo --kind service --name echo --language c
npx muon-plugin build native/echo/muon-plugin.build.json --target linux-amd64 --out artifacts/echo
```

The generated `service.c` or `service.cpp` uses `muon_service_api.h`. Export a factory and interface from `muon_get_service_factory()` and implement startup, invocation, cancellation, and shutdown. The public ABI comments describe argument and completion ownership. Keep instances and callback resources alive until their asynchronous work completes.

Build the same source for the required targets and register the resulting `muon-plugin.json`. Service artifacts are distinct from page plugin artifacts.

```json
{
  "services": {
    "echo": {
      "runtime": "native",
      "artifact": "artifacts/echo/muon-plugin.json",
      "autostart": true,
      "config": { "label": "Background echo" }
    }
  }
}
```

Implement OS-specific work for each target. The service API itself does not depend on CEF or JNI and accepts common C99/C++20 sources. The SDK includes `examples/services/native-echo.c` as an RPC example. Assembling an application from prebuilt native artifacts does not require a native compiler.

## Calling native code from QuickJS

Declare each plugin and allowed function under the QuickJS service's `plugins`. These are ordinary C/C++ plugins built with the [public SDK](muon-plugin-sdk.md).

```json
{
  "services": {
    "worker": {
      "runtime": "quickjs",
      "entry": "services/worker.ts",
      "plugins": {
        "counter": {
          "native": { "manifest": "artifacts/counter/muon-plugin.json" },
          "allow": ["muon.counter.increment"]
        }
      }
    }
  }
}
```

This service can use `import { increment } from 'muon:counter'` and call `await increment(41)`. The `native` selector accepts the same `source`, `manifest`, or `package` options as ordinary plugins. Plugin authors supply TS module declarations. See the SDK's `examples/services/quickjs-native.ts` and `counter.d.ts`.

Services do not inherit page permissions. Services sharing a plugin must agree on its artifact and configuration, but may grant different functions. Raw pointer types are unsupported. Service RPC strings support embedded NUL; the plugin ABI's C strings do not. Native implementations must release retained callbacks and function proxies during shutdown.

## QuickJS runtime

Each service has a separate JS environment. Alongside standard JavaScript, it provides `setTimeout`, `clearTimeout`, and `console`. Add `import type {} from 'muon-plugin-sdk/service-globals'` for optional host declarations.

The host does not supply DOM, `window.muon`, Node builtins, `fetch`, or `setInterval`. Use explicitly granted native plugins for filesystem and network I/O. The builder produces a single JS bundle and rejects undeclared external modules and separate assets.

Current limits are 8 MiB per JS bundle, 64 MiB per JS heap, a 1 MiB stack, 2 seconds per JS execution turn, and 10 seconds each for startup and shutdown. Waiting for asynchronous I/O differs from CPU execution time. Runtime errors and exceeded limits are reported as service failures. Unresponsive native cleanup or a process crash can affect other services in the same process.

## Desktop windows and login startup

Desktop services run in a separate process from the UI. A resident reference keeps them running after every window closes; reopening the application can reconnect to the same generation. `muon.browser.close()` closes a window; `muon.browser.shutdown()` exits the whole application, including services. Explicit shutdown preserves autostart preferences for the next normal launch.

`muon.environments.getAutostart()` and `setAutostart()` control showing the UI at login. Use `muon.services.setAutostart()` separately for each service. The OS login entry remains while either UI auto-display or any service autostart is enabled. This is a user-session host, not an OS daemon or Windows Service that outlives logout.

## Android requirements

Android services run in a process separate from the UI, as foreground services with visible notifications. Every service built for Android must declare its purpose and notification. For example, an implementation communicating with a connected device can use:

```json
{
  "android": {
    "foregroundServiceType": "connectedDevice",
    "notification": {
      "channelId": "device-work",
      "title": "Device connection",
      "text": "Communicating with your device"
    }
  }
}
```

Add this `android` property under `services.<id>`. Select the type matching the actual work and meet its permission and runtime requirements. `specialUse` also requires `specialUseSubtype`. See [Android service types](https://developer.android.com/develop/background-work/services/fgs/service-types).

Autostart ON does not guarantee continuous OS execution. Background starts, while-in-use permissions, and execution duration have OS restrictions. Rejected startup preserves the saved preference and reports the actual state and reason. See [background start restrictions](https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start) and [foreground service timeouts](https://developer.android.com/develop/background-work/services/fgs/timeout).

Closing the UI differs from an OS stop of the entire application. Android's Active apps stop action terminates the whole app without guaranteed cleanup callbacks. Reference counts do not override that action. See [user-initiated stopping](https://developer.android.com/develop/background-work/services/fgs/handle-user-stopping).

On Android API 30 and later, service host startup reads OS exit history and exposes it as `lastHostExit` in `getState()` and `watchState()` observations. It includes `reason`, the OS `reasonCode`, and `timestampMs`, a decimal string of milliseconds since the Unix epoch. The value is `null` when no record can be obtained, on API 29 and earlier, or on PC. See [ApplicationExitInfo](https://developer.android.com/reference/android/app/ApplicationExitInfo).

This is historical information about the entire service host. Services sharing that host return the same record; it does not establish that each service was running at the time. Use `state` and `error` for the current runtime state and failure.

```ts
const state = await window.muon.services!.getState('echo');
if (state.lastHostExit) {
  console.log(state.lastHostExit.reason, state.lastHostExit.timestampMs);
}
```

This release does not register service startup on device boot. Saved preferences are evaluated on a normal application launch. See the [Android guide](android.md) for application builds, signing, and distribution formats.
