/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "network/muon_network_request_handler.h"

#include "include/cef_frame.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_helpers.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

static constexpr int kHttpForbidden = 403;
static constexpr char kHttpForbiddenText[] = "Forbidden";
static constexpr char kTextPlainMimeType[] = "text/plain";
static constexpr char kTextHtmlMimeType[] = "text/html";

static void AppendHtmlEscaped(std::string* target, std::string_view value) {
  for (const auto character : value) {
    switch (character) {
      case '&':
        target->append("&amp;");
        break;
      case '<':
        target->append("&lt;");
        break;
      case '>':
        target->append("&gt;");
        break;
      case '"':
        target->append("&quot;");
        break;
      case '\'':
        target->append("&#39;");
        break;
      default:
        target->push_back(character);
        break;
    }
  }
}

static void AppendJavaScriptUnicodeEscape(std::string* target,
                                          unsigned char value) {
  static constexpr char kHex[] = "0123456789abcdef";
  target->append("\\u00");
  target->push_back(kHex[(value >> 4) & 0x0f]);
  target->push_back(kHex[value & 0x0f]);
}

static void AppendJavaScriptStringLiteral(std::string* target,
                                          std::string_view value) {
  target->push_back('"');
  for (const auto raw : value) {
    const auto byte = static_cast<unsigned char>(raw);
    switch (byte) {
      case '\\':
        target->append("\\\\");
        break;
      case '"':
        target->append("\\\"");
        break;
      case '\b':
        target->append("\\b");
        break;
      case '\f':
        target->append("\\f");
        break;
      case '\n':
        target->append("\\n");
        break;
      case '\r':
        target->append("\\r");
        break;
      case '\t':
        target->append("\\t");
        break;
      case '<':
        target->append("\\u003c");
        break;
      case '>':
        target->append("\\u003e");
        break;
      case '&':
        target->append("\\u0026");
        break;
      default:
        if (byte < 0x20) {
          AppendJavaScriptUnicodeEscape(target, byte);
        } else {
          target->push_back(static_cast<char>(byte));
        }
        break;
    }
  }
  target->push_back('"');
}

static std::string CreateForbiddenMessage(const std::string& url) {
  return std::string("Forbidden: blocked by muon network policy: ") + url;
}

static std::string CreateConsoleWarnScript(const std::string& message) {
  std::string script;
  script.reserve(message.size() + 32);
  script.append("console.warn(");
  AppendJavaScriptStringLiteral(&script, message);
  script.append(");");
  return script;
}

