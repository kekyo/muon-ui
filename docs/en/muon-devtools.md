# muon DevTools

muon can show muon DevTools.
It provides the same capabilities as Chromium or Chrome DevTools, including ad-hoc debugging, performance measurement, and diagnostics.

When launched by Vite with `vite dev`, the `F12` muon DevTools keybind and the `Ctrl+F12` recycle keybind are automatically enabled.
After a distribution build, muon apps cannot open muon DevTools by default.

Add the following definition explicitly to `muon.json` to show muon DevTools in a built muon app:

```json
{
  "browser": {
    "keybind": {
      "devtools": "f12"
    },
  }
}
```

This example assigns the `F12` key to show DevTools.
You can specify other keys or combinations such as `shift+f12`.

DevTools can also be shown in other ways.
With the remote debugging feature called CDP (Chrome DevTools Protocol), you can show DevTools remotely from Chromium or Chrome.

This feature is also disabled by default.
Add the following definition to `muon.json`:

```json
{
  "cdp": {
    "enable": true
  },
}
```

After launching the muon app with this configuration, open `chrome://inspect/` in Chromium or Chrome.

![chrome://inspect/](../../images/inspect.png)

Click the `inspect` link for the muon instance shown under "Remote Target" to connect to that muon instance with CDP and show remote DevTools.

Because muon can show DevTools on its own, using CDP only to show remote DevTools is usually unnecessary.
However, once CDP is available, other debuggers can also be used.

For example, add the following definition to VS Code's `.vscode/launch.json` to start a debugging session in VS Code:

```json
{
  "version": "0.2.0",
  "configurations": [
    {
      "type": "chrome",
      "request": "attach",
      "name": "Attach to muon",
      "port": 9222,
      "webRoot": "${workspaceFolder}"
    }
  ]
}
```

Set `port` to the same value as `cdp.port` in `muon.json`.
The default value is `9222`.
