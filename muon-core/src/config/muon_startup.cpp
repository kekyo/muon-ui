/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "config/muon_startup.h"

#include <cstring>

namespace muon_internal {

std::vector<std::string> g_muon_startup_command_line;
std::string g_muon_startup_launch_source = kMuonLaunchSourceNone;

}  // namespace muon_internal

void SetMuonStartupCommandLine(int argc, char* argv[]) {
  std::vector<std::string> command_line;
  command_line.reserve(argc > 0 ? static_cast<size_t>(argc) : 0);
  for (auto index = 0; index < argc; ++index) {
    command_line.emplace_back(argv[index] == nullptr ? "" : argv[index]);
  }

  muon_internal::g_muon_startup_command_line = std::move(command_line);
  muon_internal::g_muon_startup_launch_source =
      GetMuonLaunchSourceFromCommandLine(
          muon_internal::g_muon_startup_command_line);
}

std::string GetMuonLaunchSourceFromCommandLine(
    const std::vector<std::string>& command_line) {
  std::string launch_source = kMuonLaunchSourceNone;
  for (auto index = size_t{1}; index < command_line.size(); ++index) {
    if (command_line[index].starts_with(kMuonLaunchSourceSwitchPrefix)) {
      launch_source =
          command_line[index].substr(std::strlen(kMuonLaunchSourceSwitchPrefix));
    }
  }
  return launch_source;
}

std::vector<std::string> GetMuonStartupCommandLine() {
  return muon_internal::g_muon_startup_command_line;
}

std::string GetMuonStartupLaunchSource() {
  return muon_internal::g_muon_startup_launch_source;
}
