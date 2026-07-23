/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

/**
 * Plugin function access policy backed by glob allow patterns.
 */
class MuonPluginPolicy final {
 public:
  /**
   * Releases the policy implementation.
   */
  ~MuonPluginPolicy();

  /**
   * Returns whether the plugin function path can be exposed.
   *
   * @param function_path Full plugin function path such as `muon.fs.readFile`.
   * @return true when a configured glob fully matches.
   */
  bool IsAllowedFunctionPath(const std::string& function_path) const;

  /**
   * Returns whether this policy has any allow patterns.
   */
  bool HasAllowPatterns() const;

 private:
  struct Impl;

  explicit MuonPluginPolicy(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;

  friend bool CreateMuonPluginPolicy(
      const std::vector<std::string>& allow_patterns,
      std::shared_ptr<MuonPluginPolicy>* policy,
      std::string* error_message);
};

/**
 * Compiles a plugin policy from glob allow patterns.
 *
 * @param allow_patterns Glob patterns applied to full plugin function paths.
 * @param policy Receives the compiled policy.
 * @param error_message Receives a validation error on failure.
 * @return true when all patterns were compiled.
 */
bool CreateMuonPluginPolicy(
    const std::vector<std::string>& allow_patterns,
    std::shared_ptr<MuonPluginPolicy>* policy,
    std::string* error_message);
