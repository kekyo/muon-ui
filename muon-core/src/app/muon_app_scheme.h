/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include "app/muon_app_storage.h"

#include "include/cef_scheme.h"

#include <memory>

namespace cardio {
class dispatcher;
}

/**
 * Custom scheme name for muon app assets.
 */
inline constexpr char kMuonAppSchemeName[] = "asset";

/**
 * Default app origin host.
 */
inline constexpr char kMuonAppMainDomain[] = "main";

/**
 * Initial URL loaded by the main browser.
 */
inline constexpr char kMuonAppStartUrl[] = "asset://main/index.html";

/**
 * Returns CEF scheme options for the asset:// scheme.
 *
 * @remarks The asset scheme is a standard, secure, display-isolated origin
 * that participates in CORS and Fetch. It is intentionally not a local scheme.
 */
inline int GetMuonAppSchemeOptions() {
  return CEF_SCHEME_OPTION_STANDARD | CEF_SCHEME_OPTION_SECURE |
         CEF_SCHEME_OPTION_CORS_ENABLED | CEF_SCHEME_OPTION_FETCH_ENABLED |
         CEF_SCHEME_OPTION_DISPLAY_ISOLATED;
}

/**
 * Maps an app storage result status to an HTTP status code.
 *
 * @param status Storage lookup status.
 * @return HTTP response status for the asset scheme handler.
 */
int GetMuonAppStorageHttpStatus(MuonAppStorageStatus status);

/**
 * Registers the asset:// custom scheme with CEF.
 *
 * @param registrar CEF scheme registrar supplied by OnRegisterCustomSchemes.
 */
void RegisterMuonAppCustomScheme(CefRawPtr<CefSchemeRegistrar> registrar);

/**
 * Creates a CEF scheme handler factory for asset:// resources.
 *
 * @param storage Storage backend used to resolve app resources.
 * @return Scheme handler factory for the asset://main origin.
 */
CefRefPtr<CefSchemeHandlerFactory> CreateMuonAppSchemeHandlerFactory(
    std::shared_ptr<MuonAppStorage> storage,
    cardio::dispatcher* dispatcher = nullptr);
