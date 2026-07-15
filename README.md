# muon

A multi-platform GUI application framework that uses CEF as its backend.

![muon](./images/muon-120.png)

[![Project Status: WIP – Initial development is in progress, but there has not yet been a stable, usable release suitable for the public.](https://www.repostatus.org/badges/latest/wip.svg)](https://www.repostatus.org/#wip)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![npm version](https://img.shields.io/npm/v/muon-ui.svg)](https://www.npmjs.com/package/muon-ui)

---

[(For Japanese language/日本語はこちら)](./README_ja.md)

> Please note that this English version of the document was machine-translated and then partially edited, so it may contain inaccuracies.
> We welcome pull requests to correct any errors in the text.

## What is this?

Have you ever wanted to update an aging native GUI application into a modern application?
Application replacement is extremely complex, and it has always been a hard problem.

The causes vary widely, so there is no magic solution that makes everything easy.
Even so, everyone wants to modernize with as little cost as possible.

muon (pronounced "muon") is a multi-platform GUI application framework that uses CEF (Chromium Embedded Framework).
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

## Getting started with muon

Let's create an application with muon, a muon app.
It is very easy not only to create a new application, but also to turn an existing web application into a muon app.

To show this, start by preparing a very ordinary web application project.
For example, use a [Vite template](https://vite.dev/guide/) to create "my-muon-app":

```bash
npm create vite@latest my-muon-app -- --template react-ts
```

This example uses React + TypeScript, but of course other choices are fine.
Using TypeScript is recommended. The reason is explained later.

Install the required packages and run the application with the following commands:

```bash
cd my-muon-app
npm install
npm run dev
```

The Vite development server starts, and you can click the link to show the page in a browser.
muon has not been introduced yet. This is still a plain Vite project.

Now add muon to this project. The required work is only:

1. Install the muon package.
2. Configure the muon Vite plugin.

Both steps are very simple.

### Install the muon package

Install the muon package, officially named `muon-ui`, into `devDependencies`:

```bash
npm install -D muon-ui
```

The muon package includes the following:

- muon CLI
- muon Vite plugin
- TypeScript type definitions for muon built-in plugins
- Platform-specific muon binary assets

The CEF binary itself is not included in the NPM package.
CEF is downloaded from the official CDN when it becomes necessary.

### Configure the muon Vite plugin

Installing the muon package almost completes the preparation, but you should enable the muon Vite plugin so that HMR (Hot Module Replacement) and muon app builds are available.

For anyone unfamiliar with HMR, Vite acts as a development server, lets a browser show the page, and updates the display almost in real time when the page is edited.
In other words, it is a very useful feature that automatically previews edit results.

The muon Vite plugin supports HMR by launching muon instead of a browser and making HMR work inside the muon app.
Add the following code to `vite.config.ts` to enable it:

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

Add `muon()` to the `plugins` array in the `defineConfig()` argument.
This enables the muon Vite plugin. Preparation is now complete.

Launch muon:

```bash
npm run dev
```

The muon window and your page should appear.

![Get started](./images/get-started.png)

Try changing the page source code and confirm that HMR works just as well as it did in the browser.
For example, change the `<h1>Get started</h1>` line in `src/App.tsx` to `<h1>Get started with muon!</h1>` and save it.
The display in the muon window should update immediately without restarting.

Clicking the "Count is 0" counter button in the center of the page should increase the count.
This confirms that React from the Vite template is working correctly.

If you click a button such as "Explore Vite", a new window opens and shows the following disappointing page:

![Forbidden](./images/forbidden.png)

This is exactly the evidence that muon's network-access whitelist filter is working.
The button tries to show the official Vite site (`https://vite.dev/`), but by default muon only permits access to local assets.
Content from other sites is blocked.
The way to configure this whitelist is explained in detail in another chapter.

You can also launch muon DevTools with the `F12` key:

![muon DevTools](./images/devtools.png)

CDP (Chrome DevTools Protocol) is also enabled, so you can control the app with Playwright or debug it with VS Code. See the relevant chapter for details.

You can recycle-restart muon with `Ctrl+F12`.
Recycle restart is useful when you changed something that HMR cannot apply, such as `muon.json`.

### Packaging

Packaging a muon app is also simple:

```bash
npx muon pack
```

This generates Linux and Windows packages in deb and nsis formats under the `artifacts/` directory.
For more details, see the following documents.

---

## Documentation

See the following documents for more detailed information about muon.

### User guides

- [Application distribution](./docs/en/deployment.md)
- [CEF download and update](./docs/en/cef-download-and-update.md)
- [muon DevTools](./docs/en/muon-devtools.md)
- [Local asset configuration](./docs/en/local-assets.md)
- [Using muon plugins](./docs/en/muon-plugins.md)
- [Accessing external networks](./docs/en/external-network.md)

### Advanced topics

- [muon CLI](./docs/en/muon-cli-advanced-topics.md)
- [Package installation locations](./docs/en/package-install-location-advanced-topics.md)
- [CEF versions and CEF API versions](./docs/en/cef-version-and-cef-api-version-advanced-topics.md)
- [CEF binary update details](./docs/en/cef-binary-update-details-advanced-topics.md)
- [Window communication limitations](./docs/en/window-communication-limitations-advanced-topics.md)
- [Local asset permissions](./docs/en/local-asset-permissions-advanced-topics.md)
- [Local asset packing](./docs/en/local-asset-packing-advanced-topics.md)
- [Origin-based permissions](./docs/en/origin-based-permissions-advanced-topics.md)

### References

- [muon.json reference](./docs/en/muon-json-reference.md)
- [muon Vite plugin reference](./docs/en/muon-vite-plugin-reference.md)
- [muon built-in plugin reference](./docs/en/muon-built-in-plugin-reference.md)

### Implementing muon plugins

TODO:

### Other

- [muon-ui self-build](./docs/en/muon-ui-self-build-advanced-topic.md)
- [Limitation](./docs/en/limitation.md)

---

## License

Under MIT.
