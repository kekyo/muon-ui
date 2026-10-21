# muon built-in plugin reference

This chapter writes APIs in the `window.muon.*` form to make plugin namespaces and function paths easy to understand.
This is also the object hierarchy actually exposed by `plugin.mode: "simple"`.
In the default `validate` mode, import functions from the corresponding virtual modules and use them.

The Android WebView backend exposes only a subset of the desktop APIs in this chapter. See [Android WebView backend limitations](./limitation.md#android-webview-backend) and the [Android guide](./android.md#configuration-and-muon-apis) for available functions and platform-specific behavior. Functions that are not exposed are not replaced with no-ops.

For example, for `window.muon.executor.spawn`, first allow `muon.executor.spawn` in `plugin.plugins[].imports` or Vite `pluginAccess.plugins[].imports`, then import `spawn` from `muon:executor`:

```ts
import { spawn } from "muon:executor";
```

## HTTP and WebSocket servers on CEF

`window.muon.server.createServer(options)` creates a loopback server owned by the current page. In validate mode, import from `muon:server` and allow `muon.server.createServer`. HTTP handlers use Request/Response, while WebSocket handlers use connection handles. Optional Bearer authentication is supported, and applications can provide MCP endpoints on top of HTTP. Android is not supported. See the [Japanese guide to setup, authentication, lifetime, and MCP](../ja/local-server.md).

## Notifications and Android features

Use `muon.notifications` for local notifications on Android, Windows, and Linux. Android system bars, notification permissions and channels, and OS settings screens are under `muon.android`. The CEF backend does not expose `muon.android`.

| Simple-mode namespace | Validate-mode import | Purpose |
| --- | --- | --- |
| `window.muon.notifications` | `muon:notifications` | Posting, cancellation, and interaction events |
| `window.muon.android.systemBars` | `muon:android.systemBars` | System bars and safe areas |
| `window.muon.android.notifications` | `muon:android.notifications` | Android notification permissions and channels |
| `window.muon.android.settings` | `muon:android.settings` | Application-scoped OS settings screens |

Notification APIs are included only in applications built with `notifications.enabled: true`. Function permissions are also required. For example, allow `muon.notifications.post` through `imports[].allow` in validate mode. Remove notification imports and permission entries when building with notifications disabled. See [notification build settings](./muon-json-reference.md#notifications-key).

### Shared notification API

| Function | Return value | Behavior |
| --- | --- | --- |
| `post(options)` | `Promise<void>` | Post or replace an application-scoped `id` |
| `cancel(id)` | `Promise<void>` | Withdraw one notification; an absent ID also succeeds |
| `getCapabilities()` | `Promise<MuonNotificationCapabilities>` | Read supported features and diagnostic reasons |
| `getStatus(category?)` | `Promise<MuonNotificationStatus>` | Read current posting permission |
| `watchEvents(listener)` | `Promise<MuonNotificationWatcher>` | Subscribe to new operations in this page |
| `getPendingEvents()` | `Promise<readonly MuonNotificationEvent[]>` | Read unacknowledged events, oldest first, without consuming them |
| `acknowledgeEvent(eventId)` | `Promise<void>` | Acknowledge a processed event and remove it from the pending set |

`post()` requires string fields `id`, `title`, and `body`. `category` identifies a purpose; omission uses `notifications.defaultCategory`. Android maps it to a registered channel. CEF accepts arbitrary purpose categories and does not require Android channel settings.

```ts
import { getCapabilities, post } from "muon:notifications";

const capabilities = await getCapabilities();
await post({
  id: "order-42",
  title: "Order ready",
  body: "Your order is ready to collect.",
  category: "orders",
  data: { orderId: 42 },
  ...(capabilities.actions.state === "supported"
    ? { actions: [{ id: "view", title: "View order" }] }
    : {}),
});
```

Limits are 128 UTF-16 code units for `id` and `category`, 1024 for `title`, and 4096 for `body`. The body may be empty. `data` accepts JSON values with a maximum encoded size of 16 KiB in UTF-8 and depth of 64; omitted data becomes `null` in the event. Functions and circular references cannot be stored. Up to three `actions` are allowed. Each action requires nonempty `id` and `title` strings of at most 128 code units, and action IDs must be unique within the notification.

`post()` never requests permission implicitly. It rejects known disablement, unregistered Android categories, and unsupported options such as additional buttons. Success means the OS submission completed; it does not guarantee a banner or that the user read it. OS suppression and retention policies still apply. See [Android notification permissions](https://developer.android.com/develop/ui/views/notifications/notification-permission) and the [Linux notification specification](https://specifications.freedesktop.org/notification/latest/).

Each `getCapabilities()` field is `{ state, reason }`. The state is `supported`, `unsupported`, or `unknown`; `reason` is `null` when no explanation is needed.

| Field | Feature |
| --- | --- |
| `interaction` | Delivering operations to the running application |
| `launch` | Starting an exited application and delivering its operation |
| `persistence` | Keeping notifications after normal process exit |
| `actions` | Additional named buttons |
| `appStatus` | Reading application-wide posting permission |
| `categoryStatus` | Reading OS settings for an individual category |

Feature support and current posting permission are separate. `getStatus()` returns `app` and `category`, whose posting states are `enabled`, `disabled`, or `unknown`. Without an argument, `category` is `null`; the default category is not queried implicitly. A requested category also includes its `id`. Linux may report unknown application and category states; Windows may report an unknown category state. Do not interpret `unknown` as permission granted.

### Notification events and acknowledgement

Events have `{ eventId, id, type, actionId, data }`. Ordinary notification clicks use `type: "activate"` and `actionId: null`; buttons use `type: "action"` and their specified ID. An action named `"default"` remains distinct from an ordinary click. `data` belongs to the posted revision that was operated.

Subscribe before reading pending events, then deduplicate by `eventId`. Returning from a callback does not acknowledge its event. Call `acknowledgeEvent(eventId)` after application processing completes. An exit between processing and acknowledgement allows redelivery, so operations that must not run twice, such as confirming an order, also need application-side deduplication using a stored `eventId`.

`watchEvents()` delivers only new operations, without replaying existing events. Each subscription invokes its listener serially. Release it with the returned `close()` method; page destruction also releases it. Closing a subscription preserves notifications and pending events. Multiple pages may receive the same event, and acknowledgement affects the whole application. Callbacks that have already started are not cancelled.

Unoperated posts and unacknowledged events share 256 reserved slots. New posts are rejected when the store is full. Unoperated posts expire 30 days after posting; pending events expire 30 days after receipt. Removal by the OS or user does not free a slot, so use `cancel()` for IDs that are no longer needed. Replacing an ID invalidates its older unoperated revision while preserving received events. Late operations on an old revision do not create a new event.

On Linux, old and new notifications may briefly coexist during replacement. OS submission and local persistence are not one atomic transaction. Android APK replacement may remove OS notifications, and Muon does not repost them automatically. Updates preserving the application ID and data retain saved pending events. Uninstalling or clearing application data is outside that guarantee. See [Android package changes and notification removal](https://android.googlesource.com/platform/frameworks/base/+/1cdfff555f4a21f71ccc978290e2e212e2f8b168/services/core/java/com/android/server/notification/NotificationManagerService.java#2010).

See [notification deployment registration](./package-install-location-advanced-topics.md#notification-deployment-registration) for CEF launch requirements and update/removal procedures.

### Android system bars and safe areas

| Function | Return value | Behavior |
| --- | --- | --- |
| `configure(options)` | `Promise<void>` | Request bar visibility, icon colors, and content placement |
| `getState()` | `Promise<MuonAndroidSystemBarsState>` | Read `requested` settings and OS-reported `actual` values |
| `getInsets()` | `Promise<MuonAndroidSystemBarsInsets>` | Wait for initial geometry and read current insets |
| `watchInsets(listener)` | `Promise<MuonAndroidInsetsWatcher>` | Subscribe to initial geometry and later changes; release with `close()` |

`configure()` applies a partial update. Set `visible` and `icons: "light" | "dark"` in `statusBar` and `navigationBar`; omitted values retain their requests. `contentLayout` is `safe-area` or `edge-to-edge`. Unsupported fields and invalid values reject the entire request without partial application. Dark navigation icons are unsupported on API 24 and 25.

`requested` contains application requests and is unaffected by transient swipe reveals. `actual` contains values reported by Android's public APIs. Before initialization, each bar's `visible` and `icons` fields are `null`. These are not measurements of rendered pixels: a transient overlay may still be reported as hidden. On API 29 and earlier, AndroidX also approximates visibility from available insets. See [WindowInsets.isVisible](https://developer.android.com/reference/android/view/WindowInsets#isVisible(int)) and [Android InsetsController](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/core/java/android/view/InsetsController.java).

`browser.enterFullscreen()` requests both bars hidden, and `exitFullscreen()` requests both visible, sharing the same state. These operations preserve icon colors and content placement. `configure()` completion means the request was applied, not that OS animation has finished.

Insets are distances from each edge, returned as `top`, `right`, `bottom`, and `left` in CSS pixels. `systemBars` covers visible bars, `displayCutout` covers notches and cutouts, `systemGestures` covers gesture-reserved areas, and `ime` covers the software keyboard. `safeArea` is the per-edge maximum of bars and cutouts. Keyboard and gesture insets are not added automatically.

In `safe-area` mode, native layout reserves the safe area and the additional web clearance, `contentSafeArea`, is zero. In `edge-to-edge` mode, the page handles placement. CSS variables `--muon-safe-area-top`, `--muon-safe-area-right`, `--muon-safe-area-bottom`, and `--muon-safe-area-left` correspond to `contentSafeArea` and update after rotation or zoom. On Android, do not add the same clearance again through CSS `env(safe-area-inset-*)`.

```css
.app-controls {
  padding: var(--muon-safe-area-top, 0px)
    var(--muon-safe-area-right, 0px)
    var(--muon-safe-area-bottom, 0px)
    var(--muon-safe-area-left, 0px);
}
```

See the [Android guide](./android.md#system-bars-and-safe-areas) for initial layout and bar settings.

### Android notification permissions and channels

| Function | Behavior |
| --- | --- |
| `getPermissionState()` | Read runtime permission and application-wide notification settings |
| `requestPermission()` | Explicitly request permission from the foreground Activity |
| `getCapabilities()` | Read Android-specific `channels` support |
| `ensureChannel(options)` | Register a channel and return its effective OS settings |
| `getChannel(id)` | Read channel settings; return `null` if unregistered |

`getPermissionState()` returns `runtimePermission: "not-required" | "granted" | "denied"`, `notificationsEnabled`, `requestedBefore`, `shouldShowRationale`, and `settingsSuggested`. Below API 33, runtime permission is `not-required`, but OS settings can still disable notifications. `denied` includes never requested. `requestedBefore` records that Muon called the request API; it does not prove that a dialog appeared. `settingsSuggested` is not a permanent-denial diagnosis.

`requestPermission()` returns fresh `state` and `result: "granted" | "not-granted" | "interrupted" | "not-required"`. `not-granted` cannot distinguish every denial, dismissal, and suppressed dialog. Concurrent requests, a missing foreground Activity, and destruction of the requesting page are errors. Startup and posting never display a permission dialog automatically. See [Android permission outcomes](https://developer.android.com/develop/ui/views/notifications/notification-permission).

Channels are available on API 26 and later. Registration requires `id`, `name`, and `importance: "min" | "low" | "default" | "high"`; `description` is optional. Existing user settings are preserved. The returned object is `{ id, name, description, importance, enabled }`; a disabled importance is `none`. API 24 and 25 reject channel operations and accept posts only to statically declared categories. See [Android channels](https://developer.android.com/develop/ui/views/notifications/channels).

### Android OS settings screens

`isAvailable(target)` checks the requested screen itself. `open(target, options?)` opens it from the foreground Activity and returns `{ requestedTarget, target, fallbackUsed }`. It does not report whether the user changed a setting. Read permission and status again after the application resumes.

Targets are `"app-details"`, `"notifications"`, and `{ type: "notification-channel", channelId }`. Channel settings require API 26 or later and a registered ID. If a dedicated notification screen is unavailable, app details are used only when explicitly permitted with `{ fallback: "app-details" }`. The dedicated notification screen is unsupported on API 24 and 25. A missing Activity or destination rejects the Promise. See [Android settings actions](https://developer.android.com/reference/android/provider/Settings).


## muon.browser namespace

`window.muon.browser` controls the current muon browser window and page display.

| Function | Arguments | Return value | Description |
| :------- | :-------- | :----------- | :---------- |
| `reload()` | none | `Promise<void>` | Reloads the current page. |
| `hardReload()` | none | `Promise<void>` | Reloads the current page while ignoring cache. |
| `toggleFullscreen()` | none | `Promise<void>` | Toggles fullscreen state. |
| `enterFullscreen()` | none | `Promise<void>` | Enters fullscreen state. |
| `exitFullscreen()` | none | `Promise<void>` | Leaves fullscreen state. |
| `zoomIn()` | none | `Promise<void>` | Increases the page zoom level. |
| `zoomOut()` | none | `Promise<void>` | Decreases the page zoom level. |
| `resetZoom()` | none | `Promise<void>` | Resets the page zoom level to the initial value. |
| `show()` | none | `Promise<void>` | Shows the current window. |
| `hide()` | none | `Promise<void>` | Hides the current window. |
| `focus()` | none | `Promise<void>` | Focuses the current window. |
| `blur()` | none | `Promise<void>` | Removes focus from the current window. |
| `minimize()` | none | `Promise<void>` | Minimizes the current window. |
| `maximize()` | none | `Promise<void>` | Maximizes the current window. |
| `restore()` | none | `Promise<void>` | Restores a minimized or maximized window to its normal state. |
| `getWindowBounds()` | none | `Promise<MuonWindowBounds>` | Gets the current top-level window bounds. |
| `setWindowBounds(bounds)` | `bounds: MuonWindowBounds` | `Promise<void>` | Requests a change to the current top-level window bounds. |
| `setContextMenuItems(items, handler?)` | `items: MuonBrowserContextMenuItem[]`, `handler?: (command) => void` | `Promise<void>` | Registers custom items added to the native context menu. |
| `clearContextMenuItems()` | none | `Promise<void>` | Clears registered custom context menu items. |
| `createTray(options, handler?)` | `options: MuonBrowserTrayOptions`, `handler?: (event) => void` | `Promise<string>` | Creates a browser-owned system tray item. |
| `setTrayMenu(id, items, handler?)` | `id: string`, `items: MuonBrowserTrayMenuItem[]`, `handler?: (event) => void` | `Promise<void>` | Replaces the menu and event handler for a system tray item. |
| `setTrayIcon(id, iconPath)` | `id: string`, `iconPath: string` | `Promise<void>` | Replaces the PNG icon for a system tray item. |
| `setTrayTooltip(id, tooltip)` | `id: string`, `tooltip: string \| null` | `Promise<void>` | Sets or clears the tooltip for a system tray item. |
| `removeTray(id)` | `id: string` | `Promise<void>` | Removes a system tray item. |
| `setTitleBarVisibility(visible)` | `visible: boolean` | `Promise<void>` | Toggles title-bar visibility. |
| `setTitleBarIcon(path)` | `path: string \| null` | `Promise<void>` | Sets or clears the title-bar icon for the current window. |
| `close()` | none | `Promise<void>` | Closes the current window. |
| `shutdown(exitCode?)` | `exitCode?: number` | `Promise<void>` | Terminates the muon process. If `exitCode` is omitted, it is `0`. |
| `recycle()` | none | `Promise<void>` | Terminates the muon process and automatically restarts it when the launcher supports it. |

- `reload()`, `hardReload()`, `close()`, `shutdown()`, and `recycle()` may destroy the page context or terminate the process, so the JavaScript execution environment can disappear before the returned Promise is observed.
- `recycle()` automatically restarts only when the launcher, such as `muon-launcher` or `muon run`, supports the recycle exit code. `shutdown(88)` is rejected because it is the reserved exit code for recycle.
- `close()` aborts modal file dialogs owned by the target window before closing the window.
- The bounds for `getWindowBounds()` and `setWindowBounds()` are the top-level window bounds, including the muon custom title bar or native frame, not the browser display area.
  Coordinates and sizes use the same DIP screen coordinates as CEF Views.
  `setWindowBounds()` requires `x`, `y`, `width`, and `height` to be safe integers in the signed 32-bit integer range, and `width` and `height` must be 1 or greater.
  On Wayland, top-level window placement is managed by the compositor, so position and size requests may be ignored or adjusted.
  Use an X11 backend, such as `--ozone-platform=x11`, when strict position control is required.
- `setContextMenuItems()` keeps one registration per browser window, and calling it again replaces the registration.
  Registration is cleared by main frame navigation, browser exit, or `clearContextMenuItems()`.
  Normal items can specify `id`, `label`, `enabled`, `placement`, and `when`. Separators specify `type: "separator"`.
  `placement` is one of `"start"`, `"afterEdit"`, and `"end"`, and defaults to `"end"`.
  `when` can specify boolean conditions `editable`, `selection`, `link`, `image`, `canCopy`, and `canPaste`.
  `id` cannot be an empty string, contain control characters, start with `muon.`, or start with `standard.`.
- `createTray()` keeps tray items per browser window, and they are removed by main frame navigation, browser exit, or `removeTray()`.
  The number of live tray items is limited to 16 per browser window and 64 per process. Requests exceeding the limit are rejected before native resources are allocated.
  When `options.id` is omitted, muon generates and returns a unique ID.
  `id` cannot be an empty string, contain control characters, start with `muon.`, or start with `standard.`.
  Menu items support normal items, `type: "separator"`, `type: "checkbox"`, and `type: "radio"`.
- `setTitleBarVisibility()` toggles visibility of the muon custom title bar.
  On Linux X11 native title bars, it sets a native-decoration visibility hint on the window manager.
  This hint depends on the window manager and may not be reflected in unsupported environments.
- `setTitleBarIcon()` receives an icon asset path. Passing `null` clears the title-bar icon for the current window.
  `path` can be an `asset://main/` URL such as `"asset://main/icons/app.png"`, or an asset path relative to `main`, such as `"icons/app.png"`.
  PNG encoded data must be 1 MiB or smaller, width and height must each be 256 px or smaller, and the total pixel count must be 65,536 or less. These limits also apply to PNGs specified for `createTray()` and `setTrayIcon()`.
  The `"muon"` title bar can specify image formats the browser can display, such as SVG.
  The `"native"` title bar rejects the Promise when a non-PNG image is specified.

```js
await window.muon.browser.zoomIn();
await window.muon.browser.resetZoom();
const bounds = await window.muon.browser.getWindowBounds();
await window.muon.browser.setWindowBounds({
  ...bounds,
  width: 960,
  height: 640,
});
await window.muon.browser.setContextMenuItems(
  [
    {
      id: "app.searchSelection",
      label: "Search Selection",
      placement: "afterEdit",
      when: { selection: true },
    },
    { type: "separator", placement: "end" },
  ],
  (command) => {
    console.log(command.id, command.selectionText);
  },
);
const trayId = await window.muon.browser.createTray(
  {
    id: "main",
    icon: "icons/app.png",
    tooltip: "Ready",
    menu: [
      { id: "open", label: "Open" },
      { type: "separator" },
      { id: "quit", label: "Quit" },
    ],
  },
  async (event) => {
    if (event.type === "activate" || event.id === "open") {
      await window.muon.browser.show();
      return;
    }
    if (event.type === "menu" && event.id === "quit") {
      await window.muon.browser.shutdown(0);
    }
  },
);
await window.muon.browser.setTrayTooltip(trayId, "Running");
await window.muon.browser.setTitleBarVisibility(false);
await window.muon.browser.setTitleBarIcon("icons/app.png");
await window.muon.browser.shutdown(0);
await window.muon.browser.recycle();
```

## muon.launcher namespace

`window.muon.launcher` handles runtime catalog update settings used the next time `muon-launcher` starts.
Settings are saved in `muon-launcher.ini` in the runtime directory and do not affect the currently running CEF or Node.js sidecars.

| Function | Arguments | Return value | Description |
| :------- | :-------- | :----------- | :---------- |
| `getSettings()` | none | `Promise<MuonLauncherSettings>` | Returns the currently effective launcher settings. |
| `setSettings(settings)` | `MuonLauncherSettingsPatch` | `Promise<void>` | Saves the CEF version policy and runtime catalog refresh interval used on the next launch. Items specified as `null` remove explicit settings. |
| `triggerUpdate()` | none | `Promise<void>` | Requests update attempts for the applicable CEF and Node.js catalogs on the next `muon-launcher` launch. |

```js
await window.muon.launcher.setSettings({
  cefVersionPolicy: "compat-latest",
  catalogRefreshIntervalSeconds: 604800,
});
await window.muon.launcher.triggerUpdate();
```

- `cefVersionPolicy` and `cefExactVersion` apply only to CEF. The Node.js version is selected from the Node runtime requirement embedded into the launcher at build time; this API exposes no Node.js version-policy setting.
- `catalogRefreshIntervalSeconds` and `triggerUpdate()` apply to every catalog that is applicable during the next runtime preparation. The Node.js catalog is applicable only when the application requires Node.js at runtime.
- Catalog updates and runtime replacement are not performed while the application is running. The current CEF and all Node.js sidecars continue unchanged, and updates take effect during the next launcher startup.

## muon.environments namespace

`window.muon.environments` handles environment information for the muon process and autostart settings.

| Function | Arguments | Return value | Description |
| :------- | :-------- | :----------- | :---------- |
| `getVariables()` | none | `Promise<Record<string, string>>` | Returns the current process environment variables. |
| `getConfigValues()` | none | `Promise<Record<string, string>>` | Returns the effective top-level application `config` values. |
| `getCommandLine()` | none | `Promise<string[]>` | Returns the command line recorded when muon started. Includes `argv[0]` when available. |
| `getProcessId()` | none | `Promise<number>` | Returns the native muon process ID. |
| `getRuntimeInfo()` | none | `Promise<MuonRuntimeInfo>` | Returns runtime information for the current backend. |
| `getAutostart()` | none | `Promise<boolean \| undefined>` | Returns whether the current app is configured to start automatically when the user session starts. Returns `undefined` if it cannot be determined. |
| `setAutostart(enabled)` | `enabled: boolean` | `Promise<void>` | Enables or disables autostart. |

- `getRuntimeInfo()` returns a type discriminated by `backend`.
  When `backend === "cef"`, it returns the existing muon-core build, referenced CEF, and running CEF information. `muonCore` contains `version`, `gitCommitHash`, `buildDate`, and `gitCommitDate`; the dates are ISO 8601 strings.
  When `backend === "android-webview"`, it returns the Android OS version/API level, ABI, application ID/version, and WebView provider package/version. CEF-specific fields are absent.
- The Android WebView backend exposes only `getVariables()`, `getConfigValues()`, `getProcessId()`, and `getRuntimeInfo()` in this namespace. It does not expose `getCommandLine()`, `getAutostart()`, or `setAutostart()`.
- Android can recreate the app process, so do not use the process ID or runtime information as an installation ID or persistent session ID.
- `getAutostart()` and `setAutostart()` use the platform backend corresponding to the launch source.
  POSIX desktop uses XDG Autostart, and Windows uses the current user's Run registry entry.

On Windows, the complete autostart command, including quotes, must fit within 260 UTF-16 code units. UTF-16 is the Windows string representation; most Japanese characters use one unit, and many emoji use two. Exceeding the limit rejects `setAutostart(true)` with `Windows Run command is too long` without creating the entry. See [Run registry limits](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys).

```js
const variables = await window.muon.environments.getVariables();
const config = await window.muon.environments.getConfigValues();
const processId = await window.muon.environments.getProcessId();
const runtimeInfo = await window.muon.environments.getRuntimeInfo();

if (runtimeInfo.backend === "android-webview") {
  console.log(runtimeInfo.apiLevel, runtimeInfo.webViewVersion);
} else {
  const commandLine = await window.muon.environments.getCommandLine();
  const autostart = await window.muon.environments.getAutostart();
  console.log(runtimeInfo.muonCore.version, runtimeInfo.cefRuntime.version);
  console.log(commandLine);
  if (autostart !== true) {
    await window.muon.environments.setAutostart(true);
  }
}
```

In `validate` mode, allow `muon.environments.getConfigValues` for the importer and use the `muon:environments` virtual module:

```ts
import { getConfigValues } from "muon:environments";

const config = await getConfigValues();
const baseUrl = config.apiBaseUrl;
```

## muon.executor namespace

`window.muon.executor` collects capabilities that execute functionality outside muon.
For example, it can launch child processes or call specified functions in dynamic libraries.

These capabilities are very powerful and are not protected by muon or CEF security boundaries.
They can execute arbitrary executables by path and run native code from arbitrary dynamic libraries without restriction.

Use them with great care.
If these capabilities are not needed, do not include them in the `muon.json` whitelist.

| Function | Arguments | Return value | Description |
| :------- | :-------- | :----------- | :---------- |
| `spawn(options)` | `options: MuonExecutorSpawnOptions` | `Promise<MuonExecutorProcess>` | Launches a child process and returns a handle for operation. |
| `loadLibrary(path)` | `path: string` | `Promise<MuonAdhocLibrary>` | Loads a `.so` / `.dll` and returns a handle for ad-hoc FFI. |

### Running child processes

`MuonExecutorSpawnOptions`:

| Property | Type | Description |
| :------- | :--- | :---------- |
| `command` | `string` | Executable path, or executable name resolved from `PATH`. Required. Empty strings and NUL characters cannot be used. |
| `args` | `readonly string[]` | Command-line arguments. No shell interpretation is performed, and each element is passed to the child process as-is. |
| `cwd` | `string` | Working directory for the child process. |
| `env` | `Record<string, string>` | Environment variable overrides. Merged with the current process environment. Keys cannot be empty strings or contain `=` or NUL characters. |
| `daemon` | `boolean` | Whether to launch the process as a daemon. Defaults to `false` when omitted. |
| `onStdout` | `(chunk: Uint8Array) => void` | Receives stdout chunks sequentially. If specified, `stdout` is not included in the `wait()` result. |
| `onStderr` | `(chunk: Uint8Array) => void` | Receives stderr chunks sequentially. If specified, `stderr` is not included in the `wait()` result. |

`MuonExecutorProcess`:

| Property/function | Type | Description |
| :---------------- | :--- | :---------- |
| `processId` | `number` | Child process ID that was launched. |
| `writeStdin(data)` | `(data: string \| BufferSource) => Promise<void>` | Writes to stdin sequentially. Strings are encoded as UTF-8 and processed in call order. |
| `closeStdin()` | `() => Promise<void>` | Closes stdin after processing pending writes. |
| `wait()` | `() => Promise<MuonExecutorSpawnResult>` | Waits for the root process to exit. Reuses the same Promise. |
| `kill()` | `() => Promise<void>` | Requests termination of the entire process tree managed by the connected handle. |
| `release()` | `() => Promise<void>` | Releases the native handle. Process handling depends on `daemon`. |

`MuonExecutorSpawnResult`:

| Property | Type | Description |
| :------- | :--- | :---------- |
| `processId` | `number` | Child process ID that was launched. |
| `exitCode` | `number` | Child process exit code. The Promise resolves even on non-zero exits. |
| `stdout` | `Uint8Array` | Bytes collected as stdout when `onStdout` is not specified. |
| `stderr` | `Uint8Array` | Bytes collected as stderr when `onStderr` is not specified. |

With `daemon: false`, muon owns the launched process tree.
It terminates the remaining process tree when the root process exits naturally, `release()` is called, the context is released, or muon exits.

With `daemon: true`, `release()`, context release, and muon exit detach a running process tree without sending it a signal.
In either mode, `wait()` resolves based on the root process exit and automatically releases the handle.
Therefore, with `daemon: true`, descendants that remain after the root process exits are detached.
However, while the handle is connected, `kill()` terminates the entire process tree in either mode.

Releasing a handle also closes muon's standard I/O endpoints and callbacks.
A detached process may observe EOF on stdin or write failures on stdout or stderr.
A released process cannot be reconnected.

On Linux, the process tree consists of processes in the process group created at launch.
A process that removes itself from that group is outside muon's control.
On Windows, managed processes are members of a Job Object.

On Windows, `command` accepts a long executable path. Pass arguments separately through `args`. A bare command name keeps its existing search behavior. The resolved `cwd` must be shorter than 260 UTF-16 code units. Limits on arguments and files handled by the child program depend on that program. See [Windows process creation and working directories](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setcurrentdirectory).

```js
const child = await window.muon.executor.spawn({
  command: "node",
  args: ["script.js"],
  onStdout: (chunk) => console.log(new TextDecoder().decode(chunk)),
});

await child.writeStdin("input text");
await child.closeStdin();

const result = await child.wait();
console.log(result.exitCode);
```

For example, specify `daemon: true` to keep a process running after muon exits:

```js
const service = await window.muon.executor.spawn({
  command: "node",
  args: ["service.js"],
  daemon: true,
});

await service.release();
```

### Loading and running dynamic libraries

`loadLibrary()` loads the specified dynamic library and makes entry points in that library callable.

Use the returned handle (`MuonAdhocLibrary`) to locate function entry points in the library and obtain JavaScript function objects.
You can call dynamic library functionality through those function objects.

`MuonAdhocLibrary`:

| Function | Type | Description |
| :------- | :--- | :---------- |
| `getFunction(name, signature)` | `<T>(name: string, signature: MuonAdhocSignature) => Promise<T>` | Resolves a native symbol and returns an async JavaScript proxy function. |
| `release()` | `() => Promise<void>` | Rejects new calls and releases the library after in-flight calls complete. |

The expected signature, meaning argument and return types, of the target entry point must be specified as an argument to `getFunction()`.
Therefore, entry points with unknown signatures cannot be called.

`MuonAdhocSignature` is `{ argTypes, returnType }`.
Type constants can be `voidType`, `boolType`, `int8Type`, `uint8Type`, `int16Type`, `uint16Type`, `int32Type`, `uint32Type`, `int64Type`, `uint64Type`, `float32Type`, `float64Type`, `stringType`, `pointerType`, `bufferViewType`, and `usizeType`.
64-bit integers and `usize` are carried as decimal strings in JSON, and JavaScript arguments can pass `number`, `bigint`, or `string`.

The following example calls `malloc` and `free` in `libc.so`:

```ts
import {
  loadLibrary,
  pointerType,
  usizeType,
  voidType,
} from "muon:executor";

type MallocType = (size: MuonAdhocIntegerValue) => Promise<MuonNativePointer>;
type FreeType = (p: MuonNativePointer) => Promise<void>;

// Load libc.so.
const library = await loadLibrary("libc.so");
try {
  // Get the function object for malloc.
  const malloc = await library.getFunction<MallocType>("malloc", {
    argTypes: [usizeType],
    returnType: pointerType,
  });

  // Get the function object for free.
  const free = await library.getFunction<FreeType>("free", {
    argTypes: [pointerType],
    returnType: voidType,
  });

  // Call it.
  const p = await malloc(123n);
  await free(p);
} finally {
  await library.release();
}
```

- All entry point functions are assumed to have ordinary return-value signatures, meaning synchronous return values.
  However, on the JavaScript side, functions returned by `getFunction()` always return `Promise<T>`.
  In other words, the entry point function is called synchronously internally, but JavaScript must always use `await` and wait for `Promise<T>` to complete.
- Entry point calls run on temporary worker threads, so even if a function blocks for a long time, muon operation is not affected.
  However, when releasing a library loaded by `loadLibrary()` with `release()`, all Promises for these function calls must be completed. If they are not complete, `release()` waits.

> Note: This feature is intended for very simple and ad-hoc use.
> For example, return-value and argument pointers are handled as `MuonNativePointer`, and muon does not manage ownership.
> Also, type metadata definitions handled by `getFunction()` cannot define structures with arbitrary layouts.
> If you need complex callback ownership, long-lived or strictly managed instances, or data exchange through structures, consider implementing a normal muon plugin instead.

## muon.fs namespace

`window.muon.fs` operates on the local file system.
Each `path`, `source`, `destination`, and `target` argument is a string representing a file location.
Many functions can specify `{ signal?: AbortSignal }` as their last argument.
If an already-aborted signal is passed, the function rejects immediately. If abortion occurs during processing, native processing cancellation is requested where possible.

On Linux, GIO/GVfs is used, so local paths or URIs can be specified for file location arguments passed to normal `muon.fs` functions.
When `gtk.localOnly: false` in GTK file dialogs and a URI on GVfs is returned as the selection result, that URI can also be passed to normal `muon.fs` functions.
On non-Linux environments, normal `muon.fs` functions handle paths on the local file system.
The current Android WebView implementation handles only real filesystem paths allowed by Android. It currently rejects `content://` URIs; direct URI support is deferred. `muon.fs.dialogs` also remains unavailable on Android until that URI contract is implemented.

On Windows, ordinary long paths can be used for reading, writing, enumeration, copying, moving, removal, and watching. Long paths require no administrator action or OS policy change. Relative paths retain their existing base directory. A relative `symlink()` target is stored as relative link text.

`realpath()` returns an ordinary absolute path when it identifies the same object. It preserves an explicitly supplied `\\?\` prefix, or one needed to represent names such as those ending in spaces or periods. Relative targets returned by `readlink()` remain relative. Pass returned paths directly into subsequent `muon.fs` operations without stripping prefixes, and preserve the names returned by `readdir()`. See [Windows placement restrictions and path lengths](./limitation.md#long-paths-on-windows).

| Function | Arguments | Return value | Description |
| :------- | :-------- | :----------- | :---------- |
| `readFile(path, options?)` | `path: string`, `options?: { position?: number, length?: number, signal?: AbortSignal }` | `Promise<ArrayBuffer>` | Reads a binary file. `position` is the read start byte position, and `length` is the requested byte count. Both must be non-negative safe integers. |
| `writeFile(path, data, options?)` | `path: string`, `data: BufferSource`, `options?: { position?: number, signal?: AbortSignal }` | `Promise<void>` | Writes binary data. When `position` is omitted, replaces the whole file. When specified, writes at that byte position. |
| `readTextFile(path, encoding, options?)` | `path: string`, `encoding: "utf8" \| "utf-8"`, `options?: { signal?: AbortSignal }` | `Promise<string>` | Reads a UTF-8 text file. The file must be valid UTF-8 without NUL characters, and raw byte count must be within the configured limit. |
| `writeTextFile(path, data, encoding, options?)` | `path: string`, `data: string`, `encoding: "utf8" \| "utf-8"`, `options?: { signal?: AbortSignal }` | `Promise<void>` | Replaces the whole file as UTF-8 text. |
| `stat(path, options?)` | `path: string`, `options?: { signal?: AbortSignal }` | `Promise<MuonFsStats>` | Follows symbolic links and returns metadata. Rejects if the path does not exist. |
| `lstat(path, options?)` | `path: string`, `options?: { signal?: AbortSignal }` | `Promise<MuonFsStats>` | Returns metadata without following symbolic links. |
| `exists(path, options?)` | `path: string`, `options?: { signal?: AbortSignal }` | `Promise<boolean>` | Returns `true` if the path exists and can be inspected. File-system errors during inspection become `false`. |
| `access(path, options?)` | `path: string`, `options?: { mode?: readonly ("read" \| "write" \| "execute")[], signal?: AbortSignal }` | `Promise<boolean>` | Returns `true` when the path exists and all specified access modes are allowed. When `mode` is omitted, only existence is checked. |
| `readdir(path, options?)` | `path: string`, `options?: { withFileTypes?: false, signal?: AbortSignal }` | `Promise<string[]>` | Returns directory entry names. |
| `readdir(path, options)` | `path: string`, `options: { withFileTypes: true, signal?: AbortSignal }` | `Promise<MuonFsDirent[]>` | Returns directory entry names and metadata. |
| `mkdir(path, options?)` | `path: string`, `options?: { recursive?: boolean, signal?: AbortSignal }` | `Promise<void>` | Creates a directory. `recursive` defaults to `false`. |
| `rm(path, options?)` | `path: string`, `options?: { recursive?: boolean, force?: boolean, signal?: AbortSignal }` | `Promise<void>` | Removes a file or directory. Removing a directory tree requires `recursive: true`. `force: true` suppresses path-not-found errors. |
| `unlink(path, options?)` | `path: string`, `options?: { signal?: AbortSignal }` | `Promise<void>` | Removes a file or symbolic link. Rejects for directories. |
| `rmdir(path, options?)` | `path: string`, `options?: { signal?: AbortSignal }` | `Promise<void>` | Removes an empty directory. |
| `rename(oldPath, newPath, options?)` | `oldPath: string`, `newPath: string`, `options?: { signal?: AbortSignal }` | `Promise<void>` | Renames or moves a path. |
| `copyFile(source, destination, options?)` | `source: string`, `destination: string`, `options?: { overwrite?: boolean, signal?: AbortSignal }` | `Promise<void>` | Copies a regular file. `overwrite` defaults to `true`. |
| `appendFile(path, data, options?)` | `path: string`, `data: BufferSource`, `options?: { signal?: AbortSignal }` | `Promise<void>` | Appends binary data to the end of a file. Creates the file if it does not exist. |
| `appendTextFile(path, data, encoding, options?)` | `path: string`, `data: string`, `encoding: "utf8" \| "utf-8"`, `options?: { signal?: AbortSignal }` | `Promise<void>` | Appends UTF-8 text to the end of a file. Creates the file if it does not exist. |
| `truncate(path, length?, options?)` | `path: string`, `length?: number`, `options?: { signal?: AbortSignal }` | `Promise<void>` | Truncates or extends a file to the specified byte length. `length` is a non-negative safe integer and defaults to `0`. |
| `truncate(path, options)` | `path: string`, `options: { signal?: AbortSignal }` | `Promise<void>` | Passes `options` as the second argument and truncates the file to 0 bytes. |
| `realpath(path, options?)` | `path: string`, `options?: { signal?: AbortSignal }` | `Promise<string>` | Returns a normalized absolute path. Rejects if the path or any component does not exist. |
| `readlink(path, options?)` | `path: string`, `options?: { signal?: AbortSignal }` | `Promise<string>` | Returns the target of a symbolic link. |
| `symlink(target, path, type?, options?)` | `target: string`, `path: string`, `type?: "file" \| "dir" \| "junction"`, `options?: { signal?: AbortSignal }` | `Promise<void>` | Creates a symbolic link. `type` defaults to `"file"`. `"dir"` and `"junction"` create directory links. |
| `symlink(target, path, options)` | `target: string`, `path: string`, `options: { signal?: AbortSignal }` | `Promise<void>` | Passes `options` as the third argument and creates a file link. |
| `watch(path, listener, options?)` | `path: string`, `listener: (event: MuonFsWatchEvent) => void \| Promise<void>`, `options?: { signal?: AbortSignal }` | `Promise<MuonFsWatcher>` | Watches path changes and returns a watcher. Currently reports differences by polling snapshots. |

- `readFile()` enforces a read limit per operation in the native layer. The default is 64 MiB (`67108864` bytes).
  If an explicit `length` exceeds the limit, the `Promise` is rejected before accessing the file.
- When `length` is omitted, all bytes from `position` to the end of the file must be within the limit, and no implicit truncation to the limit is performed.
  If `length` is within the configured limit and `position` is at or beyond the end of the file, an empty `ArrayBuffer` is returned. `length: 0` also returns an empty `ArrayBuffer` without accessing the file.
- `readTextFile()` also enforces an independent raw byte read limit per operation in the native layer. The default is 64 MiB (`67108864` bytes).
  Sources exactly at the limit can be read, but oversized sources are rejected before UTF-8/NUL validation or string conversion, without truncation.

Limits can be changed in the built-in plugin settings in `muon.json`.
The JavaScript API signature does not change.

```json
{
  "plugin": {
    "plugins": [
      {
        "name": "internal",
        "config": {
          "fs.readFile.maxBytes": "67108864",
          "fs.readTextFile.maxBytes": "67108864"
        }
      }
    ]
  }
}
```

`fs.readFile.maxBytes` and `fs.readTextFile.maxBytes` are unsigned decimal integer strings in bytes.
Empty strings, signs, whitespace, decimal points, exponential notation, and unit suffixes cannot be used, and the value must be within the `uint64_t` range.
Leading zeros and `"0"` are valid.
When `fs.readFile.maxBytes` is `"0"`, only empty ranges succeed. When `fs.readTextFile.maxBytes` is `"0"`, only empty sources succeed.
Invalid values do not fall back to defaults and instead fail built-in plugin initialization.
The two limits are independent and apply only to their corresponding functions.
Neither is a quota accumulated across multiple operations.

`MuonFsStats`:

| Property/method | Type | Description |
| :-------------- | :--- | :---------- |
| `type` | `"file" \| "directory" \| "symlink" \| "blockDevice" \| "characterDevice" \| "fifo" \| "socket" \| "other"` | Entry type. |
| `size` | `number` | Byte size of a regular file. `0` for other types. |
| `mtimeMs` | `number` | Last modification time, represented as milliseconds since the Unix epoch. |
| `readonly` | `boolean` | `true` when no write permission bits are set. |
| `isFile()` | `() => boolean` | Returns `true` when `type === "file"`. |
| `isDirectory()` | `() => boolean` | Returns `true` when `type === "directory"`. |
| `isSymbolicLink()` | `() => boolean` | Returns `true` when `type === "symlink"`. |

- `MuonFsDirent` is `MuonFsStats` plus `name: string`.
- `name` is the entry name relative to the directory that was read.

`MuonFsWatchEvent`:

| Property | Type | Description |
| :------- | :--- | :---------- |
| `eventType` | `"rename" \| "change" \| "error"` | Notification type. Creation and deletion are reported as `"rename"`. |
| `filename` | `string \| null` | Changed entry name. `null` when the watched target itself changed. |
| `message` | `string` | Error message when `eventType === "error"`. |

- `MuonFsWatcher` has `close(): Promise<void>`.
- Calling `close()` multiple times is safe.
- If a `watch()` `listener` throws or returns a rejected `Promise`, the error is ignored.
  If aborted before watcher creation, `watch()` rejects. If aborted after creation, the watcher is closed.
- A single renderer V8 context can hold at most 16 filesystem watchers at the same time.
  `watch()` calls exceeding the limit reject with `"Filesystem watcher limit exceeded"` before starting the initial snapshot.

```js
await window.muon.fs.writeTextFile("/tmp/muon-note.txt", "hello\n", "utf8");
const text = await window.muon.fs.readTextFile("/tmp/muon-note.txt", "utf8");

const entries = await window.muon.fs.readdir("/tmp", { withFileTypes: true });
for (const entry of entries) {
  console.log(entry.name, entry.isDirectory() ? "dir" : entry.type);
}

const watcher = await window.muon.fs.watch("/tmp/muon-note.txt", (event) => {
  console.log(event.eventType, event.filename);
});
await watcher.close();
```

## muon.fs.dialogs namespace

`window.muon.fs.dialogs` shows native file dialogs.
Functions in this namespace do not create or modify files or directories. They only return paths or URIs selected by the user.

On Windows, the shell may return an 8.3 short alias or a path prefixed with `\\?\`. Its spelling may differ from the supplied initial folder. Pass the result directly to `muon.fs`. File I/O and the shell have separate long-path restrictions. See [Microsoft's shell notes](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation).

| Function | Arguments | Return value | Description |
| :------- | :-------- | :----------- | :---------- |
| `selectFile(options?)` | `options?: MuonFsOpenDialogOptions` | `Promise<string \| null>` | Shows a dialog for selecting one file. Returns `null` on cancel. |
| `selectFiles(options?)` | `options?: MuonFsOpenDialogOptions` | `Promise<string[]>` | Shows a dialog for selecting multiple files. Returns an empty array on cancel. |
| `selectDirectory(options?)` | `options?: MuonFsOpenDialogOptions` | `Promise<string \| null>` | Shows a dialog for selecting one directory. Returns `null` on cancel. |
| `selectDirectories(options?)` | `options?: MuonFsOpenDialogOptions` | `Promise<string[]>` | Shows a dialog for selecting multiple directories. Returns an empty array on cancel. |
| `selectSaveFile(options?)` | `options?: MuonFsSaveDialogOptions` | `Promise<string \| null>` | Shows a dialog for selecting a save destination file. Returns `null` on cancel. |

Common options:

| Property | Type | Description |
| :------- | :--- | :---------- |
| `signal` | `AbortSignal` | Signal that aborts the dialog operation. |
| `title` | `string` | Dialog title. |
| `defaultPath` | `string` | Initial display path or URI. GTK also accepts GVfs URIs when `gtk.localOnly: false`. |
| `buttonLabel` | `string` | Confirmation button label. |
| `modal` | `boolean` | Whether to disable the calling browser view while the dialog is shown. Defaults to `true`. When `true`, the dialog is aborted if the owner window is closed. |
| `showHidden` | `boolean` | Shows hidden files when the backend supports it. |
| `filters` | `readonly { name: string, extensions: readonly string[] }[]` | File type filters. `extensions` can be specified as `"png"`, `".png"`, `"*.png"`, `"*"`, and similar forms. Each filter requires a non-empty `name` and one or more extensions. |
| `gtk` | `MuonFsGtkDialogOptions` | GTK-specific options. |
| `win32` | `MuonFsWin32DialogOptions` | Win32-specific options. |

`MuonFsSaveDialogOptions` can specify the following in addition to common options.

| Property | Type | Description |
| :------- | :--- | :---------- |
| `defaultName` | `string` | Initial filename shown in the save dialog. |
| `confirmOverwrite` | `boolean` | Confirms before replacing an existing file. Defaults to `true`. |

GTK-specific options:

| Property | Type | Description |
| :------- | :--- | :---------- |
| `localOnly` | `boolean` | Restricts selection to local files only. Defaults to `false`, and URIs from GVfs locations may be returned. |
| `createFolders` | `boolean` | Allows folder creation in supported save and folder selection dialogs. |
| `mimeTypes` | `readonly string[]` | Additional MIME type filters. Each element is a non-empty string. |

Win32-specific options:

| Property | Type | Description |
| :------- | :--- | :---------- |
| `forceFilesystem` | `boolean` | Allows selection only of shell items backed by the file system. |
| `noDereferenceLinks` | `boolean` | Returns shortcut or link items themselves instead of their targets. |
| `dontAddToRecent` | `boolean` | Does not add the selected location to recent documents. |
| `noValidate` | `boolean` | Allows path input that does not pass normal shell validation. |
| `strictFileTypes` | `boolean` | Restricts the entered filename to configured file types. |
| `pathMustExist` | `boolean` | Requires the selected path to exist. |
| `fileMustExist` | `boolean` | Requires the selected file to exist. |

```js
const path = await window.muon.fs.dialogs.selectFile({
  title: "Open image",
  filters: [{ name: "Images", extensions: ["png", "jpg", "jpeg"] }],
});

if (path !== null) {
  const image = await window.muon.fs.readFile(path);
  console.log(image.byteLength);
}
```
