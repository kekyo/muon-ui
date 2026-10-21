/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "server/muon_server_http.h"
#include <iostream>
#include <stdexcept>

static void Expect(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
static MuonServerHttpParseResult Consume(
    const std::shared_ptr<MuonServerHttpParser>& parser, const std::string& bytes) {
  return parser->consume(std::span(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()));
}
int main() {
  try {
    // Every split position must preserve headers, binary bytes and completion.
    auto message = std::string("POST /echo?q=1 HTTP/1.1\r\nHost: localhost:8000\r\n"
        "Authorization: Bearer one\r\nAuthorization: Bearer two\r\nContent-Length: 4\r\n\r\n");
    message.append("\0\xff\x80\n", 4);
    for (size_t split = 1; split < message.size(); ++split) {
      const auto parser = CreateMuonServerHttpParser();
      const auto first = Consume(parser, message.substr(0, split));
      Expect(first.error_status == 0 && !first.request && first.consumed == split,
          "Partial request was not retained");
      const auto result = Consume(parser, message.substr(split));
      Expect(result.error_status == 0 && result.request, "Split request did not complete");
      const auto& request = *result.request;
      Expect(request.method == "POST" && request.target == "/echo?q=1", "Request target changed");
      Expect(request.headers.size() == 4 && request.headers[1].second == "Bearer one" &&
          request.headers[2].second == "Bearer two", "Duplicate authorization was lost");
      Expect(request.body == std::vector<uint8_t>({0, 255, 128, 10}), "Binary body changed");
    }
    const auto chunked = CreateMuonServerHttpParser();
    const auto decoded = Consume(chunked, "POST / HTTP/1.1\r\nHost: localhost:8000\r\n"
        "Transfer-Encoding: chunked\r\n\r\n2\r\nab\r\n1\r\nc\r\n0\r\nAuthorization: trailer\r\n\r\n");
    Expect(decoded.request && decoded.request->body == std::vector<uint8_t>({'a','b','c'}),
        "Chunked body did not decode");
    Expect(decoded.request->headers.size() == 2, "Trailer replaced initial headers");
    const auto upgrade = CreateMuonServerHttpParser();
    const auto headers = std::string("GET /ws HTTP/1.1\r\nHost: localhost:8000\r\n"
        "Connection: Upgrade\r\nUpgrade: websocket\r\n\r\n");
    const auto upgraded = Consume(upgrade, headers + "next");
    Expect(upgraded.request && upgraded.request->upgrade && upgraded.consumed == headers.size(),
        "Upgrade consumed following protocol bytes");
    const auto pipeline = Consume(CreateMuonServerHttpParser(),
        "GET /one HTTP/1.1\r\nHost: localhost:8000\r\n\r\nGET /two HTTP/1.1\r\n\r\n");
    Expect(pipeline.request && pipeline.request->target == "/one", "Parser dispatched a second request");
    for (const auto& invalid : {
        "POST / HTTP/1.1\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\na",
        "GET / HTTP/1.1\r\nInvalid header: x\r\n\r\n",
        "GET / HTTP/1.1\nHost: localhost:8000\n\n"}) {
      Expect(Consume(CreateMuonServerHttpParser(), invalid).error_status == 400,
          "Malformed HTTP was accepted");
    }
    Expect(Consume(CreateMuonServerHttpParser(),
        "POST / HTTP/1.1\r\nContent-Length: 1048577\r\n\r\n").error_status == 413,
        "Oversized declared body was not rejected before allocation");
    Expect(Consume(CreateMuonServerHttpParser(),
        "GET / HTTP/1.1\r\nX-Large: " + std::string(32768, 'x')).error_status == 431,
        "Unterminated oversized header was retained");
    Expect(Consume(CreateMuonServerHttpParser(),
        "GET / HTTP/1.1\r\nX-Large:" + std::string(32768, ' ') + "x\r\n\r\n").error_status == 431,
        "Discarded whitespace bypassed the header limit");
    Expect(Consume(CreateMuonServerHttpParser(),
        "GET /" + std::string(8192, 'x') + " HTTP/1.1\r\n\r\n").error_status == 400,
        "Oversized target was retained");
    const auto truncated = CreateMuonServerHttpParser();
    Consume(truncated, "POST / HTTP/1.1\r\nContent-Length: 2\r\n\r\na");
    Expect(truncated->finish().error_status == 400, "Truncated body completed at EOF");
    std::cout << "HTTP parser tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
