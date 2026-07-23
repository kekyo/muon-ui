/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "network/muon_network_policy.h"

#include "config/muon_glob.h"
#include "muon_string_helpers.h"

#include "include/cef_parser.h"
#include "include/internal/cef_types.h"

#include <cctype>
#include <utility>

static const std::string kMuonNetworkGlobSeparators = ":/?#";
static constexpr int kMuonNoPort = 0;
static constexpr int kMuonHttpDefaultPort = 80;
static constexpr int kMuonHttpsDefaultPort = 443;

using muon_internal::ToLowerAscii;

struct MuonNormalizedOrigin {
  std::string scheme;
  std::string domain;
  int port = kMuonNoPort;
};

struct MuonNetworkPolicy::Impl {
  std::vector<MuonGlob> allow_globs;
  std::vector<MuonNormalizedOrigin> authorized_origins;
  std::vector<MuonNormalizedOrigin> loopback_origins;
  std::vector<MuonNormalizedOrigin> local_network_origins;
};

static bool ParseMuonUrlOrigin(const std::string& url,
                               MuonNormalizedOrigin* origin);
static bool IsSameMuonOrigin(const MuonNormalizedOrigin& left,
                             const MuonNormalizedOrigin& right);

MuonNetworkPolicy::MuonNetworkPolicy(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

MuonNetworkPolicy::~MuonNetworkPolicy() = default;

bool MuonNetworkPolicy::IsAllowedUrl(const std::string& url) const {
  if (!impl_) {
    return false;
  }
  for (const auto& allow_glob : impl_->allow_globs) {
    if (IsMuonGlobMatch(allow_glob, url)) {
      return true;
    }
  }
  return false;
}

bool MuonNetworkPolicy::IsAuthorizedOriginUrl(const std::string& url) const {
  if (!impl_) {
    return false;
  }
  MuonNormalizedOrigin origin;
  if (!ParseMuonUrlOrigin(url, &origin)) {
    return false;
  }
  for (const auto& authorized_origin : impl_->authorized_origins) {
    if (IsSameMuonOrigin(origin, authorized_origin)) {
      return true;
    }
  }
  return false;
}

bool MuonNetworkPolicy::IsAllowedRequest(
    const std::string& url,
    bool is_top_level_navigation,
    const std::string& request_initiator) const {
  if (IsAllowedUrl(url)) {
    return true;
  }
  if (is_top_level_navigation) {
    return IsAuthorizedOriginUrl(url);
  }
  if (request_initiator.empty()) {
    return false;
  }
  return IsAuthorizedOriginUrl(request_initiator);
}

static bool ContainsMuonOrigin(
    const std::vector<MuonNormalizedOrigin>& configured_origins,
    const MuonNormalizedOrigin& requesting_origin) {
  for (const auto& configured_origin : configured_origins) {
    if (IsSameMuonOrigin(requesting_origin, configured_origin)) {
      return true;
    }
  }
  return false;
}

MuonLocalAccessPermissionDecision
MuonNetworkPolicy::GetLocalAccessPermissionDecision(
    const std::string& requesting_origin,
    uint32_t requested_permissions) const {
  const auto legacy_permission =
      static_cast<uint32_t>(CEF_PERMISSION_TYPE_LOCAL_NETWORK_ACCESS);
  const auto local_network_permission =
      static_cast<uint32_t>(CEF_PERMISSION_TYPE_LOCAL_NETWORK);
  const auto loopback_permission =
      static_cast<uint32_t>(CEF_PERMISSION_TYPE_LOOPBACK_NETWORK);
  const auto local_access_permissions = legacy_permission |
                                        local_network_permission |
                                        loopback_permission;
  if ((requested_permissions & local_access_permissions) == 0) {
    return MuonLocalAccessPermissionDecision::kUnhandled;
  }
  if ((requested_permissions & ~local_access_permissions) != 0 || !impl_) {
    return MuonLocalAccessPermissionDecision::kDeny;
  }

  MuonNormalizedOrigin origin;
  if (!ParseMuonUrlOrigin(requesting_origin, &origin)) {
    return MuonLocalAccessPermissionDecision::kDeny;
  }
  const auto allows_loopback = ContainsMuonOrigin(impl_->loopback_origins,
                                                  origin);
  const auto allows_local_network = ContainsMuonOrigin(
      impl_->local_network_origins, origin);
  if ((requested_permissions & loopback_permission) != 0 &&
      !allows_loopback) {
    return MuonLocalAccessPermissionDecision::kDeny;
  }
  if ((requested_permissions & local_network_permission) != 0 &&
      !allows_local_network) {
    return MuonLocalAccessPermissionDecision::kDeny;
  }
  if ((requested_permissions & legacy_permission) != 0 &&
      (!allows_loopback || !allows_local_network)) {
    return MuonLocalAccessPermissionDecision::kDeny;
  }
  return MuonLocalAccessPermissionDecision::kAccept;
}

static bool ParseMuonPortString(const std::string& value, int* port) {
  if (port == nullptr) {
    return false;
  }
  if (value.empty()) {
    *port = kMuonNoPort;
    return true;
  }
  auto parsed = 0;
  for (const auto character : value) {
    if (std::isdigit(static_cast<unsigned char>(character)) == 0) {
      return false;
    }
    parsed = parsed * 10 + (character - '0');
    if (parsed > 65535) {
      return false;
    }
  }
  if (parsed == kMuonNoPort) {
    return false;
  }
  *port = parsed;
  return true;
}

static int GetMuonDefaultOriginPort(const std::string& scheme) {
  if (scheme == "http") {
    return kMuonHttpDefaultPort;
  }
  if (scheme == "https") {
    return kMuonHttpsDefaultPort;
  }
  return kMuonNoPort;
}

static int NormalizeMuonOriginPort(const std::string& scheme, int port) {
  if (port != kMuonNoPort) {
    return port;
  }
  return GetMuonDefaultOriginPort(scheme);
}

static bool ParseMuonUrlOrigin(const std::string& url,
                               MuonNormalizedOrigin* origin) {
  if (origin == nullptr) {
    return false;
  }
  CefURLParts parts;
  if (!CefParseURL(url, parts)) {
    return false;
  }
  const auto scheme = ToLowerAscii(CefString(&parts.scheme).ToString());
  const auto domain = ToLowerAscii(CefString(&parts.host).ToString());
  if (scheme.empty() || domain.empty()) {
    return false;
  }

  int port = kMuonNoPort;
  if (!ParseMuonPortString(CefString(&parts.port).ToString(), &port)) {
    return false;
  }
  *origin = {scheme, domain, NormalizeMuonOriginPort(scheme, port)};
  return true;
}

static MuonNormalizedOrigin NormalizeMuonAuthorizedOrigin(
    const MuonAuthorizedOriginConfig& origin) {
  const auto scheme = ToLowerAscii(origin.scheme);
  const auto domain = ToLowerAscii(origin.domain);
  return {scheme, domain, NormalizeMuonOriginPort(scheme, origin.port)};
}

static bool IsSameMuonOrigin(const MuonNormalizedOrigin& left,
                             const MuonNormalizedOrigin& right) {
  return left.scheme == right.scheme && left.domain == right.domain &&
         left.port == right.port;
}

static bool AppendMuonOrigins(
    const std::vector<MuonAuthorizedOriginConfig>& configured_origins,
    const std::string& config_path,
    std::vector<MuonNormalizedOrigin>* normalized_origins,
    std::string* error_message) {
  if (normalized_origins == nullptr || error_message == nullptr) {
    return false;
  }
  for (const auto& origin : configured_origins) {
    if (origin.scheme.empty() || origin.domain.empty()) {
      *error_message = "Invalid " + config_path +
                       " entry: scheme and domain are required";
      return false;
    }
    if (origin.port < 0 || origin.port > 65535) {
      *error_message = "Invalid " + config_path +
                       " entry: port must be from 1 to 65535";
      return false;
    }
    normalized_origins->push_back(NormalizeMuonAuthorizedOrigin(origin));
  }
  return true;
}

bool CreateMuonUrlPolicy(const std::vector<std::string>& allow_patterns,
                         const std::string& config_path,
                         std::shared_ptr<MuonNetworkPolicy>* policy,
                         std::string* error_message) {
  if (policy == nullptr || error_message == nullptr) {
    return false;
  }

  auto impl = std::make_unique<MuonNetworkPolicy::Impl>();
  for (const auto& pattern : allow_patterns) {
    MuonGlob allow_glob;
    std::string glob_error;
    if (!CompileMuonGlob(pattern, kMuonNetworkGlobSeparators, &allow_glob,
                         &glob_error)) {
      *error_message =
          "Invalid " + config_path + " glob '" + pattern + "': " + glob_error;
      return false;
    }
    impl->allow_globs.push_back(std::move(allow_glob));
  }

  *policy =
      std::shared_ptr<MuonNetworkPolicy>(new MuonNetworkPolicy(std::move(impl)));
  error_message->clear();
  return true;
}

bool CreateMuonNetworkPolicy(
    const std::vector<std::string>& allow_patterns,
    const std::vector<MuonAuthorizedOriginConfig>& authorized_origins,
    const std::vector<MuonAuthorizedOriginConfig>& loopback_origins,
    const std::vector<MuonAuthorizedOriginConfig>& local_network_origins,
    std::shared_ptr<MuonNetworkPolicy>* policy,
    std::string* error_message) {
  if (policy == nullptr || error_message == nullptr) {
    return false;
  }
  if (!CreateMuonUrlPolicy(allow_patterns, "network.allow", policy,
                           error_message)) {
    return false;
  }
  if (!AppendMuonOrigins(authorized_origins, "network.authorizedOrigin",
                         &(*policy)->impl_->authorized_origins,
                         error_message) ||
      !AppendMuonOrigins(loopback_origins,
                         "network.localAccess.loopbackOrigins",
                         &(*policy)->impl_->loopback_origins, error_message) ||
      !AppendMuonOrigins(local_network_origins,
                         "network.localAccess.localNetworkOrigins",
                         &(*policy)->impl_->local_network_origins,
                         error_message)) {
    policy->reset();
    return false;
  }
  return true;
}

bool CreateMuonNetworkPolicy(
    const std::vector<std::string>& allow_patterns,
    const std::vector<MuonAuthorizedOriginConfig>& authorized_origins,
    std::shared_ptr<MuonNetworkPolicy>* policy,
    std::string* error_message) {
  return CreateMuonNetworkPolicy(allow_patterns, authorized_origins, {}, {},
                                 policy, error_message);
}

bool CreateMuonNetworkPolicy(
    const std::vector<std::string>& allow_patterns,
    std::shared_ptr<MuonNetworkPolicy>* policy,
    std::string* error_message) {
  return CreateMuonNetworkPolicy(allow_patterns, {}, {}, {}, policy,
                                 error_message);
}
