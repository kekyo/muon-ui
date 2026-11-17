/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "services/muon_service_lifecycle.h"

#include <charconv>
#include <iomanip>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

struct Driver {
  MuonServiceLifecycle manager{std::vector<MuonServiceDefinition>{
      {"alpha", false, std::nullopt}, {"beta", false, std::nullopt}}};
  std::map<uint64_t, MuonServiceReference> references;
  std::map<std::string, uint64_t> pending_saves;
};

static const char* StateName(MuonServiceState state) {
  switch (state) {
    case MuonServiceState::Stopped: return "stopped";
    case MuonServiceState::Starting: return "starting";
    case MuonServiceState::Running: return "running";
    case MuonServiceState::Stopping: return "stopping";
    case MuonServiceState::Failed: return "failed";
  }
  return "unknown";
}

static const char* EventName(MuonServiceEventKind kind) {
  switch (kind) {
    case MuonServiceEventKind::Start: return "start";
    case MuonServiceEventKind::Stop: return "stop";
    case MuonServiceEventKind::SaveAutostart: return "save-autostart";
    case MuonServiceEventKind::Acquired: return "acquired";
    case MuonServiceEventKind::AcquireFailed: return "acquire-failed";
    case MuonServiceEventKind::Invalidated: return "invalidated";
    case MuonServiceEventKind::AutostartResult: return "autostart-result";
    case MuonServiceEventKind::Changed: return "changed";
  }
  return "unknown";
}

static void PrintSnapshot(const MuonServiceSnapshot& snapshot) {
  std::cout << " service=" << std::quoted(snapshot.id)
            << " state=" << StateName(snapshot.state)
            << " generation=" << snapshot.generation
            << " revision=" << snapshot.revision
            << " autostart=" << snapshot.autostart
            << " resident=" << snapshot.resident
            << " clients=" << snapshot.client_references;
  if (!snapshot.error.empty())
    std::cout << " runtime-error=" << std::quoted(snapshot.error);
}

static void PrintReference(const MuonServiceReference& reference) {
  std::cout << " reference=" << reference.id
            << " owner=" << std::quoted(reference.owner_id)
            << " reference-generation=" << reference.generation;
}

static void DrainEvents(Driver& driver) {
  for (const auto& event : driver.manager.TakeEvents()) {
    if (event.kind == MuonServiceEventKind::SaveAutostart) {
      driver.pending_saves[event.snapshot.id] = event.operation_id;
    } else if (event.kind == MuonServiceEventKind::AutostartResult) {
      const auto pending = driver.pending_saves.find(event.snapshot.id);
      if (pending != driver.pending_saves.end() &&
          pending->second == event.operation_id)
        driver.pending_saves.erase(pending);
    }
    std::cout << "event=" << EventName(event.kind);
    PrintSnapshot(event.snapshot);
    if (event.reference.has_value()) PrintReference(*event.reference);
    if (event.operation_id != 0)
      std::cout << " operation=" << event.operation_id
                << " requested-autostart=" << event.autostart;
    if (event.kind == MuonServiceEventKind::AutostartResult)
      std::cout << " success=" << event.success;
    if (!event.error.empty())
      std::cout << " error=" << std::quoted(event.error);
    std::cout << '\n';
  }
}

static std::string Argument(int argc, char** argv, int& index,
                            const std::string& label) {
  if (index == argc) throw std::invalid_argument("Missing " + label);
  return argv[index++];
}

static uint64_t Identity(const std::string& text) {
  uint64_t result = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(),
                                    result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
      result == 0)
    throw std::invalid_argument("Expected a nonzero identity: " + text);
  return result;
}

static uint64_t CurrentGeneration(const Driver& driver,
                                  const std::string& service) {
  const auto snapshot = driver.manager.GetSnapshot(service);
  if (!snapshot.has_value() || snapshot->generation == 0)
    throw std::invalid_argument("No runtime generation for service: " + service);
  return snapshot->generation;
}

static const MuonServiceReference& Reference(const Driver& driver, uint64_t id) {
  const auto reference = driver.references.find(id);
  if (reference == driver.references.end())
    throw std::invalid_argument("Unknown reference: " + std::to_string(id));
  return reference->second;
}

static void PrintStatus(const Driver& driver) {
  for (const auto* service : {"alpha", "beta"}) {
    std::cout << "status";
    PrintSnapshot(driver.manager.GetSnapshot(service).value());
    std::cout << '\n';
  }
  std::cout << "host shutting-down=" << driver.manager.IsShuttingDown()
            << " quiescent=" << driver.manager.IsQuiescent() << '\n';
}

static void Usage() {
  std::cout
      << "Usage: service-driver COMMAND...\n"
      << "Services: alpha and beta, both initially OFF.\n"
      << "Commands are processed in argv order; quote diagnostics with spaces.\n"
      << "  acquire SERVICE OWNER       Reserve a reference.\n"
      << "  release REFERENCE           Release an issued reference.\n"
      << "  ready REFERENCE             Check whether it is callable.\n"
      << "  owner-gone OWNER            Recover all of that owner's references.\n"
      << "  autostart SERVICE on|off    Queue a persistent setting change.\n"
      << "  saved SERVICE OPERATION     Complete a durable save successfully.\n"
      << "  save-failed SERVICE OPERATION ERROR\n"
      << "  started SERVICE             Complete the current start.\n"
      << "  start-failed SERVICE ERROR  Complete the current start with failure.\n"
      << "  stopped SERVICE             Complete the current stop.\n"
      << "  exited SERVICE ERROR        Report disposal after an unexpected exit.\n"
      << "  started-at SERVICE GENERATION\n"
      << "  start-failed-at SERVICE GENERATION ERROR\n"
      << "  stopped-at SERVICE GENERATION\n"
      << "  exited-at SERVICE GENERATION ERROR\n"
      << "  shutdown REASON             Stop the entire host.\n"
      << "  status                      Observe both services and the host.\n"
      << "Explicit generations allow stale completions to be replayed.\n"
      << "On exit, cleanup cancels pending fake starts/saves and completes stops.\n"
      << "Example:\n"
      << "  service-driver acquire alpha page-a started alpha "
         "autostart alpha on saved alpha 1 release 1 status\n";
}

