# muon

A multi-platform GUI application framework that uses CEF as its backend.

![muon](./images/muon-120.png)

[![Project Status: WIP – Initial development is in progress, but there has not yet been a stable, usable release suitable for the public.](https://www.repostatus.org/badges/latest/wip.svg)](https://www.repostatus.org/#wip)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

---

## What is this?

Have you ever wanted to update an aging native GUI application into a modern application?
Application replacement is extremely complex, and it has always been a hard problem.

The causes vary widely, so there is no magic solution that makes everything easy.
Even so, everyone wants to modernize with as little cost as possible.

muon (/ˈmjuːɒn/) is a multi-platform GUI application framework that uses CEF (Chromium Embedded Framework).
You have probably heard of similar projects such as [Electron](https://www.electronjs.org/).

Roughly speaking, muon is framework software in the same category.
In other words, it lets you implement local GUI applications as web applications.

With muon, you can build native applications using the most mature and widely used browser UI frameworks, such as React and Vue.

![vscode](./images/vscode.png)

So how is muon different from earlier framework software?

muon specializes in local GUI application development by using a whitelist approach for access to external web resources.
In practice, access is denied by default.

You may think that using CEF without access to websites around the world is a waste.
However, we believe that this is the biggest reason existing solutions have become very complex and difficult to handle.

muon also does not completely forbid access to external resources.
Because the policy is whitelist-based, permitted websites remain accessible.

muon also includes a plugin system with fully asynchronous processing for interoperability with native libraries.
These features are enabled from explicit configuration, so unused capabilities are not left open.

Because muon is based on CEF, Chromium features such as WebGL, WebGPU, WebAssembly, Web workers, and Local storage are available as expected.
DevTools can be shown. CDP (Chrome DevTools Protocol) is also available, so you can debug with VS Code, connect Playwright, or control the app externally.
You can also use remote DevTools from Chromium/Chrome through `chrome://inspect/`.

In short, muon clearly removes one of the major problems that appears when migrating GTK/Qt/Windows native applications to web applications.
This lets you build modern local GUI applications with the web technology ecosystem.

muon also minimizes the steps required to create a muon application, keeping the barrier to entry low.
With the minimal setup, installing the NPM package and adding one line to the configuration file is enough to launch your first muon app.

### Features

- Restricts all network access through a whitelist filter, letting you completely exclude problematic content.
- Provided as an easy-to-use NPM package, so you can turn your web application project into a native GUI application without complex configuration or changes.
- The rendering browser is CEF (Chromium Embedded Framework). From the web application's point of view, this is almost the same as using Chromium or Chrome.
- Supports the Vite plugin system. It also supports Vite HMR, so previews update in real time during development.
- Supports Linux (deb) and Windows (NSIS) package generation, and portable distribution.
- DevTools can be used. CDP (Chrome DevTools Protocol) is also supported, so remote debugging from external tools is possible.
- Includes a plugin system. Plugin capabilities can also be restricted by a whitelist filter.
- Built-in plugins provide access to local files, open dialogs, child process launching, and window operations.

### Environment

- Supports the following architectures from the official CEF binary architectures:
  - Linux: amd64, armhf, arm64
  - Windows: i686, amd64
- Build environment
  - Node.js 20 or later
  - Vite 5 or later

---

## Getting started with muon (Quick version)

Let's get started by creating an application with muon (a muon app).
Use a [Vite template](https://vite.dev/guide/) to create "my-muon-app" as you normally would:

```bash
npm create vite@latest my-muon-app -- --template react-ts
```

This example uses React + TypeScript, but of course other choices are fine. Be sure to enable TypeScript.
Then run:

```bash
cd my-muon-app
npm install
npm run dev
```

This starts the Vite development server, and you can click the link to view the page in a browser.

Now add muon to this project:

```bash
npm install -D muon-ui
```

The CEF binary itself is not included in the NPM package.
CEF is downloaded from the official CDN when it is needed.

Next, add the muon Vite plugin to Vite so that you can build your muon app. Add the following code to `vite.config.ts`:

```ts
import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import muon from 'muon-ui/vite'

export default defineConfig({
  plugins: [
    react(),  // React plugin
    muon(),   // muon plugin (add this)
  ],
})
```

Everything is now ready. Launch muon:

```bash
npm run dev
```

The muon window should appear with your page!

---

## Documents

For more information, [please visit the repository](https://github.com/kekyo/muon-ui/).

## License

Under MIT.