static std::string CreateForbiddenHtmlDocument(const std::string& url) {
  const auto message = CreateForbiddenMessage(url);
  std::string document;
  document.reserve(url.size() * 2 + message.size() + 512);
  document.append(R"HTML(<!doctype html>
<html>
<head>
<meta charset="utf-8">
<title>Forbidden</title>
<style>
:root {
  color-scheme: light;
}

html,
body {
  min-height: 100%;
  margin: 0;
  background: #fff;
  color: #111;
  font: 14px/1.45 sans-serif;
}

body {
  box-sizing: border-box;
  padding: 24px;
}

pre {
  white-space: pre-wrap;
  overflow-wrap: anywhere;
}
</style>
</head>
<body>
<h1>Forbidden</h1>
<p>Blocked by muon network policy:</p>
<pre>)HTML");
  AppendHtmlEscaped(&document, url);
  document.append(R"HTML(</pre>
<script>
console.error()HTML");
  AppendJavaScriptStringLiteral(&document, message);
  document.append(R"HTML();
</script>
</body>
</html>
)HTML");
  return document;
}

class MuonNetworkPolicyWarningTask final : public CefTask {
 public:
  MuonNetworkPolicyWarningTask(CefRefPtr<CefFrame> frame, std::string message)
      : frame_(frame), message_(std::move(message)) {}

  void Execute() override {
    CEF_REQUIRE_UI_THREAD();

    if (!frame_ || !frame_->IsValid()) {
      return;
    }
    frame_->ExecuteJavaScript(CreateConsoleWarnScript(message_),
                              "muon://network-policy", 0);
  }

 private:
  CefRefPtr<CefFrame> frame_;
  std::string message_;

  IMPLEMENT_REFCOUNTING(MuonNetworkPolicyWarningTask);
  DISALLOW_COPY_AND_ASSIGN(MuonNetworkPolicyWarningTask);
};

class MuonForbiddenResourceHandler final : public CefResourceHandler {
 public:
  MuonForbiddenResourceHandler(std::string url,
                               bool is_top_level_navigation,
                               bool is_head_response)
      : is_head_response_(is_head_response),
        mime_type_(is_top_level_navigation ? kTextHtmlMimeType
                                           : kTextPlainMimeType),
        body_(is_head_response ? std::string()
                               : is_top_level_navigation
                                     ? CreateForbiddenHtmlDocument(url)
                                     : std::string(kHttpForbiddenText)) {}

  bool Open(CefRefPtr<CefRequest> request,
            bool& handle_request,
            CefRefPtr<CefCallback> callback) override {
    handle_request = true;
    return true;
  }

  void GetResponseHeaders(CefRefPtr<CefResponse> response,
                          int64_t& response_length,
                          CefString& redirect_url) override {
    response->SetStatus(kHttpForbidden);
    response->SetStatusText(kHttpForbiddenText);
    response->SetMimeType(mime_type_);
    response_length = static_cast<int64_t>(body_.size());
  }

  bool Read(void* data_out,
            int bytes_to_read,
            int& bytes_read,
            CefRefPtr<CefResourceReadCallback> callback) override {
    bytes_read = 0;
    if (is_head_response_ || data_out == nullptr || bytes_to_read <= 0 ||
        read_offset_ >= body_.size()) {
      return false;
    }

    const auto available = body_.size() - read_offset_;
    const auto read_size =
        std::min(static_cast<size_t>(bytes_to_read), available);
    std::memcpy(data_out, body_.data() + read_offset_, read_size);
    read_offset_ += read_size;
    bytes_read = static_cast<int>(read_size);
    return true;
  }

  void Cancel() override {}

 private:
  bool is_head_response_ = false;
  std::string mime_type_;
  std::string body_;
  size_t read_offset_ = 0;

  IMPLEMENT_REFCOUNTING(MuonForbiddenResourceHandler);
  DISALLOW_COPY_AND_ASSIGN(MuonForbiddenResourceHandler);
};

class MuonNetworkResourceRequestHandler final
    : public CefResourceRequestHandler {
 public:
  explicit MuonNetworkResourceRequestHandler(
      std::shared_ptr<MuonNetworkPolicy> policy,
      bool is_top_level_navigation,
      std::string request_initiator)
      : policy_(std::move(policy)),
        is_top_level_navigation_(is_top_level_navigation),
        request_initiator_(std::move(request_initiator)) {}

  CefRefPtr<CefResourceHandler> GetResourceHandler(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      CefRefPtr<CefRequest> request) override {
    if (!request || !policy_ ||
        policy_->IsAllowedRequest(request->GetURL().ToString(),
                                  is_top_level_navigation_,
                                  request_initiator_)) {
      return nullptr;
    }

    const auto url = request->GetURL().ToString();
    if (!is_top_level_navigation_ && frame) {
      CefPostTask(TID_UI, new MuonNetworkPolicyWarningTask(
                              frame, CreateForbiddenMessage(url)));
    }

    const auto method = request->GetMethod().ToString();
    return new MuonForbiddenResourceHandler(url, is_top_level_navigation_,
                                            method == "HEAD");
  }

  void OnProtocolExecution(CefRefPtr<CefBrowser> browser,
                           CefRefPtr<CefFrame> frame,
                           CefRefPtr<CefRequest> request,
                           bool& allow_os_execution) override {
    allow_os_execution =
        request && policy_ &&
        policy_->IsAllowedRequest(request->GetURL().ToString(),
                                  is_top_level_navigation_,
                                  request_initiator_);
  }

 private:
  std::shared_ptr<MuonNetworkPolicy> policy_;
  bool is_top_level_navigation_ = false;
  std::string request_initiator_;

  IMPLEMENT_REFCOUNTING(MuonNetworkResourceRequestHandler);
  DISALLOW_COPY_AND_ASSIGN(MuonNetworkResourceRequestHandler);
};

CefRefPtr<CefResourceRequestHandler> CreateMuonNetworkResourceRequestHandler(
    std::shared_ptr<MuonNetworkPolicy> policy,
    bool is_top_level_navigation,
    std::string request_initiator) {
  return new MuonNetworkResourceRequestHandler(
      std::move(policy), is_top_level_navigation, std::move(request_initiator));
}
