/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "muon_plugin_api.h"

#include <string>

/**
 * Internal options key used to associate a modal dialog with its opener.
 */
inline constexpr char kMuonFsDialogsOwnerBrowserIdOption[] =
    "__muonOwnerBrowserId";

/**
 * Internal options key used to associate a modal dialog with a native owner
 * window handle.
 */
inline constexpr char kMuonFsDialogsOwnerWindowHandleOption[] =
    "__muonOwnerWindowHandle";

/**
 * Optional internal symbol exported by the built-in filesystem dialogs plugin.
 */
inline constexpr char kMuonFsDialogsCancelOwnerBrowserSymbol[] =
    "muon_builtin_fs_dialogs_cancel_owner_browser";

/**
 * Cancels active modal filesystem dialogs opened by the given browser.
 */
using MuonFsDialogsCancelOwnerBrowserFunction = void (*)(int owner_browser_id);

/**
 * Initializes the filesystem dialogs plugin runtime.
 *
 * @param helpers Host helper table used by dialog cancellation functions.
 * @param error_message Receives an initialization diagnostic.
 * @return true when the runtime is ready.
 */
bool InitializeMuonBuiltinFsDialogs(const muon_plugin_helpers* helpers,
                                    std::string* error_message);

/**
 * Shuts down the filesystem dialogs plugin runtime.
 */
void ShutdownMuonBuiltinFsDialogs();

/**
 * Cancels active modal filesystem dialogs for a browser owner.
 *
 * @param owner_browser_id CEF browser identifier for the opener window.
 */
extern "C" void muon_builtin_fs_dialogs_cancel_owner_browser(
    int owner_browser_id);

/**
 * JavaScript-visible namespace metadata for filesystem dialogs.
 */
extern const muon_plugin_namespace kMuonBuiltinFsDialogsNamespace;

/**
 * Returns metadata for JavaScript-visible filesystem dialog functions.
 *
 * @return Filesystem dialog plugin metadata.
 */
const muon_plugin_metadata* GetMuonBuiltinFsDialogsPluginMetadata();
