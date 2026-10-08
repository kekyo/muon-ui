/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "app/muon_app_scheme.h"

#include "muon_cardio_post.h"

#include "include/cef_parser.h"

#include <cardio.h>

#include <algorithm>
#include <cstring>
#include <utility>

static constexpr int kHttpOk = 200;
static constexpr int kHttpForbidden = 403;
static constexpr int kHttpNotFound = 404;
static constexpr int kHttpMethodNotAllowed = 405;
static constexpr int kHttpInternalServerError = 500;

class MuonAppResourceHandler final : public CefResourceHandler {
 public:
  MuonAppResourceHandler(int status_code,
                         std::string status_text,
                         std::string mime_type,
                         std::vector<uint8_t> data,
                         bool is_head_response,
                         CefResponse::HeaderMap header_map)
      : status_code_(status_code),
        status_text_(std::move(status_text)),
        mime_type_(std::move(mime_type)),
        data_(std::move(data)),
        is_head_response_(is_head_response),
        header_map_(std::move(header_map)) {}

  MuonAppResourceHandler(std::shared_ptr<MuonAppStorage> storage,
                         MuonAppStorageRequest storage_request,
                         bool is_head_response,
                         cardio::dispatcher* dispatcher)
      : storage_(std::move(storage)),
        storage_request_(std::move(storage_request)),
        dispatcher_(dispatcher),
        is_head_response_(is_head_response) {}

  bool Open(CefRefPtr<CefRequest> request,
            bool& handle_request,
            CefRefPtr<CefCallback> callback) override {
    if (!storage_) {
      handle_request = true;
      return true;
    }

    if (dispatcher_ == nullptr) {
      CompleteStorageRead(nullptr);
      handle_request = true;
      return true;
    }

    handle_request = false;
    CefRefPtr<MuonAppResourceHandler> self(this);
    muon_internal::FireAndForgetOnDispatcher(dispatcher_, [self, callback] {
      self->CompleteStorageRead(callback);
    });
    return true;
  }

  void GetResponseHeaders(CefRefPtr<CefResponse> response,
                          int64_t& response_length,
                          CefString& redirect_url) override {
    response->SetStatus(status_code_);
    response->SetStatusText(status_text_);
    if (!mime_type_.empty()) {
      response->SetMimeType(mime_type_);
    }
    response->SetHeaderMap(header_map_);
    response_length = static_cast<int64_t>(data_.size());
  }

  bool Read(void* data_out,
            int bytes_to_read,
            int& bytes_read,
            CefRefPtr<CefResourceReadCallback> callback) override {
    bytes_read = 0;
    if (is_head_response_ || data_out == nullptr || bytes_to_read <= 0 ||
        read_offset_ >= data_.size()) {
      return false;
    }

    const auto available = data_.size() - read_offset_;
    const auto read_size =
        std::min(static_cast<size_t>(bytes_to_read), available);
    std::memcpy(data_out, data_.data() + read_offset_, read_size);
    read_offset_ += read_size;
    bytes_read = static_cast<int>(read_size);
    return true;
  }

  void Cancel() override {
    canceled_ = true;
  }

 private:
  void CompleteStorageRead(CefRefPtr<CefCallback> callback) {
    auto resource =
        storage_ ? storage_->ReadResource(storage_request_)
                 : MuonAppStorageResource{MuonAppStorageStatus::kReadError,
                                          "", {}};
    if (canceled_) {
      return;
    }
    ApplyResource(std::move(resource));
    if (callback) {
      callback->Continue();
    }
  }

  void ApplyResource(MuonAppStorageResource resource) {
    header_map_.clear();
    if (resource.status == MuonAppStorageStatus::kOk) {
      status_code_ = kHttpOk;
      status_text_ = "OK";
      mime_type_ = std::move(resource.mime_type);
      data_ = std::move(resource.data);
      return;
    }
    if (resource.status == MuonAppStorageStatus::kRejected) {
      status_code_ = kHttpForbidden;
      status_text_ = "Forbidden";
      mime_type_.clear();
      data_.clear();
      return;
    }
    if (resource.status == MuonAppStorageStatus::kReadError) {
      status_code_ = kHttpInternalServerError;
      status_text_ = "Internal Server Error";
      mime_type_.clear();
      data_.clear();
      return;
    }
    status_code_ = kHttpNotFound;
    status_text_ = "Not Found";
    mime_type_.clear();
    data_.clear();
  }

