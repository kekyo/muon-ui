/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "network/muon_network_policy.h"

#include "config/muon_glob.h"

#include "include/cef_parser.h"

#include <cctype>
#include <utility>

static const std::string kMuonNetworkGlobSeparators = ":/?#";
static constexpr int kMuonNoPort = 0;
static constexpr int kMuonHttpDefaultPort = 80;
static constexpr int kMuonHttpsDefaultPort = 443;

struct MuonNormalizedOrigin {
  std::string scheme;
  std::string domain;
  int port = kMuonNoPort;
};

struct MuonNetworkPolicy::Impl {
  std::vector<MuonGlob> allow_globs;
  std::vector<MuonNormalizedOrigin> authorized_origins;
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

static std::string ToLowerAscii(std::string value) {
  for (auto& character : value) {
    character = static_cast<char>(
        std::tolower(static_cast<unsigned char>(character)));
  }
  return value;
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

static bool AppendMuonAuthorizedOrigins(
    const std::vector<MuonAuthorizedOriginConfig>& authorized_origins,
    std::vector<MuonNormalizedOrigin>* normalized_origins,
    std::string* error_message) {
  if (normalized_origins == nullptr || error_message == nullptr) {
    return false;
  }
  for (const auto& origin : authorized_origins) {
    if (origin.scheme.empty() || origin.domain.empty()) {
      *error_message =
          "Invalid network.authorizedOrigin entry: scheme and domain are "
          "required";
      return false;
    }
    if (origin.port < 0 || origin.port > 65535) {
      *error_message =
          "Invalid network.authorizedOrigin entry: port must be from 1 to "
          "65535";
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
    std::shared_ptr<MuonNetworkPolicy>* policy,
    std::string* error_message) {
  if (policy == nullptr || error_message == nullptr) {
    return false;
  }
  if (!CreateMuonUrlPolicy(allow_patterns, "network.allow", policy,
                           error_message)) {
    return false;
  }
  if (!AppendMuonAuthorizedOrigins(
          authorized_origins, &(*policy)->impl_->authorized_origins,
          error_message)) {
    policy->reset();
    return false;
  }
  return true;
}

bool CreateMuonNetworkPolicy(
    const std::vector<std::string>& allow_patterns,
    std::shared_ptr<MuonNetworkPolicy>* policy,
    std::string* error_message) {
  return CreateMuonNetworkPolicy(allow_patterns, {}, policy, error_message);
}
