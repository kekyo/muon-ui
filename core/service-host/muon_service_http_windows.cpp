/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#include "muon_service_http_desktop.h"
#include "services/muon_service_files.h"
#include <windows.h>
#include <winhttp.h>
#include <array>
#include <charconv>
#include <condition_variable>
#include <utility>

struct WinHttpOwner {
  HINTERNET value = nullptr;
  ~WinHttpOwner() { if (value) WinHttpCloseHandle(value); }
};

static void HttpFailure(DWORD error) {
  const auto code = error == ERROR_WINHTTP_TIMEOUT ? "timeout" : error == ERROR_WINHTTP_SECURE_FAILURE ||
      error == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED ? "tls-error" : "network-error";
  throw MuonServiceIoError(code, "WinHTTP error " + std::to_string(error));
}
static std::wstring Wide(const std::string& ascii) { return {ascii.begin(), ascii.end()}; }

struct WinHttpTransfer {
  WinHttpOwner session, connection;
  HINTERNET request = nullptr;
  // A status callback may run inline during a WinHTTP call on this same thread.
  std::recursive_mutex mutex;
  std::condition_variable_any changed;
  DWORD expected = 0, error = 0, count = 0;
  bool ready = false;
  std::array<uint8_t, 16384> bytes;
  std::vector<uint8_t> outgoing;

  void Close() {
    HINTERNET closing;
    { std::lock_guard lock(mutex); closing = std::exchange(request, nullptr); }
    if (closing) WinHttpCloseHandle(closing);
    changed.notify_all();
  }
  void Await(DWORD stage, const std::function<BOOL(HINTERNET)>& action) {
    std::unique_lock lock(mutex);
    if (!request) HttpFailure(ERROR_WINHTTP_OPERATION_CANCELLED);
    expected = stage; ready = false;
    if (!action(request)) {
      const auto error_code = GetLastError();
      if (error_code != ERROR_IO_PENDING) HttpFailure(error_code);
    }
    changed.wait(lock, [&] { return ready || error || !request; });
    if (error || !request) HttpFailure(error ? error : ERROR_WINHTTP_OPERATION_CANCELLED);
  }
};
static void CALLBACK HttpStatus(HINTERNET, DWORD_PTR context, DWORD status, LPVOID info, DWORD length) {
  auto* holder = reinterpret_cast<std::shared_ptr<WinHttpTransfer>*>(context);
  if (!holder) return;
  const auto state = *holder;
  if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) { delete holder; return; }
  std::lock_guard lock(state->mutex);
  if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR) state->error = static_cast<WINHTTP_ASYNC_RESULT*>(info)->dwError;
  if (status == state->expected) { state->ready = true; state->count = length; }
  state->changed.notify_all();
}

