/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once
#include "services/muon_service_plugin_pool.h"
#include <deque>
#include <atomic>
#include <map>
#include <mutex>

// Private, JS-free lifetime state. Retained after a failed heap is destroyed.
struct MuonServicePluginLeaseImpl {
  std::shared_ptr<MuonServicePluginPoolImpl> pool;
  MuonRpcOwner owner;
  std::string id;
  uint64_t generation = 0;
  std::shared_ptr<MuonPluginPolicy> policy;
  bool attached = false;
  std::atomic<bool> closing{false};
  bool closed = false;
  bool drained = false;
  size_t calls = 0;
  std::function<void()> detach;
  std::function<void(const MuonRpcMessage&)> receive;
  std::vector<std::function<void()>> completions;
};

struct MuonServicePluginPoolImpl : std::enable_shared_from_this<MuonServicePluginPoolImpl> {
  std::filesystem::path directory;
  std::vector<MuonPluginRuntimeLoadEntry> plugins;
  MuonPluginRuntimeServices services;
  std::map<std::string, std::shared_ptr<MuonPluginPolicy>> policies;
  std::unique_ptr<MuonPluginRuntime> runtime;
  std::map<std::string, std::shared_ptr<MuonServicePluginLeaseImpl>> owners;
  std::mutex owners_mutex;
  std::deque<std::function<void()>> acquisitions;
  uint64_t next_owner = 1;
  std::atomic<size_t> posted{0};
  std::atomic<size_t> runtime_posts{0};
  bool stopping = false;
  bool stopped = false;

  void Post(std::function<void()> task);
  void Acquire(const std::string& id, uint64_t generation,
      std::function<void(std::shared_ptr<MuonServicePluginLease>, const std::string&)> complete);
  void Close(const std::shared_ptr<MuonServicePluginLeaseImpl>& lease, std::function<void()> complete);
  void Invoke(const std::shared_ptr<MuonServicePluginLeaseImpl>& lease,
      MuonRpcCallRequest request, MuonPluginRuntime::Completion complete);
  void Advance();
  bool Receive(const MuonRpcMessage& message, std::string* error);
};
