/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "services/muon_service_client.h"
#include "services/muon_service_transport.h"
#include <yyjson.h>
#include <iostream>

static std::string StringField(const std::string& json, const char* name) {
  auto doc = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(json.data(), json.size(), 0), yyjson_doc_free);
  const auto* value = yyjson_get_str(yyjson_obj_get(yyjson_doc_get_root(doc.get()), name));
  if (!value) throw std::runtime_error("Missing driver response field");
  return value;
}

static cardio::promise<void> Run(std::string endpoint, bool& passed) {
  const auto connector = [endpoint](cardio::cancellation cancellation) {
    return ConnectMuonServiceTransport(endpoint, cancellation);
  };
  MuonServiceClient first(connector);
  MuonServiceClient second(connector);
  try {
    auto acquire1 = first.Call("acquire", "{\"serviceId\":\"resident\"}");
    auto acquire2 = second.Call("acquire", "{\"serviceId\":\"resident\"}");
    const auto reference1 = co_await acquire1;
    const auto reference2 = co_await acquire2;
    if (StringField(reference1, "generation") != StringField(reference2, "generation") ||
        StringField(reference1, "refId") == StringField(reference2, "refId"))
      throw std::runtime_error("Native page clients did not receive independent references");
    co_await first.Call("release", reference1);
    co_await second.Call("release", reference2);
    const auto state = co_await second.Call("getState", "{\"serviceId\":\"resident\"}");
    if (StringField(state, "state") != "running") throw std::runtime_error("Resident stopped after release");
    passed = true;
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
  }
  auto close1 = first.Close();
  auto close2 = second.Close();
  co_await close1;
  co_await close2;
}

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  cardio::dispatcher_host dispatcher;
  bool passed = false;
  auto work = Run(argv[1], passed);
  dispatcher.park();
  return passed ? 0 : 1;
}
