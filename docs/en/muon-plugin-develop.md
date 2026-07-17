# Developing muon plugins

Welcome to the world of muon plugins.

A muon plugin is a gateway that lets JavaScript in CEF access any capability in the native world.
By developing a plugin, you can perform literally any operating system operation.
For example, by invoking Linux system calls, third-party libraries, or Windows APIs,
you can control powerful capabilities from a page that CEF and muon's built-in plugins cannot provide.

However.

Stop here and think again. The road ahead can be difficult.

- A muon plugin must be implemented in C or C++.
- A crash can terminate the entire muon app process.
- If a capability can be implemented with muon's built-in plugins, there is no need to implement a muon plugin yourself.
  In particular, if you are interested in this development, you should review the capabilities in the `muon.executor` namespace first.

## Overview

A muon plugin runs through the following phases:

### Plugin initialization and wiring

According to `muon.json`, the `.so`/`.dll` library that implements the muon plugin is loaded and initialized so that the muon app can reference it.

- The plugin provides an entry point named `muon_init_plugin()`.
  The muon app calls it to initiate initialization and exposes a set of JavaScript functions based on the returned information.
- At this point, the muon plugin is loaded into the CEF process known as the "browser process."
  CEF also maintains a separate "renderer process" for each browser window, while the browser process is a management process isolated from them.
  Therefore, a muon plugin does not interact directly with each renderer process.
- Based on the plugin function entry-point metadata returned by the plugin, muon performs the "wiring" that establishes IPC with the JavaScript world.
  This prepares JavaScript function calls as IPC requests to the browser process and allows the two sides to communicate seamlessly.
- Finally, the plugin inserts custom JavaScript source code if necessary.
  Because the wiring procedure above restricts how arguments and return values can be represented, you can insert JavaScript conversion code to provide definitions that are easier to use from JavaScript.
  This is optional, so JavaScript source insertion is unnecessary when the standard wiring rules are sufficient to implement the capability.

### Calling plugin functions from a muon app

When JavaScript in a muon app calls an exposed function, IPC forwards the request from the renderer process to the browser process.
Because this IPC acts as a type of RPC (remote procedure call), JavaScript sees what appears to be a simple call to a plugin function.

The request forwarded to the browser process invokes the plugin's concrete function, implemented in C or C++, giving your native code an opportunity to run.
However, just as plugin functions are expected to always return a `Promise`, native functions must also be designed for asynchronous processing.

When implementing a plugin in C or C++, no standard continuation mechanism corresponds to a `Promise`, so function signatures must follow the rules below.
For example, assume the function appears as follows from JavaScript (written in TypeScript for clarity):

```typescript
// Signature of a function exposed by the plugin
function plugin_add(a: number, b: number) : Promise<number> { ... };
```

The corresponding C (C++) code is as follows:

```cpp
#include "muon_plugin_api.h"

// C++ code in the plugin
extern "C" void plugin_add(muon_completion_func comp, int32_t a, int32_t b) {
  const auto result = a + b
  // Call `comp` instead of returning a value
  comp(&result, nullptr);
}
```

`comp` is the completion continuation function, and calling it returns the result.
Although this form may look unusual, it has the advantage that both immediate and delayed results can be returned in the same way.

If a function takes a long time to run and therefore needs to be handled asynchronously, save `comp` and call it at any point after the operation completes to report completion asynchronously.
You can think of it simply as a callback function that reports the result.

### Asynchronous processing support

In this way, all plugin functions natively support asynchronous processing.
Conversely, plugin functions must **not** block for an extended period.
This is because plugin functions run in an important CEF thread context, and blocking that thread affects CEF operation.

CEF is an embeddable framework based on Chromium, and end users therefore expect a browser experience as smooth as Chromium/Chrome.
However, blocking the thread inside a plugin function can directly affect the behavior of CEF browser windows.

If an I/O operation introduces a delay, you should use asynchronous I/O.
If CPU-bound computation continues for a long time, it must be offloaded to a worker thread, which calls `comp` when the computation finishes.

Although it is technically possible to implement all of this in C, muon uses [`cardio`](https://github.com/kekyo/cardio/) internally,
so using `cardio` in your plugin project can significantly simplify the implementation.
Because `cardio` supports `co_await` and `co_return` with C++20 or later, you can implement readable code without callbacks.
You may, of course, use another library that supports asynchronous processing.

For muon plugin implementation examples, see the [`muon-core/test_plugins/`](../../muon-core/test_plugins/) directory.
These are test implementations for `muon-core`, but their small size and focus on individual test cases make the minimal implementation easy to understand.

### Signature metadata

Plugin functions are wired by using [`tra-ffic`](https://github.com/kekyo/tra-ffic/).
The current version of `tra-ffic` supports only a limited set of type definitions, primarily primitive, string, and function types, as well as nested function signatures.
These types are not available as runtime information in either the JavaScript runtime or native C/C++ code.
Therefore, the developer must supply this metadata from the plugin so that wiring can succeed and IPC can be established.

The following example defines metadata for `plugin_add()` from the previous section:

```cpp
// Definition of the i32 (int32_t) type
static const muon_type_descriptor type_i32 = {
  MUON_TYPE_I32,
  nullptr,
};

// Definition of the arguments
static const muon_type_descriptor plugin_add_args[] = {
  type_i32,   // Type of argument a
  type_i32,   // Type of argument b
};

// Definition of exposed functions
static const muon_plugin_function_metadata plugin_function = {
  "plugin_add",  // Function name
  reinterpret_cast<muon_native_function>(&plugin_add),  // Function entry point
  {2, plugin_add_args, &type_i32},  // Function signature (argument count, argument definitions, and return type)
  nullptr
};
```

In this way, you must define the argument and return types of each plugin function in C/C++ code.
These definitions are then collected into a list of all functions in a namespace and ultimately returned from `muon_init_plugin()`.

The final implementation of `muon_init_plugin()` is as follows:

```cpp
// List of functions in the plugin namespace
static const muon_plugin_function_metadata* const plugin_functions_pointers[] = {
  &plugin_function,
  nullptr
};

// Definition of the plugin namespace
static const muon_plugin_namespace plugin_namespace = {
  "foobar.baz",               // Namespace name
  nullptr,
  plugin_functions_pointers   // Functions in this plugin namespace
};

// List of namespaces (one plugin can support multiple namespaces)
static const muon_plugin_namespace* const plugin_namespaces_pointers[] = {
  &plugin_namespaces,
  nullptr,
};

// Definition of plugin metadata
static const muon_plugin_metadata plugin_metadata = {
  plugin_namespaces_pointers,
};

// Initialization function
extern "C" const muon_plugin_metadata* muon_init_plugin(
  const muon_plugin_init_context* context) {

  // Example: reference the value of a config key from muon.json
  const auto* value = muon_plugin_get_config_value(context, "foobar.startAt");

  // Return the metadata
  return &plugin_metadata;
}
```
