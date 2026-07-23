/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "browser/muon_browser_background_color.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <gio/gio.h>
#endif

#include <cstdint>
#include <cstring>
#include <optional>

#if !defined(_WIN32)
static constexpr int kMuonPortalSettingsTimeoutMs = 500;
static constexpr char kMuonPortalBusName[] = "org.freedesktop.portal.Desktop";
static constexpr char kMuonPortalObjectPath[] =
    "/org/freedesktop/portal/desktop";
static constexpr char kMuonPortalSettingsInterface[] =
    "org.freedesktop.portal.Settings";
static constexpr char kMuonPortalAppearanceNamespace[] =
    "org.freedesktop.appearance";
static constexpr char kMuonPortalColorSchemeKey[] = "color-scheme";
static constexpr char kMuonGnomeInterfaceSchema[] =
    "org.gnome.desktop.interface";
static constexpr char kMuonGnomeColorSchemeKey[] = "color-scheme";
static constexpr char kMuonGnomePreferDark[] = "prefer-dark";
static constexpr char kMuonGnomePreferLight[] = "prefer-light";
#endif

static MuonResolvedBrowserBackgroundColor CreateResolvedColor(
    uint8_t red,
    uint8_t green,
    uint8_t blue) {
  return {true, CefColorSetARGB(0xff, red, green, blue)};
}

#if !defined(_WIN32)
static std::optional<uint32_t> ReadUint32SettingVariant(GVariant* value) {
  if (value == nullptr) {
    return std::nullopt;
  }

  auto* current = value;
  auto owns_current = false;
  while (g_variant_is_of_type(current, G_VARIANT_TYPE_VARIANT)) {
    auto* unwrapped = g_variant_get_variant(current);
    if (owns_current) {
      g_variant_unref(current);
    }
    current = unwrapped;
    owns_current = true;
  }

  auto result = std::optional<uint32_t>{};
  if (g_variant_is_of_type(current, G_VARIANT_TYPE_UINT32)) {
    result = g_variant_get_uint32(current);
  }
  if (owns_current) {
    g_variant_unref(current);
  }
  return result;
}

static std::optional<uint32_t> ReadPortalColorSchemeWithMethod(
    GDBusProxy* proxy,
    const char* method_name) {
  if (proxy == nullptr) {
    return std::nullopt;
  }

  GError* error = nullptr;
  auto* result = g_dbus_proxy_call_sync(
      proxy, method_name,
      g_variant_new("(ss)", kMuonPortalAppearanceNamespace,
                    kMuonPortalColorSchemeKey),
      G_DBUS_CALL_FLAGS_NONE, kMuonPortalSettingsTimeoutMs, nullptr, &error);
  if (error != nullptr) {
    g_error_free(error);
  }
  if (result == nullptr) {
    return std::nullopt;
  }

  GVariant* raw_value = nullptr;
  if (g_variant_is_of_type(result, G_VARIANT_TYPE("(v)"))) {
    g_variant_get(result, "(v)", &raw_value);
  }
  const auto color_scheme = ReadUint32SettingVariant(raw_value);
  if (raw_value != nullptr) {
    g_variant_unref(raw_value);
  }
  g_variant_unref(result);
  return color_scheme;
}

static MuonSystemColorScheme MapPortalColorScheme(uint32_t color_scheme) {
  switch (color_scheme) {
    case 1:
      return kMuonSystemColorSchemeDark;
    case 2:
      return kMuonSystemColorSchemeLight;
    case 0:
    default:
      return kMuonSystemColorSchemeUnknown;
  }
}

static MuonSystemColorScheme ReadPortalSystemColorScheme() {
  GError* error = nullptr;
  auto* proxy = g_dbus_proxy_new_for_bus_sync(
      G_BUS_TYPE_SESSION,
      static_cast<GDBusProxyFlags>(
          G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES |
          G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS |
          G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START),
      nullptr, kMuonPortalBusName, kMuonPortalObjectPath,
      kMuonPortalSettingsInterface, nullptr, &error);
  if (error != nullptr) {
    g_error_free(error);
  }
  if (proxy == nullptr) {
    return kMuonSystemColorSchemeUnknown;
  }

  g_dbus_proxy_set_default_timeout(proxy, kMuonPortalSettingsTimeoutMs);
  auto color_scheme = ReadPortalColorSchemeWithMethod(proxy, "ReadOne");
  if (!color_scheme.has_value()) {
    color_scheme = ReadPortalColorSchemeWithMethod(proxy, "Read");
  }
  g_object_unref(proxy);

  if (!color_scheme.has_value()) {
    return kMuonSystemColorSchemeUnknown;
  }
  return MapPortalColorScheme(color_scheme.value());
}

