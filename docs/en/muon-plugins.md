# Using muon plugins

So far, this documentation has explained the development lifecycle for building a muon app.
However, a few capabilities are still missing for a muon app to behave like a native application.

For example, fine-grained control of the muon window itself, local file reads and writes, process environment inspection, and process launching are not exposed in the standard browser JavaScript environment.
muon provides an extensible plugin structure for accessing these native capabilities.
This is called a "muon plugin".

> Note: This is not the muon Vite plugin.

The basic muon plugin capabilities are provided by muon's built-in plugins.
You can also implement the muon plugin API and load it into muon to extend functionality in the same way as the built-in plugins.

The following example launches a child process by using a muon built-in plugin.

```ts
// Reference spawn in the muon.executor namespace.
import { spawn } from "muon:executor";

// Use the spawn function.
const child = await spawn({
  command: "ps",
  args: ["xw"],
});
const result = await child.wait();
```

You may think, "That is easy, let's try it right away!", but this does not work as-is.
The reason is that all muon plugins are controlled by whitelist-based filters.

## Access permissions for muon plugin functions

First, specify which JavaScript code can import which plugin functions through `"plugin"` in `muon.json`.
The muon Vite plugin reads this setting and permits access to muon plugins:

```json
{
  "plugin": {
    "mode": "validate",
    "pages": ["asset://main/**"],
    "plugins": [
      {
        "name": "internal",
        "imports": [
          {
            "sources": ["src/native/**"],
            "allow": ["muon.executor.spawn"]
          }
        ],
        "config": {
          "foobar.settings": "John Doe"
        }
      }
    ]
  }
}
```

- `mode` determines the plugin access method. It can be omitted, and the default is `"validate"`.
- `pages` is the list of page URLs that can access muon plugins.
  By default, access is allowed only for `asset://main/**`.
  Pages outside this list cannot access muon plugin functions.
- `name` is the plugin name. Only built-in plugins use the special value `"internal"`.
  Other plugins load plugin files (`*.so` or `*.dll`) placed in the `plugins/` directory, and `name` is the filename without its extension.
  `spawn()` is a function from a muon built-in plugin placed in the `muon.executor` namespace.
  To make it callable, specify `"internal"` for `name` and `"muon.executor.spawn"` for `imports[].allow`.
- `config` is a string key-value setting passed to that plugin during initialization.
  Both keys and values are strings. Objects, arrays, numbers, booleans, and null cannot be used as values.
  When omitted, it is treated as an empty setting. The meaning of each value depends on the plugin.
- `imports` allows imports, meaning references through TypeScript/JavaScript `import`, of the muon plugin functions specified in `allow` only from source files that match paths specified in `sources`.
  `allow` can contain complete function paths such as `muon.executor.spawn`, or globs such as `muon.executor.*` and `muon.**`.

Next, make the type definitions visible to the TypeScript compiler.
In `tsconfig.json`, explicitly add muon's type definitions to `compilerOptions.types`:

```json
{
  "compilerOptions": {
    "types": [
      "vite/client",   // Vite type definitions
      "muon-ui"        // Type definitions for muon built-in plugins (add this)
    ]
  }
}
```

You can now implement code that uses muon's built-in plugins.

Referencing muon plugins this way is called `validate` mode.
There is also a `simple` mode:

```json
{
  "plugin": {
    "mode": "simple",
    "pages": ["asset://main/**"],
    "plugins": [
      {
        "name": "internal",
        "allow": ["muon.executor.spawn"]
      }
    ]
  }
}
```

In `simple` mode, you can access muon plugin functions by traversing namespace objects from the `window` object:

```javascript
// Launch a child process (simple mode).
var child = await window.muon.executor.spawn({
  command: "ps",
  args: ["xw"],
});
var result = await child.wait();
```

This method does not require applying the build process from the muon Vite plugin.
You can open muon DevTools and try it directly in the console:

![muon API](../../images/muon-api.png)

| Mode | Vite plugin | Page filter | Access-source filter | Details |
| :--- | :---------- | :---------- | :------------------- | :------ |
| `validate` | Required | Available | Available | This is the default. This mode is used when omitted. It can filter code that imports muon plugin functions by explicitly specified source files or NPM packages. |
| `simple` | Not required | Available | Not available | This mode directly inserts muon plugin namespace objects and functions under the `window` object. It is simple, but unlike `validate` mode, it cannot filter access by referencing source code. |

> Note: muon strongly recommends using `validate` mode.
> The reason is that `validate` mode can restrict access to muon plugin functions with whitelist filters.
> For example, you can restrict muon plugin function calls to only your `myfoobar.ts` file and the `foobar` NPM package.
> This makes it harder for other JavaScript code to call muon plugin functions.
> As a result, resistance to NPM supply-chain attacks is improved.

## Reading settings from muon.json

Plugins can read `plugins[].config` from `muon.json` through the `muon_plugin_init_context` passed to `muon_init_plugin()`.
`plugin_name` and `config_entries` are valid only during the `muon_init_plugin()` call.
Values that are needed after initialization must be copied by the plugin.
The helper table can still be referenced from `context->helpers` as before, and plugins may keep it.

```cpp
extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  const char* mode = muon_plugin_get_config_value(context, "foobar.mode");
  if (mode != NULL && strcmp(mode, "strict") == 0) {
    /* enable strict behavior */
  }
  return &metadata;
}
```

muon only passes `config` as an array of strings and does not interpret the contents of values.
If newline-separated values, globs, regular expressions, comma-separated values, or other formats are needed, define and implement them as part of each plugin's specification.

## Signatures of muon plugin functions

Note that all functions exposed by muon plugins are defined as functions that return `Promise`.
In general, remember to use `await` to get the result of an asynchronous function that returns a `Promise`.

APIs provided by muon's built-in plugins have TypeScript type definitions in `muon.d.ts`.
These are available once the muon plugin is installed.
When writing code in TypeScript, you can get type checking for virtual module imports such as `muon:executor` and for the `window.muon` hierarchy:

![intellisense](../../images/intellisense.png)

> Note: Only functions specified in the whitelist from the previous section can be used.
> However, the type definitions in `muon.d.ts` assume those APIs exist.
> If a function is missing from the whitelist, the failure occurs at runtime.

### Releasing plugin function proxies

When a native plugin function returns a function object, JavaScript receives it as `MuonPluginFunctionProxy<TArgs, TResult>`.
Like normal plugin functions, calling a proxy always returns a `Promise`.

After finishing with the proxy, call `release()`:

```typescript
// A native plugin function returned a function object.
const proxy = ...;

try {
  const result = await proxy("value");
  console.log(result);
} finally {
  // Release the function object that is no longer needed.
  proxy.release();
}
```

The proxy implements `Releaseable`, and `release()` and `[Symbol.dispose]()` perform the same release operation.

Calling the proxy after `release()` returns a rejected `Promise`.
Passing a released proxy as an argument to another plugin function is an argument validation error.
GC finalizers and V8 context cleanup also help with releasing, but both are fallbacks.
Do not depend on them as replacements for deterministic release.

Proxies obtained from different native return values are not guaranteed to be identical JavaScript objects, even when they represent the same native function.

On the other hand, when passing a normal JavaScript callback from the renderer to a plugin as an argument, `release()` and `[Symbol.dispose]()` are not added to that callback.
Functions returned by `MuonAdhocLibrary.getFunction()` are also different from this proxy, and the library itself is released with `MuonAdhocLibrary.release()`.
