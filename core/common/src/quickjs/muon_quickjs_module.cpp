/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "quickjs/muon_quickjs_module.h"
#include <cstring>

static int Exports(JSContext* context, JSModuleDef* definition, JSValueConst object, bool initialize) {
  JSPropertyEnum* properties = nullptr;
  uint32_t count = 0;
  if (JS_GetOwnPropertyNames(context, &properties, &count, object,
      JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) < 0) return -1;
  int result = 0;
  for (uint32_t index = 0; index < count; ++index) {
    const auto atom = properties[index].atom;
    const auto name = JS_AtomToCString(context, atom);
    if (!name) { result = -1; break; }
    if (std::strcmp(name, "default") != 0) {
      if (initialize) {
        const auto value = JS_GetProperty(context, object, atom);
        result = JS_IsException(value) ? -1 : JS_SetModuleExport(context, definition, name, value);
      } else result = JS_AddModuleExport(context, definition, name);
    }
    JS_FreeCString(context, name);
    if (result < 0) break;
  }
  JS_FreePropertyEnum(context, properties, count);
  return result;
}

static int Initialize(JSContext* context, JSModuleDef* definition) {
  // The loader retains the object until QuickJS instantiates the synthetic
  // module, at which point its properties can become ESM exports.
  const auto object = JS_GetModulePrivateValue(context, definition);
  if (JS_IsException(object)) return -1;
  auto value = JS_GetPropertyStr(context, object, "default");
  if (JS_IsException(value)) { JS_FreeValue(context, object); return -1; }
  if (JS_IsUndefined(value)) {
    JS_FreeValue(context, value);
    value = JS_DupValue(context, object);
  }
  auto result = JS_SetModuleExport(context, definition, "default", value);
  if (result >= 0) result = Exports(context, definition, object, true);
  JS_FreeValue(context, object);
  return result;
}

JSModuleDef* CreateMuonQuickJsObjectModule(JSContext* context, const char* name, JSValue object) {
  if (JS_IsException(object)) return nullptr;
  if (!JS_IsObject(object)) {
    JS_FreeValue(context, object);
    JS_ThrowTypeError(context, "Host module '%s' is not an object", name);
    return nullptr;
  }
  const auto definition = JS_NewCModule(context, name, Initialize);
  if (!definition || JS_AddModuleExport(context, definition, "default") < 0 ||
      Exports(context, definition, object, false) < 0) {
    JS_FreeValue(context, object);
    return nullptr;
  }
  if (JS_SetModulePrivateValue(context, definition, object) < 0) return nullptr;
  return definition;
}
