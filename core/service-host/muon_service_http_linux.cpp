/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 */
#include "muon_service_http_desktop.h"
#include "services/muon_service_files.h"
#include <gio/gio.h>
#include <llhttp.h>
#include <array>
#include <memory>

struct HttpGObject { void operator()(gpointer value) const { if (value) g_object_unref(value); } };
template<class T> using HttpObject = std::unique_ptr<T, HttpGObject>;

struct HttpResponseParser {
  MuonServiceHttpResult result;
  std::string field, value;
  size_t header_bytes = 0;
  bool head = false, complete = false, exceeded = false;
};

static int HeaderPart(llhttp_t* parser, const char* bytes, size_t count, bool field) {
  auto& state = *static_cast<HttpResponseParser*>(parser->data);
  state.header_bytes += count;
  if (state.header_bytes > MuonHttpHeaderLimit) { state.exceeded = true; return HPE_USER; }
  (field ? state.field : state.value).append(bytes, count);
  return 0;
}
static int HeaderDone(llhttp_t* parser) {
  auto& state = *static_cast<HttpResponseParser*>(parser->data);
  try { AddMuonServiceHttpHeader(state.result, std::move(state.field), std::move(state.value)); }
  catch (const MuonServiceIoError&) { return HPE_USER; }
  state.field.clear(); state.value.clear();
  return 0;
}
static void CheckGio(GError* error, const MuonServiceIoCancellation& cancel) {
  if (!error) return;
  std::unique_ptr<GError, decltype(&g_error_free)> owned(error, g_error_free);
  cancel.Check();
  const char* code = error->domain == G_TLS_ERROR ? "tls-error" :
      g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT) ? "timeout" : "network-error";
  throw MuonServiceIoError(code, error->message);
}
MuonServiceHttpResult RunDesktopMuonServiceHttp(const MuonServiceHttpHop& hop, MuonServiceIoCancellation& cancel) {
  auto interrupt = std::shared_ptr<GCancellable>(g_cancellable_new(), HttpGObject{});
  cancel.Bind([interrupt] { g_cancellable_cancel(interrupt.get()); });
  cancel.Check();
  HttpObject<GSocketClient> client(g_socket_client_new());
  g_socket_client_set_enable_proxy(client.get(), FALSE);
  g_socket_client_set_tls(client.get(), hop.scheme == "https");
  g_socket_client_set_timeout(client.get(), static_cast<guint>((hop.timeout_ms + 999) / 1000));
  HttpObject<GSocketConnectable> address(g_network_address_new(hop.host.c_str(), hop.port));
  GError* error = nullptr;
  HttpObject<GSocketConnection> socket(g_socket_client_connect(client.get(), address.get(), interrupt.get(), &error));
  CheckGio(error, cancel);
  const auto output = g_io_stream_get_output_stream(G_IO_STREAM(socket.get()));
  auto request = hop.method + " " + hop.target + " HTTP/1.1\r\nHost: " + hop.origin.substr(hop.origin.find("://") + 3) +
      "\r\nConnection: close\r\nContent-Length: " + std::to_string(hop.body.size()) + "\r\n";
  for (const auto& [name, value] : hop.headers) request += name + ": " + value + "\r\n";
  request += "\r\n";
  g_output_stream_write_all(output, request.data(), request.size(), nullptr, interrupt.get(), &error);
  CheckGio(error, cancel);
  if (!hop.body.empty()) {
    g_output_stream_write_all(output, hop.body.data(), hop.body.size(), nullptr, interrupt.get(), &error);
    CheckGio(error, cancel);
  }
  HttpResponseParser state;
  state.head = hop.method == "HEAD";
  llhttp_settings_t settings;
  llhttp_settings_init(&settings);
  settings.on_header_field = [](llhttp_t* p, const char* data, size_t n) { return HeaderPart(p, data, n, true); };
  settings.on_header_value = [](llhttp_t* p, const char* data, size_t n) { return HeaderPart(p, data, n, false); };
  settings.on_header_value_complete = HeaderDone;
  settings.on_headers_complete = [](llhttp_t* p) {
    auto& s = *static_cast<HttpResponseParser*>(p->data);
    s.result.status = llhttp_get_status_code(p);
    if (!s.head && s.result.status >= 200 && s.result.status != 304 &&
        (p->flags & F_CONTENT_LENGTH) && p->content_length > MuonHttpBodyLimit) {
      s.exceeded = true;
      return -1;
    }
    return s.head ? 1 : 0;
  };
  settings.on_body = [](llhttp_t* p, const char* data, size_t n) {
    auto& s = *static_cast<HttpResponseParser*>(p->data);
    if (n > MuonHttpBodyLimit - s.result.body.size()) { s.exceeded = true; return -1; }
    s.result.body.insert(s.result.body.end(), data, data + n);
    return 0;
  };
  settings.on_message_complete = [](llhttp_t* p) {
    auto& s = *static_cast<HttpResponseParser*>(p->data);
    if (s.result.status < 200 && s.result.status != 101) { s.result = {}; return 0; }
    s.complete = true;
    return static_cast<int>(HPE_PAUSED);
  };
  llhttp_t parser;
  llhttp_init(&parser, HTTP_RESPONSE, &settings);
  parser.data = &state;
  const auto input = g_io_stream_get_input_stream(G_IO_STREAM(socket.get()));
  std::array<char, 16384> bytes;
  size_t wire_size = 0;
  while (!state.complete) {
    const auto count = g_input_stream_read(input, bytes.data(), bytes.size(), interrupt.get(), &error);
    CheckGio(error, cancel);
    wire_size += static_cast<size_t>(count);
    const auto parsed = count ? llhttp_execute(&parser, bytes.data(), static_cast<size_t>(count)) : llhttp_finish(&parser);
    if (state.exceeded || wire_size > MuonHttpBodyLimit * 2 + MuonHttpHeaderLimit)
      throw MuonServiceIoError("size-limit", "HTTP response exceeds transfer limits");
    if (!state.complete && (parsed != HPE_OK || !count))
      throw MuonServiceIoError("network-error", "Incomplete or invalid HTTP response");
  }
  return std::move(state.result);
}
