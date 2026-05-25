/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/muon_plugin_policy.h"

#include "config/muon_glob.h"

#include <utility>

static const std::string kMuonPluginGlobSeparators = ".";

struct MuonPluginPolicy::Impl {
  std::vector<MuonGlob> allow_globs;
};

MuonPluginPolicy::MuonPluginPolicy(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

MuonPluginPolicy::~MuonPluginPolicy() = default;

bool MuonPluginPolicy::IsAllowedFunctionPath(
    const std::string& function_path) const {
  if (!impl_) {
    return false;
  }
  for (const auto& allow_glob : impl_->allow_globs) {
    if (IsMuonGlobMatch(allow_glob, function_path)) {
      return true;
    }
  }
  return false;
}

bool MuonPluginPolicy::HasAllowPatterns() const {
  return impl_ && !impl_->allow_globs.empty();
}

bool CreateMuonPluginPolicy(
    const std::vector<std::string>& allow_patterns,
    std::shared_ptr<MuonPluginPolicy>* policy,
    std::string* error_message) {
  if (policy == nullptr || error_message == nullptr) {
    return false;
  }

  auto impl = std::make_unique<MuonPluginPolicy::Impl>();
  for (const auto& pattern : allow_patterns) {
    MuonGlob allow_glob;
    std::string glob_error;
    if (!CompileMuonGlob(pattern, kMuonPluginGlobSeparators, &allow_glob,
                         &glob_error)) {
      *error_message =
          "Invalid plugin allow glob '" + pattern + "': " + glob_error;
      return false;
    }
    impl->allow_globs.push_back(std::move(allow_glob));
  }

  *policy =
      std::shared_ptr<MuonPluginPolicy>(new MuonPluginPolicy(std::move(impl)));
  error_message->clear();
  return true;
}
