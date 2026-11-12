/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "services/muon_service_session.h"
#include <yyjson.h>
#include <iostream>

static void CheckReopenedState(const std::string& before, const std::string& after) {
  auto first = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(before.data(), before.size(), 0), yyjson_doc_free);
  auto second = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(after.data(), after.size(), 0), yyjson_doc_free);
  auto* previous = yyjson_doc_get_root(first.get());
  auto* current = yyjson_doc_get_root(second.get());
  const auto* generation = yyjson_get_str(yyjson_obj_get(previous, "generation"));
  if (!generation || !yyjson_equals_str(yyjson_obj_get(current, "generation"), generation) ||
      !yyjson_equals_str(yyjson_obj_get(current, "state"), "running") ||
      !yyjson_is_true(yyjson_obj_get(current, "resident")) ||
      yyjson_get_uint(yyjson_obj_get(current, "clientReferences")) != 0)
    throw std::runtime_error("Resident state was not preserved after UI exit");
}

static cardio::promise<void> Run(MuonServiceProcessOptions options,
    cardio::dispatcher_group_glib& group, bool& passed) {
  MuonServiceSession first(options);
  MuonServiceSession second(options);
  try {
    co_await first.Start();
    auto page1 = first.Call("page1", "acquire", "{\"serviceId\":\"worker\"}");
    auto page2 = first.Call("page2", "acquire", "{\"serviceId\":\"worker\"}");
    const auto reference1 = co_await page1;
    const auto reference2 = co_await page2;
    if (reference1 == reference2) throw std::runtime_error("Pages received the same reference");
    first.ReleaseOwner("page1");
    co_await first.Call("page2", "release", reference2);
    const auto before = co_await first.Call("page2", "getState", "{\"serviceId\":\"worker\"}");
    co_await first.Stop(false);
    // Normal UI exit must preserve the native generation. A new UI session
    // attaches to that host and explicit application shutdown ends it.
    co_await second.Start();
    const auto after = co_await second.Call("page3", "getState", "{\"serviceId\":\"worker\"}");
    CheckReopenedState(before, after);
    co_await second.Stop(true);
    passed = true;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
  }
  co_await first.Stop(true);
  co_await second.Stop(true);
  group.shutdown();
}

int main(int argc, char** argv) {
  if (argc != 5) return 2;
  cardio::dispatcher_group_glib group;
  cardio::dispatcher_host_glib dispatcher(group);
  bool passed = false;
  cardio::fire_and_forget(Run({argv[1], argv[2], argv[3], argv[4]}, group, passed));
  dispatcher.park();
  return passed ? 0 : 1;
}
