/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once
#include <quickjs.h>

/**
 * Exposes a host-provided object as an ES module in the current engine.
 * @param context Owner context, inside a protected engine execution turn.
 * @param name Resolved module identity supplied to the engine's module loader.
 * @param object Owned module object, consumed on success and failure.
 * @returns Engine-owned module, or null with a JavaScript exception pending.
 * @remarks Enumerable string properties become named exports. An undefined
 * default export falls back to the whole object. The engine retains the object
 * until module disposal; no filesystem or shared-library loading occurs here.
 */
JSModuleDef* CreateMuonQuickJsObjectModule(JSContext* context, const char* name, JSValue object);
