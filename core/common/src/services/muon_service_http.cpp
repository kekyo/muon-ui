/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#include "services/muon_service_http.h"
#include "services/muon_service_files.h"
#include "services/muon_service_interface.h"
#include <yyjson.h>
#include <algorithm>
#include <charconv>
#include <set>

void MuonServiceIoCancellation::Cancel(bool timeout) {
  std::function<void()> interrupt;
  {
    std::lock_guard lock(mutex_);
    if (cancelled.load()) return;
    timed_out_.store(timeout);
    cancelled.store(true);
    interrupt = std::move(interrupt_);
  }
  if (interrupt) interrupt();
}
void MuonServiceIoCancellation::Bind(std::function<void()> interrupt) {
  {
    std::lock_guard lock(mutex_);
    if (!cancelled.load()) { interrupt_ = std::move(interrupt); return; }
  }
  if (interrupt) interrupt();
}
void MuonServiceIoCancellation::Check() const {
  if (cancelled.load()) throw MuonServiceIoError(timed_out_.load() ? "timeout" : "cancelled", "Service I/O interrupted");
}

static void Invalid() { throw MuonServiceIoError("invalid-input", "Invalid service HTTP request"); }
static std::string Lower(std::string value) {
  for (auto& c : value) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return value;
}
static bool Token(const std::string& value) {
  return !value.empty() && value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&'*+-.^_`|~") == std::string::npos;
}
static bool SafeField(const std::string& value) {
  return std::all_of(value.begin(), value.end(), [](unsigned char c) { return c == 9 || (c >= 32 && c < 127); });
}
static std::string String(yyjson_val* value) {
  if (!yyjson_is_str(value)) Invalid();
  return {yyjson_get_str(value), yyjson_get_len(value)};
}
static std::string Quote(const std::string& value) {
  return EncodeMuonServiceValue({MUON_SERVICE_VALUE_STRING, {}, {}}, {value});
}
static auto Document(const std::string& json) {
  auto doc = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(yyjson_read(json.data(), json.size(), 0), yyjson_doc_free);
  if (!doc || !yyjson_is_obj(yyjson_doc_get_root(doc.get()))) Invalid();
  return doc;
}

MuonServiceHttpHop ParseMuonServiceHttpUrl(const std::string& url) {
  if (url.empty() || url.size() > 8192 || std::any_of(url.begin(), url.end(), [](unsigned char c) {
    return c <= 32 || c >= 127 || c == '\\';
  })) Invalid();
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos) Invalid();
  MuonServiceHttpHop result;
  result.scheme = Lower(url.substr(0, scheme_end));
  if (result.scheme != "http" && result.scheme != "https") Invalid();
  const auto start = scheme_end + 3;
  const auto end = url.find_first_of("/?#", start);
  const auto authority = Lower(url.substr(start, end - start));
  if (authority.empty() || authority.find('@') != std::string::npos) Invalid();
  const bool ipv6 = authority.front() == '[';
  const auto host_end = ipv6 ? authority.find(']') : authority.find(':');
  if (ipv6 && (host_end == std::string::npos || host_end == 1)) Invalid();
  const auto host = authority.substr(0, host_end == std::string::npos ? host_end : host_end + (ipv6 ? 1 : 0));
  result.host = ipv6 ? host.substr(1, host.size() - 2) : host;
  if (result.host.empty() || result.host.find_first_not_of(ipv6 ? "abcdef0123456789:." :
      "abcdefghijklmnopqrstuvwxyz0123456789-.") != std::string::npos) Invalid();
  const unsigned standard = result.scheme == "https" ? 443U : 80U;
  auto port = standard;
  if (host.size() < authority.size()) {
    if (authority[host.size()] != ':') Invalid();
    const auto last = authority.data() + authority.size();
    const auto number = std::from_chars(authority.data() + host.size() + 1, last, port);
    if (number.ec != std::errc{} || number.ptr != last || !port || port > 65535) Invalid();
  }
  result.port = static_cast<uint16_t>(port);
  result.origin = result.scheme + "://" + host + (port == standard ? "" : ":" + std::to_string(port));
  const auto hash = url.find('#', start);
  result.target = end == std::string::npos ? "/" : url.substr(end, hash - end);
  if (result.target.empty() || result.target.front() == '?') result.target.insert(0, "/");
  return result;
}

