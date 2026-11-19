#include "services/muon_service_io.h"
#include <cassert>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>

int main() {
  const auto root = std::filesystem::temp_directory_path() / ("muon-files-" + std::to_string(std::random_device{}()));
  std::filesystem::create_directories(root / "data");
  struct Cleanup { std::filesystem::path root; ~Cleanup() { std::filesystem::remove_all(root); } } cleanup{root};
  std::deque<std::function<void()>> tasks;
  const auto post = [&](std::function<void()> task) { tasks.push_back(std::move(task)); };
  const auto pump = [&] {
    while (!tasks.empty()) { auto fn = std::move(tasks.front()); tasks.pop_front(); fn(); }
  };
  MuonServiceIo scope((root / "data").string(), {R"({"fs":{"data":"read-write","assets":{"initial":"AP8="}}})",
      [&](std::function<void()> work, std::function<void(std::string)> done) {
        post([work = std::move(work), done = std::move(done)] { work(); done({}); });
      }}, post);
  const auto request = [&](MuonServiceIoRequest input, bool cancel = false) {
    MuonServiceIoReply result;
    int calls = 0;
    auto stop = scope.Run(std::move(input), [&](MuonServiceIoReply reply) { result = std::move(reply); ++calls; });
    assert(calls == 0);
    if (cancel) stop();
    pump(); assert(calls == 1);
    return result;
  };
  auto initial = request({"fs.read", R"({"area":"assets","path":"initial"})", {}});
  assert(initial.code.empty() && (initial.body == std::vector<uint8_t>{0, 255}));
  assert(request({"fs.write", R"({"area":"assets","path":"initial"})", {5}}).code == "permission-denied");
  assert(request({"fs.mkdir", R"({"area":"data","path":"nested"})", {}}).code.empty());
  const std::string file = R"({"area":"data","path":"nested/value"})";
  assert(request({"fs.write", file, initial.body}).code.empty());
  assert(request({"fs.write", file, {5}}, true).code == "cancelled");
  assert(request({"fs.read", file, {}}).body == initial.body);
  assert(request({"fs.rename", R"({"area":"data","path":"nested/value","destination":"other"})", {}}).code.empty());
  assert(request({"fs.writeText", R"({"area":"data","path":"other","value":"日本\u0000語"})", {}}).code.empty());
  assert(request({"fs.readText", R"({"area":"data","path":"other"})", {}}).metadata == R"("日本\u0000語")");
  assert(request({"fs.list", R"({"area":"data","path":""})", {}}).metadata == R"(["nested","other"])");
  std::filesystem::create_directory_symlink(root, root / "data" / "outside");
  assert(request({"fs.write", R"({"area":"data","path":"outside/escape"})", {}}).code == "permission-denied");
  assert(!std::filesystem::exists(root / "escape"));
  bool drained = false;
  scope.Close([&] { drained = true; });
  assert(drained && request({"fs.write", file, {1}}).code == "cancelled");
  std::cout << "Generation-scoped service files, assets, cancellation, and atomic replacement passed\n";
}
