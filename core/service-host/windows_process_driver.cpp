/* Native launcher/session observations used by the Windows Vitest driver. */
#include "services/muon_service_process.h"
#include "services/muon_service_session.h"
#include "muon_filesystem.h"
#include <windows.h>
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

struct ProcessObservation {
  HANDLE handle = nullptr;
  ~ProcessObservation() { if (handle) CloseHandle(handle); }
};

static DWORD FindHost(const std::string& endpoint) {
  const auto title = muon_internal::FilesystemPathFromUtf8(endpoint).native();
  const auto window = FindWindowW(L"MuonServiceSessionHost", title.c_str());
  DWORD pid = 0;
  if (!window || !GetWindowThreadProcessId(window, &pid))
    throw std::runtime_error("Ready host has no session window");
  return pid;
}

static cardio::promise<void> Wait(DWORD pid) {
  ProcessObservation process{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
  if (!process.handle) {
    if (GetLastError() == ERROR_INVALID_PARAMETER) co_return;
    throw std::runtime_error("Cannot observe launched host");
  }
  std::cout << "{\"waiting\":true}" << std::endl;
  co_await cardio::from_win32_handle(process.handle);
  DWORD code = 1;
  if (!GetExitCodeProcess(process.handle, &code) || code != 0)
    throw std::runtime_error("Launched host did not exit successfully");
}

static cardio::promise<void> Launch(MuonServiceProcessOptions options, std::string mode) {
  if (mode == "session") {
    MuonServiceSession first(options);
    MuonServiceSession second(options);
    DWORD pid = 0;
    std::exception_ptr failure;
    try {
      co_await first.Start();
      pid = FindHost(options.endpoint);
      auto a = first.Call("page-a", "acquire", "{\"serviceId\":\"worker\"}");
      auto b = first.Call("page-b", "acquire", "{\"serviceId\":\"worker\"}");
      const auto reference_a = co_await a;
      const auto reference_b = co_await b;
      if (reference_a == reference_b) throw std::runtime_error("Page references collided");
      first.ReleaseOwner("page-a");
      co_await first.Call("page-b", "release", reference_b);
      const auto before = co_await first.Call("page-b", "getState", "{\"serviceId\":\"worker\"}");
      co_await first.Stop(false);
      co_await second.Start();
      if (pid != FindHost(options.endpoint)) throw std::runtime_error("UI reopen replaced the resident process");
      const auto after = co_await second.Call("page-c", "getState", "{\"serviceId\":\"worker\"}");
      CheckReopenedState(before, after);
      co_await second.Stop(true);
    } catch (...) { failure = std::current_exception(); }
    co_await first.Stop(true);
    co_await second.Stop(true);
    if (failure) std::rethrow_exception(failure);
    co_await Wait(pid);
    std::cout << "{\"session\":true}" << std::endl;
    co_return;
  }
  auto process = co_await LaunchMuonServiceProcess(options, {});
  const auto pid = FindHost(options.endpoint);
  if (mode == "detach") {
    co_await process.detach();
    co_await process.detach();
  }
  std::cout << "{\"launched\":true,\"pid\":" << pid << "}" << std::endl;
  if (mode == "hold") {
    cardio::promise_source<void> never;
    co_await never.get_promise();
  }
}

static cardio::promise<void> Run(int argc, wchar_t** argv, int& code) {
  try {
    if (argc == 3 && std::wstring(argv[1]) == L"wait") co_await Wait(std::stoul(argv[2]));
    else if (argc == 6) co_await Launch({
        muon_internal::FilesystemPathToUtf8(argv[2]), muon_internal::FilesystemPathToUtf8(argv[3]),
        muon_internal::FilesystemPathToUtf8(argv[4]), muon_internal::FilesystemPathToUtf8(argv[5])},
        muon_internal::FilesystemPathToUtf8(argv[1]));
    else throw std::runtime_error("Invalid process driver arguments");
  } catch (const std::exception& error) {
    std::cerr << error.what() << std::endl;
    code = 1;
  }
}

int wmain(int argc, wchar_t** argv) {
  int code = 0;
  cardio::dispatcher_host_win32_auto dispatcher;
  cardio::fire_and_forget(Run(argc, argv, code));
  dispatcher.park();
  return code;
}