void AddMuonServiceHttpHeader(MuonServiceHttpResult& result, std::string name, std::string value) {
  if (!Token(name) || !SafeField(value)) throw MuonServiceIoError("network-error", "Invalid HTTP response header");
  auto [it, inserted] = result.headers.try_emplace(Lower(std::move(name)), value);
  if (!inserted) it->second += ", " + value;
}
int MuonServiceHttpTimeout(const std::string& parameters) {
  const auto doc = Document(parameters);
  const auto value = yyjson_obj_get(yyjson_doc_get_root(doc.get()), "timeoutMs");
  if (!value) return 30000;
  if (!yyjson_is_uint(value) || !yyjson_get_uint(value) || yyjson_get_uint(value) > 120000) Invalid();
  return static_cast<int>(yyjson_get_uint(value));
}
static std::string Redirect(const MuonServiceHttpHop& hop, const std::string& location) {
  const auto delimiter = location.find_first_of("/?#");
  if (location.substr(0, delimiter).find(':') != std::string::npos) return location;
  if (location.starts_with("//")) return hop.scheme + ":" + location;
  const auto path = hop.target.substr(0, hop.target.find('?'));
  if (location.starts_with('/')) return hop.origin + location;
  if (location.starts_with('?')) return hop.origin + path + location;
  if (location.empty() || location.starts_with('#')) return hop.origin + hop.target;
  return hop.origin + path.substr(0, path.rfind('/') + 1) + location;
}
MuonServiceIoReply RunMuonServiceHttp(const std::string& policy, const MuonServiceIoRequest& request,
    const MuonServiceHttpTransport& transport, MuonServiceIoCancellation& cancel) {
  const auto permissions = Document(policy);
  const auto list = yyjson_obj_get(yyjson_obj_get(yyjson_doc_get_root(permissions.get()), "http"), "origins");
  std::set<std::string> origins;
  for (size_t i = 0; i < yyjson_arr_size(list); ++i) origins.insert(String(yyjson_arr_get(list, i)));
  const auto options = Document(request.parameters);
  const auto root = yyjson_doc_get_root(options.get());
  auto hop = ParseMuonServiceHttpUrl(String(yyjson_obj_get(root, "url")));
  hop.method = yyjson_obj_get(root, "method") ? String(yyjson_obj_get(root, "method")) : "GET";
  if (!Token(hop.method) || hop.method.size() > 32 || Lower(hop.method) == "connect" || Lower(hop.method) == "trace") Invalid();
  const auto representation = yyjson_obj_get(root, "responseType") ? String(yyjson_obj_get(root, "responseType")) : "bytes";
  if (representation != "text" && representation != "bytes") Invalid();
  auto headers = yyjson_obj_get(root, "headers");
  if (headers && !yyjson_is_obj(headers)) Invalid();
  size_t i, max, header_bytes = 0; yyjson_val *key, *value;
  yyjson_obj_foreach(headers, i, max, key, value) {
    const auto name = Lower(String(key));
    const auto field = String(value);
    static const std::set<std::string> forbidden{"host","content-length","connection","transfer-encoding","upgrade","expect","proxy-authorization","proxy-connection","te","trailer"};
    header_bytes += name.size() + field.size() + 4;
    if (!Token(name) || !SafeField(field) || forbidden.contains(name) || !hop.headers.emplace(name, field).second) Invalid();
    if (header_bytes > MuonHttpHeaderLimit || i >= 128) throw MuonServiceIoError("size-limit", "Too many HTTP headers");
  }
  if (const auto text = yyjson_obj_get(root, "text")) {
    if (!request.body.empty()) Invalid();
    const auto content = String(text);
    hop.body.assign(content.begin(), content.end());
  } else hop.body = request.body;
  if (hop.body.size() > MuonHttpBodyLimit) throw MuonServiceIoError("size-limit", "HTTP request exceeds 1 MiB");
  if (!hop.body.empty() && (hop.method == "GET" || hop.method == "HEAD")) Invalid();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(MuonServiceHttpTimeout(request.parameters));
  for (unsigned redirects = 0;; ++redirects) {
    cancel.Check();
    if (!origins.contains(hop.origin) || !transport) throw MuonServiceIoError("permission-denied", "HTTP destination is not allowed");
    hop.timeout_ms = static_cast<int>(std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
    if (hop.timeout_ms <= 0) throw MuonServiceIoError("timeout", "HTTP deadline expired");
    const auto response = transport(hop, cancel);
    cancel.Bind({});
    cancel.Check();
    if (response.body.size() > MuonHttpBodyLimit) throw MuonServiceIoError("size-limit", "HTTP response exceeds 1 MiB");
    const auto status = response.status;
    const auto location = response.headers.find("location");
    if (location != response.headers.end() && (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)) {
      if (redirects >= 5) throw MuonServiceIoError("redirect-limit", "Too many HTTP redirects");
      auto next = ParseMuonServiceHttpUrl(Redirect(hop, location->second));
      next.headers = std::move(hop.headers);
      if (next.origin != hop.origin) { next.headers.erase("authorization"); next.headers.erase("cookie"); }
      if ((status == 303 && hop.method != "HEAD") || ((status == 301 || status == 302) && hop.method == "POST")) {
        next.method = "GET"; next.headers.erase("content-type");
      } else { next.method = std::move(hop.method); next.body = std::move(hop.body); }
      hop = std::move(next);
      continue;
    }
    MuonServiceIoReply result;
    result.metadata = "{\"status\":" + std::to_string(status) + ",\"headers\":{";
    bool first = true;
    for (const auto& [name, field] : response.headers) {
      if (!first) result.metadata += ',';
      first = false;
      result.metadata += Quote(name) + ':' + Quote(field);
    }
    result.metadata += '}';
    if (representation == "text") result.metadata += ",\"text\":" + Quote({response.body.begin(), response.body.end()});
    else result.body = response.body;
    result.metadata += '}';
    return result;
  }
}
