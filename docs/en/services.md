# Background services

Muon services run work that can continue after a window closes. The CEF backend on Linux and Windows supports native, QuickJS, and Node.js implementations. Android supports native and QuickJS implementations.

| `runtime` | Implementation and available features | Backends |
| --- | --- | --- |
| `native` | C/C++ with the public service SDK | CEF, Android |
| `quickjs` | JS/TS and explicitly allowed native plugins | CEF, Android |
| `node` | JS/TS, Node.js builtins, and npm dependencies | CEF |

Declare named services in `muon.json`. An application can have several services, and pages acquire them by name before calling methods. Ending a service does not close an open UI.

A generation lasts from service startup through shutdown. Enabling autostart keeps a resident reference after the last page releases its own reference.

To save text from a page and read it back after a reload, follow [Build a QuickJS note service](quickjs-service-tutorial.md).

## Your first TS service

Add the SDK to an existing Muon application. Building QuickJS and Node.js services requires Vite 8 or later.

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

Select `windows-amd64` or `windows-i686` for Windows, or `android` to produce an APK to install. During CEF development, `npx muon run` and `npx vite` run the same service declarations. Vite also applies service changes. See [Development startup and updates](#development-startup-and-updates).

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

## Developing a QuickJS service as an npm project

Set `project` and `entry` in the application's `muon.json` to manage service dependencies independently. The builder resolves `project` relative to `muon.json` and `entry` relative to the project. An entry without a project is relative to `muon.json`.

```json
{
  "services": {
    "worker": {
      "runtime": "quickjs",
      "project": "./services/worker",
      "entry": "src/service.ts"
    }
  }
}
```

The SDK example includes its dependencies, lockfile, TypeScript configuration and Vitest tests. From the application directory, copy the example and run its checks. From the application root, the same definition works with `npx muon run`, `npx vite dev` and `npx muon build --target linux-amd64`.

```sh
mkdir -p services
cp -R node_modules/muon-plugin-sdk/examples/services/quickjs-project services/worker
cd services/worker
npm install
npm run typecheck
npm test
```

Use `"strict": true`, `"lib": ["ES2022"]` and `"types": ["muon-plugin-sdk/service-globals"]` in `compilerOptions`. Import service types from `muon-plugin-sdk/services`. The builder resolves dependencies from the service project and bundles them into its JS. Choose dependencies that run in QuickJS. The build identifies imports that require Node builtins or native addons.

## Observing startup and shutdown

Each new generation calls `start(context)`. Its `context.startReason` tells the service what started the generation. A page reconnecting to a running service can call the same instance.

| Start reason | Trigger |
| --- | --- |
| `acquire` | A page acquired a reference. |
| `app-start` | The service started because autostart was enabled on app launch or, on Android, after a device reboot. |
| `login-autostart` | The desktop's login registration started the application. |
| `autostart-enabled` | The user enabled service autostart while the application was running. |
| `development-update` | The development host applied a service update. |

When stopping begins, the host cancels the generation's `context.signal` and outstanding calls' signals. A successfully started generation receives `stop(info)` once. The first accepted stop reason is available as `info.reason`. Pass `info.signal` when saving state in this handler. If the host knows the deadline, `info.remainingMs` gives the time remaining in milliseconds.

| Stop reason | Trigger |
| --- | --- |
| `reference-released` | The last reference was released. |
| `app-exit` | The application requested an explicit exit. |
| `session-ending` | The desktop session is ending, or the Android host received a device shutdown notification. |
| `development-update`, `development-exit` | A service update or development command exit. |
| `execution-timeout` | The OS notified the service of its execution deadline. |
| `host-stop` | The host received a general termination request. |

A desktop service can record when it starts at login and when it receives a session-end notification. Give the service `read-write` access to `io.fs.data`. Enable service autostart to try the login case.

```ts
import { defineService, defineServiceInterface } from 'muon-plugin-sdk/services';

export default defineService({
  interface: defineServiceInterface({ schemaVersion: 1, methods: {}, events: {} }),
  start: async (context) => {
    if (context.startReason === 'login-autostart') {
      await context.fs.writeText('login-start.txt', new Date().toISOString(), {
        signal: context.signal,
      });
    }
    return {
      methods: {},
      stop: async (info) => {
        if (info.reason === 'session-ending') {
          await context.fs.writeText('session-end.txt', new Date().toISOString(), {
            signal: info.signal,
          });
        }
      },
    };
  },
});
```

On Windows and Linux the service keeps running when a session-end query is cancelled. Linux session notifications require a desktop portal that supports session monitoring. Android delivers notifications to a running service host. A process crash or power loss can prevent the host from calling `stop`. [Windows session notification](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-endsession) · [Linux session monitoring](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Inhibit.html) · [Android shutdown broadcast](https://developer.android.com/reference/android/content/Intent#ACTION_SHUTDOWN)

## Service files, outgoing requests and local listeners

Give each service its own `io` grants in `muon.json`. Page permissions apply separately. This example grants private storage, a packaged initial value, one outgoing HTTP origin and a loopback listener. An origin identifies a scheme, host and port.

```json
{
  "services": {
    "worker": {
      "runtime": "quickjs",
      "project": "./services/worker",
      "entry": "src/service.ts",
      "io": {
        "fs": { "data": "read-write", "assets": { "initial": "assets/initial.bin" } },
        "http": { "origins": ["https://api.example.org"] },
        "server": { "listen": true }
      }
    }
  }
}
```

- Use `context.fs.readBytes` and `writeBytes` for `Uint8Array` data, or `readText` and `writeText` for UTF-8. Paths are relative to an area. The default area, `data`, is private to the service. A write replaces the destination atomically. Failure before replacement preserves its previous contents. The API also provides `mkdir`, `list` and `rename`. Each file is limited to 1 MiB.
- Read the packaged initial value with `context.fs.readBytes('initial', { area: 'assets' })`. An asset key is its logical name and the value is its project-relative source. To grant another directory, add a named entry such as `{"path":"/absolute/path","access":"read"}` to `fs.directories`. Use `read-write` to allow writes. The API rejects paths and links outside the granted area.

  For example, a method that restores the default reads the packaged asset and replaces the service's private file. Here `context` is the argument to `start`, and `call` is the first argument to the public method.

  ```ts
  const initial = await context.fs.readBytes('initial', {
    area: 'assets',
    signal: call.signal,
  });
  await context.fs.writeBytes('state.bin', initial, { signal: call.signal });
  ```

- `context.http.request({ url, method, headers, body, responseType, signal })` returns `status`, lowercase `headers` and `body`. The default body is binary. Select `responseType: 'text'` for UTF-8. Responses with a 4xx or 5xx status carry ordinary response data. Each redirect must target an allowed origin, with a maximum of five redirects. HTTPS uses the operating system's certificate trust and hostname checks. The request and response body limits are each 1 MiB. A generation can have eight requests in flight. `timeoutMs` covers the whole request including redirects. It defaults to 30000 and accepts values from 1 through 120000.

  This method sends a GET request and returns the response text on a 200 status. Declare `errors: ['http-status']` on the method so the page receives the status in the error message for other responses. To try it, change `url` to your API's URL and add its origin to `io.http.origins`. Passing `call.signal` also cancels the HTTP request if the page cancels the call.

  ```ts
  const reply = await context.http.request({
    url: 'https://api.example.org/status',
    responseType: 'text',
    timeoutMs: 5000,
    signal: call.signal,
  });
  if (reply.status !== 200) {
    throw Object.assign(new Error('HTTP ' + reply.status), { code: 'http-status' });
  }
  return reply.body;
  ```

- `context.server.listen({ name, port, fetch, websocket, allowedOrigins, bearerToken })` binds to `127.0.0.1` on ports 1025 through 65535. The `fetch` handler receives the request's `method`, `url`, `headers` and binary `body`, and returns the status, headers and optional text or binary body. Use `websocket.accept` to accept upgrades and `message` to receive text or bytes. The host validates Host and Origin, and checks credentials when you supply a Bearer token. The returned listener's `close()` closes its connections. A resident service can continue listening after its page closes. Stopping its generation releases the listener.

  This `start` handler body echoes HTTP bodies and WebSocket messages. While holding a reference to the service, run `curl --data hello http://127.0.0.1:43123/` to receive `hello`. For browser clients, set `allowedOrigins` to the origin of the page making the connection.

  ```ts
  const listener = await context.server.listen({
    name: 'echo',
    port: 43123,
    allowedOrigins: ['https://app.example.org'],
    fetch: (request) => ({ body: request.body }),
    websocket: {
      accept: () => ({}),
      message: async (socket, data) => await socket.send(data),
    },
  });
  return { methods: {}, stop: async () => await listener.close() };
  ```

For HTTP requests to a listener with a Bearer token, send an Authorization header containing `Bearer <token>`. A browser WebSocket client can offer the `muon.auth` subprotocol together with `muon.bearer.` followed by the Base64URL-encoded token. [WebSocket authentication example (Japanese)](../ja/local-server.md#ブラウザーからのwebsocket認証)

Use `isServiceIoError(error)` to narrow a caught value. Its `code` identifies failures such as `permission-denied`, `tls-error`, `size-limit`, `timeout` or `cancelled`. Native code and Node standard libraries retain the OS permissions of their process. The Android builder adds the `INTERNET` permission when a service uses networking and puts permitted cleartext hosts in the network security configuration. [Android networking permission](https://developer.android.com/develop/connectivity/network-ops/connecting) · [Android network security configuration](https://developer.android.com/privacy-and-security/security-config)

With C ABI v3, start the same operations through `muon_service_context.io->request`. For example, `fs.write` takes JSON area and path fields plus a binary payload. Each completion runs once on the service dispatcher. The returned buffers remain valid for the duration of the callback. C++20 services can include `muon_service_io_cpp.h` and await `muon::request_service_io` in a `cardio` coroutine to receive an owned result. The SDK's `examples/services` directory contains C and TS examples of shutdown persistence, outgoing HTTP requests and local listeners.

## References and autostart

A page reference keeps the service running. When autostart is enabled, the host also holds one resident reference per service.

When a QuickJS service accepts work that must continue after the page closes, call `const release = context.retainWork!()`. Call `release()` when that job completes or fails. Each job has its own release function. The last release lets the host stop the service once its other references are gone. A job retains only the current generation. Check `context.retainWork` for host support.

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

`call(method, args, { signal })` supports cancellation. JS/TS methods receive invocation context as their first argument and can observe its `signal`. Implement cancellation for pending I/O and release resources during shutdown.

The `returns` field describes the value a method's `Promise` resolves to. Replace `start` in the first `echo` service with the following property and grant the service `read-write` access to `io.fs.data`. The implementation returns a `Promise<string>`, so its interface declares `returns: 'string'`.

```ts
start: (context) => ({
  methods: {
    echo: async (call, text): Promise<string> => {
      await context.fs.writeText('last-echo.txt', text, { signal: call.signal });
      return text;
    },
  },
  stop: () => {},
}),
```

On the page, `await handle.call('echo', ['Hello'])` yields `'Hello'` after the write completes. If the write fails, the call rejects and the page can handle the error with `try` / `catch`.

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

To build the source with the application, select an SDK build configuration through `source` instead of `artifact`:

```json
{
  "services": {
    "echo": {
      "runtime": "native",
      "source": "native/echo/muon-plugin.build.json"
    }
  }
}
```

`source` and `artifact` are mutually exclusive. When using `source`, optionally specify `sysroot` for cross-compilation. These paths resolve relative to the directory containing `muon.json`. Install the SDK and the target toolchain. See the [SDK guide](muon-plugin-sdk.md#make-cmake-and-custom-builds) for build systems and dependency declarations.

## Implementing a Node.js service

Node.js services use the same `defineService()` and page APIs as QuickJS. Each service has its own Node.js child process and continues after the UI closes according to the reference rules.

Create a `package.json` in the service project and install its npm dependencies. For example, create `services/worker/package.json`:

```json
{
  "name": "worker",
  "private": true,
  "type": "module",
  "engines": { "node": ">=22.12" }
}
```

```bash
npm install --prefix services/worker muon-plugin-sdk
```

Copy the SDK's `examples/services/node-files.ts` and `node-greeting.txt` into this project, then add the following to `muon.json`. This example reads an asset with the Node.js file API and exposes a `greet` method.

```json
{
  "services": {
    "worker": {
      "runtime": "node",
      "project": "services/worker",
      "entry": "node-files.ts",
      "autostart": true
    }
  }
}
```

`project` resolves relative to the directory containing `muon.json`; `entry` resolves relative to that project. Keep the entry and local sources inside the project. `autostart` and the string values in `config` work as for the other runtimes. Do not specify `artifact`, `source`, or `plugins` for Node.js. A build including Android rejects `runtime: "node"` and identifies the affected service.

TS is compiled to ES modules, preserving relative directory structure and assets in the distribution. File access relative to `import.meta.url` is supported. Runtime npm dependencies and their transitive dependencies are also included, so install dependencies compatible with the target OS beforehand. Preparation fails if required packages are missing. Muon's build does not run `npm install` or package scripts.

Muon prepares a Node.js version that satisfies every service project’s `engines.node` range and the range supported by muon. If no version meets all requirements, the error identifies the conflicting projects and ranges.

Normal shutdown waits for pending methods and callbacks to clean up and for the service's asynchronous `stop()` to finish. The example saves the last name in `context.dataDirectory`, provided to `start`. This directory stores persistent data for the service. Startup and shutdown each have a 10-second deadline. Deadline expiry, child-process crashes, and disconnection are reported as failures.

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

The host does not supply DOM, `window.muon`, Node builtins, `fetch`, or `setInterval`. Use the service’s granted file and network APIs for I/O. The builder produces a single JS bundle and packages declared assets with the service.

The SDK's `quickjs-project` uses the npm package `base64-js` to encode binary data as a Base64 string. To use it in your own service, run `npm install base64-js` in the service project. [base64-js API](https://github.com/beatgammit/base64-js#methods)

```ts
import { fromByteArray } from 'base64-js';

const bytes = Uint8Array.of(0, 128, 255);
console.log(fromByteArray(bytes)); // AID/
```

For examples of reading a file, sending an HTTP request, and answering HTTP or WebSocket clients, see [Service files, outgoing requests and local listeners](#service-files-outgoing-requests-and-local-listeners).

Current limits are 8 MiB per JS bundle, 64 MiB per JS heap, a 1 MiB stack, 2 seconds per JS execution turn, and 10 seconds each for startup and shutdown. Waiting for asynchronous I/O differs from CPU execution time. Runtime errors and exceeded limits are reported as service failures. Unresponsive native cleanup or a process crash can affect other services in the same process.

## Development startup and updates

For CEF, `npx muon run` starts local page assets and services using the same `services` configuration as a distribution build. Projects using Muon's Vite plugin can start the page and services with `npx vite`. A service preparation error also fails UI startup.

Vite watches service sources, assets, configuration, npm dependencies, and native build inputs or artifacts. Once the new configuration is ready, it restarts all services and reloads the page. Acquire services again during page initialization. Unchanged services also restart, and old handles become invalid. `muon run` does not watch for these changes.

HMR is Vite's mechanism for updating changed modules without reloading the entire page. UI-only HMR does not restart the service host. However, destroying a page still stops a nonresident service when its last reference disappears. Closing the UI leaves the development command waiting while resident services remain. `muon.browser.shutdown()`, stopping Vite, or interrupting the development command ends the whole development environment, including its services.

The old services continue running if a build fails, and the next edit retries the update. Startup and shutdown failures also produce diagnostics and can be retried after editing. In those cases, old handles cannot be reused. Consecutive edits are processed sequentially; updates and shutdown reclaim processes and staged files that are no longer used.

Development profiles and service data are separate from the distributed application. Service and UI restarts preserve autostart preferences, but ending the development command deletes that session's settings and data. Development does not register OS login startup. Android supports native and QuickJS builds; device development startup and Vite live preview are not supported.

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

Once a service is running, Android can limit its execution time according to its type. [Foreground service timeouts](https://developer.android.com/develop/background-work/services/fgs/timeout)

Closing the UI differs from an OS stop of the entire application. Android's Active apps stop action terminates the whole app without guaranteed cleanup callbacks. Reference counts do not override that action. See [user-initiated stopping](https://developer.android.com/develop/background-work/services/fgs/handle-user-stopping).

On Android API 30 and later, service host startup reads OS exit history and exposes it as `lastHostExit` in `getState()` and `watchState()` observations. It includes `reason`, the OS `reasonCode`, and `timestampMs`, a decimal string of milliseconds since the Unix epoch. The value is `null` when no record can be obtained, on API 29 and earlier, or on PC. See [ApplicationExitInfo](https://developer.android.com/reference/android/app/ApplicationExitInfo).

This is historical information about the entire service host. Services sharing that host return the same record; it does not establish that each service was running at the time. Use `state` and `error` for the current runtime state and failure.

```ts
const state = await window.muon.services!.getState('echo');
if (state.lastHostExit) {
  console.log(state.lastHostExit.reason, state.lastHostExit.timestampMs);
}
```

### Autostart after a reboot

Once the user has opened the installed app, services can start at the first unlock after a reboot. Muon uses `services.<id>.autostart` as the default (OFF if omitted). A choice saved with `muon.services.setAutostart(id, enabled)` overrides that default in either direction. If all services are OFF, none start. Enabled services that meet Android's startup requirements run before the user opens the app. Opening the UI or receiving another boot notification keeps each running service in the same generation. [Android boot notification](https://developer.android.com/reference/android/content/Intent#ACTION_BOOT_COMPLETED) · [First launch after installation](https://developer.android.com/develop/background-work/services/alarms)

For APKs built by Muon, any of the following types defers the service until the app is visible. This applies even when the service declares multiple types. [Android restrictions by service type](https://developer.android.com/about/versions/15/changes/foreground-service-types)

| Device OS | Foreground service types restricted at boot |
| --- | --- |
| Android 14 and later | `microphone` |
| Additional types on Android 15 and later | `camera`, `dataSync`, `mediaPlayback`, `mediaProjection`, `phoneCall` |

Android can also reject a start when a required permission is missing, for example for a location service. Muon keeps that service's autostart setting ON and reports the reason in the `error` field of `getState(id)`. Other eligible services still start. Muon retries deferred services when the user opens or returns to the app, so a service can start after the user grants the required permission. [Background start restrictions](https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start) · [Build-time permissions and runtime grants](android.md#build-time-permissions-and-runtime-grants)

After a force stop, the user must open the app again. On Android 13 and later, if the user restricts the app's background battery usage, Android delays the boot notification until the app starts for another reason. [Boot notification after leaving the stopped state](https://developer.android.com/reference/android/content/Intent#ACTION_BOOT_COMPLETED) · [Battery usage restrictions](https://developer.android.com/about/versions/13/behavior-changes-13#battery-resource-utilization)