static void ReadHeaders(WinHttpTransfer& state, MuonServiceHttpResult& result) {
  std::lock_guard lock(state.mutex);
  if (!state.request) HttpFailure(ERROR_WINHTTP_OPERATION_CANCELLED);
  DWORD status = 0, length = sizeof(status);
  if (!WinHttpQueryHeaders(state.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX, &status, &length, WINHTTP_NO_HEADER_INDEX)) HttpFailure(GetLastError());
  result.status = static_cast<int>(status);
  length = 0;
  WinHttpQueryHeaders(state.request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &length, WINHTTP_NO_HEADER_INDEX);
  if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) HttpFailure(GetLastError());
  if (length > (MuonHttpHeaderLimit + 1) * sizeof(wchar_t)) throw MuonServiceIoError("size-limit", "HTTP response headers exceed limit");
  std::wstring raw(length / sizeof(wchar_t), L'\0');
  if (!WinHttpQueryHeaders(state.request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
      raw.data(), &length, WINHTTP_NO_HEADER_INDEX)) HttpFailure(GetLastError());
  raw.resize(length / sizeof(wchar_t));
  size_t pos = 0;
  while (pos < raw.size()) {
    const auto end = raw.find(L"\r\n", pos);
    if (end == std::wstring::npos) break;
    const auto colon = raw.find(L':', pos);
    if (pos && colon < end) {
      const auto value = raw.find_first_not_of(L" \t", colon + 1);
      const auto start = std::min(value, end);
      AddMuonServiceHttpHeader(result, {raw.begin() + pos, raw.begin() + colon}, {raw.begin() + start, raw.begin() + end});
    }
    pos = end + 2;
  }
}
MuonServiceHttpResult RunDesktopMuonServiceHttp(const MuonServiceHttpHop& hop, MuonServiceIoCancellation& cancel) {
  const auto state = std::make_shared<WinHttpTransfer>();
  state->session.value = WinHttpOpen(L"muon", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
  if (!state->session.value) HttpFailure(GetLastError());
  if (!WinHttpSetTimeouts(state->session.value, hop.timeout_ms, hop.timeout_ms, hop.timeout_ms, hop.timeout_ms)) HttpFailure(GetLastError());
  state->connection.value = WinHttpConnect(state->session.value, Wide(hop.host).c_str(), hop.port, 0);
  if (!state->connection.value) HttpFailure(GetLastError());
  WinHttpOwner request{WinHttpOpenRequest(state->connection.value, Wide(hop.method).c_str(), Wide(hop.target).c_str(), nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, hop.scheme == "https" ? WINHTTP_FLAG_SECURE : 0)};
  if (!request.value) HttpFailure(GetLastError());
  DWORD disabled = WINHTTP_DISABLE_REDIRECTS | WINHTTP_DISABLE_COOKIES | WINHTTP_DISABLE_AUTHENTICATION;
  if (!WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled))) HttpFailure(GetLastError());
  auto holder = std::make_unique<std::shared_ptr<WinHttpTransfer>>(state);
  const auto context = reinterpret_cast<DWORD_PTR>(holder.get());
  auto context_value = context;
  if (!WinHttpSetOption(request.value, WINHTTP_OPTION_CONTEXT_VALUE, &context_value, sizeof(context_value))) HttpFailure(GetLastError());
  if (WinHttpSetStatusCallback(request.value, HttpStatus, WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0) ==
      WINHTTP_INVALID_STATUS_CALLBACK) HttpFailure(GetLastError());
  holder.release(); // released by the final HANDLE_CLOSING notification
  state->request = std::exchange(request.value, nullptr);
  struct CloseTransfer { WinHttpTransfer& state; ~CloseTransfer() { state.Close(); } } guard{*state};
  cancel.Bind([state] { state->Close(); });
  cancel.Check();
  std::wstring fields;
  for (const auto& [name, value] : hop.headers) fields += Wide(name + ": " + value + "\r\n");
  state->outgoing = hop.body;
  const auto size = static_cast<DWORD>(state->outgoing.size());
  state->Await(WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE, [&](HINTERNET handle) {
    return WinHttpSendRequest(handle, fields.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : fields.c_str(),
        static_cast<DWORD>(fields.size()), size ? state->outgoing.data() : WINHTTP_NO_REQUEST_DATA, size, size, context);
  });
  state->Await(WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE, [](HINTERNET handle) { return WinHttpReceiveResponse(handle, nullptr); });
  MuonServiceHttpResult result;
  ReadHeaders(*state, result);
  const auto length = result.headers.find("content-length");
  if (hop.method != "HEAD" && result.status != 304 && length != result.headers.end()) {
    uint64_t declared = 0;
    const auto& text = length->second;
    const auto number = std::from_chars(text.data(), text.data() + text.size(), declared);
    if (number.ec == std::errc::result_out_of_range || (number.ec == std::errc{} && declared > MuonHttpBodyLimit))
      throw MuonServiceIoError("size-limit", "HTTP response exceeds 1 MiB");
  }
  for (;;) {
    state->Await(WINHTTP_CALLBACK_STATUS_READ_COMPLETE, [&](HINTERNET handle) {
      return WinHttpReadData(handle, state->bytes.data(), static_cast<DWORD>(state->bytes.size()), nullptr);
    });
    if (!state->count) break;
    if (state->count > MuonHttpBodyLimit - result.body.size()) throw MuonServiceIoError("size-limit", "HTTP response exceeds 1 MiB");
    result.body.insert(result.body.end(), state->bytes.begin(), state->bytes.begin() + state->count);
  }
  return result;
}
