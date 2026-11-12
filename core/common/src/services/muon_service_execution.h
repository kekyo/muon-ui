/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once
#include "services/muon_service_interface.h"
#include <functional>
#include <optional>

/** Copied invocation result; success has a value, failure has a nonempty code. */
struct MuonServiceResult {
  std::optional<MuonServiceValue> value; /**< Typed success value, including void. */
  std::string code; /**< Empty on success; common or declared application code. */
  std::string message; /**< Copied UTF-8 diagnostic on failure. */
};

/** One terminal result, delivered on the owner dispatcher. */
using MuonServiceCallCompletion = std::function<void(MuonServiceResult)>;
/**
 * Dispatches an invocation-owned callback with copied arguments.
 * The completion may outlive cancellation; late completions are ignored.
 */
using MuonServiceCallbackHandler = std::function<void(uint64_t,
    const MuonServiceSignature&, std::vector<MuonServiceValue>, MuonServiceCallCompletion)>;
/** Receives a copied event scoped to one logical service generation. */
using MuonServiceEventSink = std::function<void(const std::string&, uint64_t,
    const std::string&, const MuonServiceValue&)>;

/**
 * Borrowed execution operations for native, QuickJS, or process adapters.
 * Every operation runs on the same owner dispatcher as the broker. The owning
 * runtime must outlive this table and finish its deferred callbacks before
 * destruction. Calls never create independent service lifetime references.
 */
struct MuonServiceExecution {
  /** Returns validated metadata for an exact running generation. */
  std::function<const MuonServiceInterface&(const std::string&, uint64_t)> interface;
  /** Returns portable interface JSON for that generation. */
  std::function<std::string(const std::string&, uint64_t)> interface_json;
  /** Starts a typed call; returns a nonzero identity and completes once later. */
  std::function<uint64_t(const std::string&, uint64_t, const std::string&,
      std::vector<MuonServiceValue>, MuonServiceCallbackHandler,
      MuonServiceCallCompletion)> invoke;
  /** Cancels one invocation without affecting other calls or generations. */
  std::function<void(const std::string&, uint64_t, uint64_t)> cancel;
  /** Installs one generation-scoped event sink; empty detaches it. */
  std::function<void(MuonServiceEventSink)> set_event_sink;
};
