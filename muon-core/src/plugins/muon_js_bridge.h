/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

/**
 * Process message name used for plugin function calls.
 */
inline constexpr char kMuonPluginCallMessageName[] = "muon.plugin.call";

/**
 * Shared-memory companion message used for plugin function call buffers.
 */
inline constexpr char kMuonPluginCallSharedMessageName[] =
    "muon.plugin.call.shared";

/**
 * Process message name used for plugin function results.
 */
inline constexpr char kMuonPluginResultMessageName[] = "muon.plugin.result";

/**
 * Shared-memory companion message used for plugin function result buffers.
 */
inline constexpr char kMuonPluginResultSharedMessageName[] =
    "muon.plugin.result.shared";

/**
 * Process message name used for plugin-owned function proxy calls.
 */
inline constexpr char kMuonPluginProxyCallMessageName[] =
    "muon.plugin.proxy.call";

/**
 * Shared-memory companion message used for plugin proxy call buffers.
 */
inline constexpr char kMuonPluginProxyCallSharedMessageName[] =
    "muon.plugin.proxy.call.shared";

/**
 * Process message name used when a plugin calls a renderer-owned function.
 */
inline constexpr char kMuonRendererFunctionCallMessageName[] =
    "muon.renderer.function.call";

/**
 * Shared-memory companion message used for renderer function call buffers.
 */
inline constexpr char kMuonRendererFunctionCallSharedMessageName[] =
    "muon.renderer.function.call.shared";

/**
 * Process message name used for renderer-owned function call results.
 */
inline constexpr char kMuonRendererFunctionResultMessageName[] =
    "muon.renderer.function.result";

/**
 * Shared-memory companion message used for renderer function result buffers.
 */
inline constexpr char kMuonRendererFunctionResultSharedMessageName[] =
    "muon.renderer.function.result.shared";

/**
 * Process message name used when a renderer V8 context releases function ids.
 */
inline constexpr char kMuonFunctionContextReleasedMessageName[] =
    "muon.function.context.released";
