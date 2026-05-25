/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "network/muon_network_request_handler.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

static constexpr int kHttpForbidden = 403;
static constexpr char kHttpForbiddenText[] = "Forbidden";
static constexpr char kTextPlainMimeType[] = "text/plain";

class MuonForbiddenResourceHandler final : public CefResourceHandler {
 public:
  explicit MuonForbiddenResourceHandler(bool is_head_response)
      : is_head_response_(is_head_response),
        body_(kHttpForbiddenText,
              kHttpForbiddenText + std::strlen(kHttpForbiddenText)) {}

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
    response->SetMimeType(kTextPlainMimeType);
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
  std::vector<uint8_t> body_;
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

    const auto method = request->GetMethod().ToString();
    return new MuonForbiddenResourceHandler(method == "HEAD");
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