static void RunCommand(Driver& driver, int argc, char** argv, int& index) {
  const auto command = Argument(argc, argv, index, "command");
  std::cout << "command=" << command << '\n';
  if (command == "acquire") {
    const auto service = Argument(argc, argv, index, "service");
    const auto owner = Argument(argc, argv, index, "owner");
    const auto reference = driver.manager.Acquire(service, owner);
    std::cout << "reservation accepted=" << reference.has_value();
    if (reference.has_value()) {
      driver.references.emplace(reference->id, *reference);
      std::cout << " service=" << std::quoted(reference->service_id);
      PrintReference(*reference);
    }
    std::cout << '\n';
  } else if (command == "release" || command == "ready") {
    const auto id = Identity(Argument(argc, argv, index, "reference"));
    const auto& reference = Reference(driver, id);
    const auto result = command == "release"
                            ? driver.manager.Release(reference)
                            : driver.manager.IsReady(reference);
    // Keep issued identities so duplicate releases remain observable.
    std::cout << command << " reference=" << id << " result=" << result << '\n';
  } else if (command == "owner-gone") {
    const auto owner = Argument(argc, argv, index, "owner");
    const auto recovered = driver.manager.ReleaseOwner(owner);
    std::cout << "owner-gone owner=" << std::quoted(owner)
              << " recovered=" << recovered << '\n';
  } else if (command == "autostart") {
    const auto service = Argument(argc, argv, index, "service");
    const auto value = Argument(argc, argv, index, "on or off");
    if (value != "on" && value != "off")
      throw std::invalid_argument("Expected on or off: " + value);
    const auto operation = driver.manager.SetAutostart(service, value == "on");
    std::cout << "setting service=" << std::quoted(service)
              << " operation=" << operation << '\n';
  } else if (command == "saved" || command == "save-failed") {
    const auto service = Argument(argc, argv, index, "service");
    const auto operation = Identity(Argument(argc, argv, index, "operation"));
    const auto error = command == "saved"
                           ? std::string{}
                           : Argument(argc, argv, index, "error");
    driver.manager.CompleteAutostart(service, operation, error);
  } else if (command == "shutdown") {
    driver.manager.Shutdown(Argument(argc, argv, index, "reason"));
  } else if (command == "status") {
    PrintStatus(driver);
  } else if (command == "started" || command == "started-at" ||
             command == "start-failed" || command == "start-failed-at" ||
             command == "stopped" || command == "stopped-at" ||
             command == "exited" || command == "exited-at") {
    const auto service = Argument(argc, argv, index, "service");
    const auto generation = command.ends_with("-at")
                                ? Identity(Argument(argc, argv, index,
                                                    "generation"))
                                : CurrentGeneration(driver, service);
    if (command.starts_with("started")) {
      driver.manager.CompleteStart(service, generation, "");
    } else if (command.starts_with("stopped")) {
      driver.manager.CompleteStop(service, generation);
    } else {
      const auto error = Argument(argc, argv, index, "error");
      if (command.starts_with("start-failed"))
        driver.manager.CompleteStart(service, generation, error);
      else
        driver.manager.RuntimeExited(service, generation, error);
    }
  } else {
    throw std::invalid_argument("Unknown command: " + command);
  }
  DrainEvents(driver);
}

static void Finish(Driver& driver) {
  std::cout << "cleanup=begin\n";
  driver.manager.Shutdown("driver exiting");
  DrainEvents(driver);
  // No real runtime or durable store exists in this driver. Settle only the
  // outstanding fake operations so destruction respects the host contract.
  while (!driver.manager.IsQuiescent()) {
    auto progressed = false;
    for (const auto* service : {"alpha", "beta"}) {
      const auto snapshot = driver.manager.GetSnapshot(service).value();
      if (snapshot.state == MuonServiceState::Starting) {
        driver.manager.CompleteStart(service, snapshot.generation,
                                     "driver exiting before start completed");
        progressed = true;
      } else if (snapshot.state == MuonServiceState::Stopping) {
        driver.manager.CompleteStop(service, snapshot.generation);
        progressed = true;
      }
      DrainEvents(driver);
    }
    const auto saves = driver.pending_saves;
    for (const auto& [service, operation] : saves) {
      driver.manager.CompleteAutostart(
          service, operation, "driver exiting before save completed");
      progressed = true;
      DrainEvents(driver);
    }
    if (!progressed)
      throw std::logic_error("Lifecycle retained work without an outstanding action");
  }
  std::cout << "cleanup=complete quiescent=true\n";
}

int main(int argc, char** argv) {
  std::cout << std::boolalpha;
  if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
    Usage();
    return 0;
  }
  auto driver = Driver{};
  auto result = 0;
  try {
    DrainEvents(driver);
    auto index = 1;
    while (index < argc) RunCommand(driver, argc, argv, index);
    PrintStatus(driver);
  } catch (const std::exception& error) {
    std::cerr << "service-driver: " << error.what() << '\n';
    result = 1;
  }
  try {
    Finish(driver);
  } catch (const std::exception& error) {
    std::cerr << "service-driver cleanup: " << error.what() << '\n';
    return 1;
  }
  return result;
}