static bool HasGSettingsStringKey(const char* schema_id, const char* key) {
  auto* source = g_settings_schema_source_get_default();
  if (source == nullptr) {
    return false;
  }
  auto* schema = g_settings_schema_source_lookup(source, schema_id, TRUE);
  if (schema == nullptr) {
    return false;
  }
  const auto has_key = g_settings_schema_has_key(schema, key);
  g_settings_schema_unref(schema);
  return has_key;
}

static MuonSystemColorScheme ReadGSettingsSystemColorScheme() {
  if (!HasGSettingsStringKey(kMuonGnomeInterfaceSchema,
                             kMuonGnomeColorSchemeKey)) {
    return kMuonSystemColorSchemeUnknown;
  }

  auto* settings = g_settings_new(kMuonGnomeInterfaceSchema);
  if (settings == nullptr) {
    return kMuonSystemColorSchemeUnknown;
  }
  auto* color_scheme = g_settings_get_string(settings,
                                             kMuonGnomeColorSchemeKey);
  auto result = kMuonSystemColorSchemeUnknown;
  if (color_scheme != nullptr) {
    if (std::strcmp(color_scheme, kMuonGnomePreferDark) == 0) {
      result = kMuonSystemColorSchemeDark;
    } else if (std::strcmp(color_scheme, kMuonGnomePreferLight) == 0) {
      result = kMuonSystemColorSchemeLight;
    }
    g_free(color_scheme);
  }
  g_object_unref(settings);
  return result;
}
#endif

static MuonSystemColorScheme ReadMuonSystemColorScheme() {
#if defined(_WIN32)
  DWORD apps_use_light_theme = 0;
  DWORD value_type = REG_DWORD;
  DWORD value_size = sizeof(apps_use_light_theme);
  const auto status = RegGetValueW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, &value_type,
      &apps_use_light_theme, &value_size);
  if (status != ERROR_SUCCESS || value_type != REG_DWORD ||
      value_size < sizeof(apps_use_light_theme)) {
    return kMuonSystemColorSchemeUnknown;
  }
  return apps_use_light_theme == 0 ? kMuonSystemColorSchemeDark
                                   : kMuonSystemColorSchemeLight;
#else
  const auto portal_color_scheme = ReadPortalSystemColorScheme();
  if (portal_color_scheme != kMuonSystemColorSchemeUnknown) {
    return portal_color_scheme;
  }
  return ReadGSettingsSystemColorScheme();
#endif
}

MuonResolvedBrowserBackgroundColor ResolveMuonBrowserBackgroundColorForSystemScheme(
    const MuonBrowserBackgroundColorConfig& background_color,
    MuonSystemColorScheme system_color_scheme) {
  if (background_color.mode == kMuonBrowserBackgroundColorRgb) {
    return CreateResolvedColor(background_color.red, background_color.green,
                               background_color.blue);
  }

  switch (system_color_scheme) {
    case kMuonSystemColorSchemeDark:
      return CreateResolvedColor(0, 0, 0);
    case kMuonSystemColorSchemeLight:
      return CreateResolvedColor(0xff, 0xff, 0xff);
    case kMuonSystemColorSchemeUnknown:
      return {};
  }
  return {};
}

MuonResolvedBrowserBackgroundColor ResolveMuonBrowserBackgroundColor(
    const MuonBrowserBackgroundColorConfig& background_color) {
  if (background_color.mode == kMuonBrowserBackgroundColorRgb) {
    return ResolveMuonBrowserBackgroundColorForSystemScheme(
        background_color, kMuonSystemColorSchemeUnknown);
  }
  return ResolveMuonBrowserBackgroundColorForSystemScheme(
      background_color, ReadMuonSystemColorScheme());
}

void ApplyMuonBrowserBackgroundColor(
    CefBrowserSettings& settings,
    const MuonBrowserBackgroundColorConfig& background_color) {
  const auto resolved_color =
      ResolveMuonBrowserBackgroundColor(background_color);
  if (resolved_color.has_color) {
    settings.background_color = resolved_color.color;
  }
}