  std::shared_ptr<MuonAppStorage> storage_;
  MuonAppStorageRequest storage_request_;
  cardio::dispatcher* dispatcher_ = nullptr;
  bool canceled_ = false;
  int status_code_ = kHttpNotFound;
  std::string status_text_ = "Not Found";
  std::string mime_type_;
  std::vector<uint8_t> data_;
  bool is_head_response_ = false;
  CefResponse::HeaderMap header_map_;
  size_t read_offset_ = 0;

  IMPLEMENT_REFCOUNTING(MuonAppResourceHandler);
  DISALLOW_COPY_AND_ASSIGN(MuonAppResourceHandler);
};

class MuonAppSchemeHandlerFactory final : public CefSchemeHandlerFactory {
 public:
  MuonAppSchemeHandlerFactory(std::shared_ptr<MuonAppStorage> storage,
                              cardio::dispatcher* dispatcher)
      : storage_(std::move(storage)), dispatcher_(dispatcher) {}

  CefRefPtr<CefResourceHandler> Create(
      CefRefPtr<CefBrowser> browser,
      CefRefPtr<CefFrame> frame,
      const CefString& scheme_name,
      CefRefPtr<CefRequest> request) override {
    if (!storage_ || scheme_name.ToString() != kMuonAppSchemeName) {
      return nullptr;
    }

    const auto method = request->GetMethod().ToString();
    const auto is_head = method == "HEAD";
    if (method != "GET" && !is_head) {
      CefResponse::HeaderMap headers;
      headers.insert(std::make_pair("Allow", "GET, HEAD"));
      return new MuonAppResourceHandler(kHttpMethodNotAllowed,
                                        "Method Not Allowed", "", {}, false,
                                        std::move(headers));
    }

    CefURLParts parts;
    if (!CefParseURL(request->GetURL(), parts)) {
      return CreateStatusHandler(kHttpNotFound, "Not Found", is_head);
    }

    const auto host = CefString(&parts.host).ToString();
    const auto path = CefString(&parts.path).ToString();
    return new MuonAppResourceHandler(storage_, {host, path}, is_head,
                                      dispatcher_);
  }

 private:
  static CefRefPtr<CefResourceHandler> CreateStatusHandler(
      int status_code,
      const std::string& status_text,
      bool is_head) {
    return new MuonAppResourceHandler(status_code, status_text, "", {},
                                      is_head, {});
  }

  std::shared_ptr<MuonAppStorage> storage_;
  cardio::dispatcher* dispatcher_ = nullptr;

  IMPLEMENT_REFCOUNTING(MuonAppSchemeHandlerFactory);
  DISALLOW_COPY_AND_ASSIGN(MuonAppSchemeHandlerFactory);
};

int GetMuonAppStorageHttpStatus(MuonAppStorageStatus status) {
  if (status == MuonAppStorageStatus::kOk) {
    return kHttpOk;
  }
  if (status == MuonAppStorageStatus::kRejected) {
    return kHttpForbidden;
  }
  if (status == MuonAppStorageStatus::kReadError) {
    return kHttpInternalServerError;
  }
  return kHttpNotFound;
}

void RegisterMuonAppCustomScheme(CefRawPtr<CefSchemeRegistrar> registrar) {
  if (registrar) {
    registrar->AddCustomScheme(kMuonAppSchemeName, GetMuonAppSchemeOptions());
  }
}

CefRefPtr<CefSchemeHandlerFactory> CreateMuonAppSchemeHandlerFactory(
    std::shared_ptr<MuonAppStorage> storage,
    cardio::dispatcher* dispatcher) {
  return new MuonAppSchemeHandlerFactory(std::move(storage), dispatcher);
}
