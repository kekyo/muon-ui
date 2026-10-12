/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "plugins/muon_plugin_metadata.h"

#include <cctype>

bool IsValidMuonJsIdentifier(const std::string& name) {
  if (name.empty()) {
    return false;
  }

  const auto first = static_cast<unsigned char>(name[0]);
  if (!(std::isalpha(first) != 0 || name[0] == '_' || name[0] == '$')) {
    return false;
  }

  for (auto index = size_t{1}; index < name.size(); ++index) {
    const auto character = static_cast<unsigned char>(name[index]);
    if (!(std::isalnum(character) != 0 || name[index] == '_' ||
          name[index] == '$')) {
      return false;
    }
  }
  return true;
}

bool SplitMuonPluginNamespace(const std::string& plugin_namespace,
                               std::vector<std::string>* segments) {
  if (segments != nullptr) {
    segments->clear();
  }
  if (plugin_namespace.empty()) {
    return false;
  }

  auto begin = size_t{0};
  while (begin <= plugin_namespace.size()) {
    const auto dot = plugin_namespace.find('.', begin);
    const auto end = dot == std::string::npos ? plugin_namespace.size() : dot;
    const auto segment = plugin_namespace.substr(begin, end - begin);
    if (!IsValidMuonJsIdentifier(segment)) {
      if (segments != nullptr) {
        segments->clear();
      }
      return false;
    }
    if (segments != nullptr) {
      segments->push_back(segment);
    }
    if (dot == std::string::npos) {
      return true;
    }
    begin = dot + 1;
  }
  if (segments != nullptr) {
    segments->clear();
  }
  return false;
}

bool IsValidMuonPluginNamespace(const std::string& plugin_namespace) {
  return SplitMuonPluginNamespace(plugin_namespace, nullptr);
}

std::string CreateMuonFunctionPublicPath(
    const std::string& plugin_namespace,
    const std::string& js_name) {
  return plugin_namespace + "." + js_name;
}

std::string CreateMuonFunctionPublicPath(
    const MuonFunctionMetadata& function) {
  return CreateMuonFunctionPublicPath(
      function.plugin_namespace,
      function.public_name.empty() ? function.js_name : function.public_name);
}
