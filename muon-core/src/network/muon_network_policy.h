/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include "config/muon_config.h"

#include <memory>
#include <string>
#include <vector>

/**
 * URL allow policy backed by glob allow patterns.
 */
class MuonNetworkPolicy final {
 public:
  /**
   * Releases the policy implementation.
   */
  ~MuonNetworkPolicy();

  /**
   * Returns whether the supplied URL is allowed.
   *
   * @param url Fully qualified URL.
   * @return true when a configured glob fully matches.
   */
  bool IsAllowedUrl(const std::string& url) const;

  /**
   * Returns whether the supplied URL has an authorized origin.
   *
   * @param url Fully qualified URL or origin URL.
   * @return true when the URL origin matches network.authorizedOrigin.
   */
  bool IsAuthorizedOriginUrl(const std::string& url) const;

  /**
   * Returns whether a browser request is allowed.
   *
   * @param url Fully qualified request URL.
   * @param is_top_level_navigation Whether this is a main-frame navigation.
   * @param request_initiator Origin URL of the page that initiated the request.
   * @return true when the URL glob, navigation target origin, or initiator
   * origin is authorized.
   */
  bool IsAllowedRequest(const std::string& url,
                        bool is_top_level_navigation,
                        const std::string& request_initiator) const;

 private:
  struct Impl;

  explicit MuonNetworkPolicy(std::unique_ptr<Impl> impl);

  std::unique_ptr<Impl> impl_;

  friend bool CreateMuonUrlPolicy(
      const std::vector<std::string>& allow_patterns,
      const std::string& config_path,
      std::shared_ptr<MuonNetworkPolicy>* policy,
      std::string* error_message);
  friend bool CreateMuonNetworkPolicy(
      const std::vector<std::string>& allow_patterns,
      const std::vector<MuonAuthorizedOriginConfig>& authorized_origins,
      std::shared_ptr<MuonNetworkPolicy>* policy,
      std::string* error_message);
  friend bool CreateMuonNetworkPolicy(
      const std::vector<std::string>& allow_patterns,
      std::shared_ptr<MuonNetworkPolicy>* policy,
      std::string* error_message);
};

/**
 * Compiles a URL policy from glob allow patterns.
 *
 * @param allow_patterns Glob patterns applied to full URLs.
 * @param config_path Configuration path used in validation errors.
 * @param policy Receives the compiled policy.
 * @param error_message Receives a validation error on failure.
 * @return true when all patterns were compiled.
 */
bool CreateMuonUrlPolicy(const std::vector<std::string>& allow_patterns,
                         const std::string& config_path,
                         std::shared_ptr<MuonNetworkPolicy>* policy,
                         std::string* error_message);

/**
 * Compiles a network policy from glob allow patterns and authorized origins.
 *
 * @param allow_patterns Glob patterns applied to full request URLs.
 * @param authorized_origins Exact origins that authorize navigation targets and
 * initiated requests.
 * @param policy Receives the compiled policy.
 * @param error_message Receives a validation error on failure.
 * @return true when all patterns were compiled.
 */
bool CreateMuonNetworkPolicy(
    const std::vector<std::string>& allow_patterns,
    const std::vector<MuonAuthorizedOriginConfig>& authorized_origins,
    std::shared_ptr<MuonNetworkPolicy>* policy,
    std::string* error_message);

/**
 * Compiles a network policy from glob allow patterns and no authorized origins.
 *
 * @param allow_patterns Glob patterns applied to full request URLs.
 * @param policy Receives the compiled policy.
 * @param error_message Receives a validation error on failure.
 * @return true when all patterns were compiled.
 */
bool CreateMuonNetworkPolicy(
    const std::vector<std::string>& allow_patterns,
    std::shared_ptr<MuonNetworkPolicy>* policy,
    std::string* error_message);
