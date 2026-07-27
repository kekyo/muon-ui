/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "process/muon_executor_supervisor_protocol.h"

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace muon_internal {

static constexpr uint32_t kProtocolMagic = 0x4d584553;
static constexpr uint16_t kProtocolVersion = 1;
static constexpr size_t kFrameHeaderSize = 12;
static constexpr uint32_t kMaximumPayloadSize = 16 * 1024 * 1024;
static constexpr uint32_t kConfigFlagDaemon = 1U << 0;
static constexpr uint32_t kConfigFlagHasCwd = 1U << 1;
static constexpr uint32_t kConfigFlagHasEnvironment = 1U << 2;

static void AppendUint16(std::vector<uint8_t>* output, uint16_t value) {
  output->push_back(static_cast<uint8_t>(value & 0xffU));
  output->push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
}

static void AppendUint32(std::vector<uint8_t>* output, uint32_t value) {
  output->push_back(static_cast<uint8_t>(value & 0xffU));
  output->push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
  output->push_back(static_cast<uint8_t>((value >> 16U) & 0xffU));
  output->push_back(static_cast<uint8_t>((value >> 24U) & 0xffU));
}

static bool AppendString(
    std::vector<uint8_t>* output,
    const std::string& value,
    std::string* error_message) {
  if (value.find('\0') != std::string::npos) {
    *error_message = "Supervisor protocol strings cannot contain NUL";
    return false;
  }
  if (value.size() > std::numeric_limits<uint32_t>::max()) {
    *error_message = "Supervisor protocol string is too large";
    return false;
  }
  AppendUint32(output, static_cast<uint32_t>(value.size()));
  output->insert(output->end(), value.begin(), value.end());
  return true;
}

static bool ReadUint16(
    const std::vector<uint8_t>& input,
    size_t* offset,
    uint16_t* value) {
  if (*offset > input.size() || input.size() - *offset < 2) {
    return false;
  }
  *value = static_cast<uint16_t>(input[*offset]) |
           (static_cast<uint16_t>(input[*offset + 1]) << 8U);
  *offset += 2;
  return true;
}

static bool ReadUint32(
    const std::vector<uint8_t>& input,
    size_t* offset,
    uint32_t* value) {
  if (*offset > input.size() || input.size() - *offset < 4) {
    return false;
  }
  *value = static_cast<uint32_t>(input[*offset]) |
           (static_cast<uint32_t>(input[*offset + 1]) << 8U) |
           (static_cast<uint32_t>(input[*offset + 2]) << 16U) |
           (static_cast<uint32_t>(input[*offset + 3]) << 24U);
  *offset += 4;
  return true;
}

static bool ReadString(
    const std::vector<uint8_t>& input,
    size_t* offset,
    std::string* value) {
  uint32_t length = 0;
  if (!ReadUint32(input, offset, &length) || *offset > input.size() ||
      input.size() - *offset < length) {
    return false;
  }
  value->assign(
      reinterpret_cast<const char*>(input.data() + *offset),
      static_cast<size_t>(length));
  *offset += length;
  return value->find('\0') == std::string::npos;
}

static bool WriteAll(
    int fd,
    const uint8_t* data,
    size_t size,
    std::string* error_message) {
  auto offset = size_t{0};
  while (offset < size) {
    const auto written =
        send(fd, data + offset, size - offset, MSG_NOSIGNAL);
    if (written > 0) {
      offset += static_cast<size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) {
      continue;
    }
    const auto error_number = written == 0 ? EPIPE : errno;
    *error_message =
        "Failed to write supervisor control channel: " +
        std::string(std::strerror(error_number));
    return false;
  }
  return true;
}

static MuonExecutorSupervisorReceiveResult ReadAll(
    int fd,
    uint8_t* data,
    size_t size,
    bool frame_started,
    std::string* error_message) {
  auto offset = size_t{0};
  while (offset < size) {
    const auto received = recv(fd, data + offset, size - offset, 0);
    if (received > 0) {
      frame_started = true;
      offset += static_cast<size_t>(received);
      continue;
    }
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received == 0 && !frame_started) {
      return MuonExecutorSupervisorReceiveResult::kClosed;
    }
    if (received == 0) {
      *error_message = "Supervisor control channel closed mid-frame";
    } else {
      *error_message =
          "Failed to read supervisor control channel: " +
          std::string(std::strerror(errno));
    }
    return MuonExecutorSupervisorReceiveResult::kError;
  }
  return MuonExecutorSupervisorReceiveResult::kMessage;
}

static bool SendFrame(
    int fd,
    MuonExecutorSupervisorMessageType type,
    const std::vector<uint8_t>& payload,
    std::string* error_message) {
  if (payload.size() > kMaximumPayloadSize) {
    *error_message = "Supervisor protocol payload is too large";
    return false;
  }
  std::vector<uint8_t> frame;
  frame.reserve(kFrameHeaderSize + payload.size());
  AppendUint32(&frame, kProtocolMagic);
  AppendUint16(&frame, kProtocolVersion);
  AppendUint16(&frame, static_cast<uint16_t>(type));
  AppendUint32(&frame, static_cast<uint32_t>(payload.size()));
  frame.insert(frame.end(), payload.begin(), payload.end());
  return WriteAll(fd, frame.data(), frame.size(), error_message);
}

static bool DecodeConfig(
    const std::vector<uint8_t>& payload,
    MuonExecutorSupervisorConfig* config,
    std::string* error_message) {
  auto offset = size_t{0};
  uint32_t flags = 0;
  uint32_t argument_count = 0;
  uint32_t environment_count = 0;
  if (!ReadUint32(payload, &offset, &flags) ||
      !ReadString(payload, &offset, &config->command) ||
      !ReadString(payload, &offset, &config->cwd) ||
      !ReadUint32(payload, &offset, &argument_count)) {
    *error_message = "Malformed supervisor CONFIG frame";
    return false;
  }
  if ((flags & ~(kConfigFlagDaemon | kConfigFlagHasCwd |
                 kConfigFlagHasEnvironment)) != 0) {
    *error_message = "Supervisor CONFIG frame contains unknown flags";
    return false;
  }
  config->daemon = (flags & kConfigFlagDaemon) != 0;
  config->has_cwd = (flags & kConfigFlagHasCwd) != 0;
  config->has_environment = (flags & kConfigFlagHasEnvironment) != 0;
  config->arguments.clear();
  if (argument_count > payload.size() / sizeof(uint32_t)) {
    *error_message = "Malformed supervisor CONFIG argument count";
    return false;
  }
  config->arguments.reserve(argument_count);
  for (auto index = uint32_t{0}; index < argument_count; ++index) {
    std::string argument;
    if (!ReadString(payload, &offset, &argument)) {
      *error_message = "Malformed supervisor CONFIG argument";
      return false;
    }
    config->arguments.push_back(std::move(argument));
  }
  if (!ReadUint32(payload, &offset, &environment_count) ||
      environment_count > payload.size() / sizeof(uint32_t)) {
    *error_message = "Malformed supervisor CONFIG environment count";
    return false;
  }
  config->environment.clear();
  config->environment.reserve(environment_count);
  for (auto index = uint32_t{0}; index < environment_count; ++index) {
    std::string entry;
    if (!ReadString(payload, &offset, &entry)) {
      *error_message = "Malformed supervisor CONFIG environment entry";
      return false;
    }
    config->environment.push_back(std::move(entry));
  }
  if (offset != payload.size() || config->command.empty()) {
    *error_message = "Malformed supervisor CONFIG frame";
    return false;
  }
  return true;
}

bool SendMuonExecutorSupervisorConfig(
    int fd,
    const MuonExecutorSupervisorConfig& config,
    std::string* error_message) {
  std::vector<uint8_t> payload;
  auto flags = uint32_t{0};
  if (config.daemon) {
    flags |= kConfigFlagDaemon;
  }
  if (config.has_cwd) {
    flags |= kConfigFlagHasCwd;
  }
  if (config.has_environment) {
    flags |= kConfigFlagHasEnvironment;
  }
  AppendUint32(&payload, flags);
  if (!AppendString(&payload, config.command, error_message) ||
      !AppendString(&payload, config.cwd, error_message)) {
    return false;
  }
  if (config.arguments.size() > std::numeric_limits<uint32_t>::max()) {
    *error_message = "Too many supervisor target arguments";
    return false;
  }
  AppendUint32(&payload, static_cast<uint32_t>(config.arguments.size()));
  for (const auto& argument : config.arguments) {
    if (!AppendString(&payload, argument, error_message)) {
      return false;
    }
  }
  if (config.environment.size() > std::numeric_limits<uint32_t>::max()) {
    *error_message = "Supervisor target environment is too large";
    return false;
  }
  AppendUint32(&payload, static_cast<uint32_t>(config.environment.size()));
  for (const auto& entry : config.environment) {
    if (!AppendString(&payload, entry, error_message)) {
      return false;
    }
  }
  return SendFrame(
      fd, MuonExecutorSupervisorMessageType::kConfig, payload, error_message);
}

bool SendMuonExecutorSupervisorKill(int fd, std::string* error_message) {
  return SendFrame(
      fd, MuonExecutorSupervisorMessageType::kKill, {}, error_message);
}

bool SendMuonExecutorSupervisorReady(
    int fd,
    int32_t process_id,
    int32_t process_group_id,
    std::string* error_message) {
  std::vector<uint8_t> payload;
  AppendUint32(&payload, static_cast<uint32_t>(process_id));
  AppendUint32(&payload, static_cast<uint32_t>(process_group_id));
  return SendFrame(
      fd, MuonExecutorSupervisorMessageType::kReady, payload, error_message);
}

bool SendMuonExecutorSupervisorAck(
    int fd,
    MuonExecutorSupervisorMessageType acknowledged_type,
    std::string* error_message) {
  std::vector<uint8_t> payload;
  AppendUint32(&payload, static_cast<uint32_t>(acknowledged_type));
  return SendFrame(
      fd, MuonExecutorSupervisorMessageType::kAck, payload, error_message);
}

bool SendMuonExecutorSupervisorExit(
    int fd,
    int32_t exit_code,
    std::string* error_message) {
  std::vector<uint8_t> payload;
  AppendUint32(&payload, static_cast<uint32_t>(exit_code));
  return SendFrame(
      fd, MuonExecutorSupervisorMessageType::kExit, payload, error_message);
}

bool SendMuonExecutorSupervisorError(
    int fd,
    int32_t error_number,
    const std::string& message,
    std::string* error_message) {
  std::vector<uint8_t> payload;
  AppendUint32(&payload, static_cast<uint32_t>(error_number));
  if (!AppendString(&payload, message, error_message)) {
    return false;
  }
  return SendFrame(
      fd, MuonExecutorSupervisorMessageType::kError, payload, error_message);
}

MuonExecutorSupervisorReceiveResult ReceiveMuonExecutorSupervisorMessage(
    int fd,
    MuonExecutorSupervisorMessage* message,
    std::string* error_message) {
  std::array<uint8_t, kFrameHeaderSize> header = {};
  auto result =
      ReadAll(fd, header.data(), header.size(), false, error_message);
  if (result != MuonExecutorSupervisorReceiveResult::kMessage) {
    return result;
  }
  std::vector<uint8_t> header_data(header.begin(), header.end());
  auto offset = size_t{0};
  uint32_t magic = 0;
  uint16_t version = 0;
  uint16_t type_value = 0;
  uint32_t payload_size = 0;
  if (!ReadUint32(header_data, &offset, &magic) ||
      !ReadUint16(header_data, &offset, &version) ||
      !ReadUint16(header_data, &offset, &type_value) ||
      !ReadUint32(header_data, &offset, &payload_size) ||
      magic != kProtocolMagic || version != kProtocolVersion ||
      payload_size > kMaximumPayloadSize) {
    *error_message = "Malformed supervisor protocol frame header";
    return MuonExecutorSupervisorReceiveResult::kError;
  }
  if (type_value <
          static_cast<uint16_t>(MuonExecutorSupervisorMessageType::kConfig) ||
      type_value >
          static_cast<uint16_t>(MuonExecutorSupervisorMessageType::kError)) {
    *error_message = "Unknown supervisor protocol frame type";
    return MuonExecutorSupervisorReceiveResult::kError;
  }

  std::vector<uint8_t> payload(payload_size);
  result = ReadAll(
      fd, payload.data(), payload.size(), true, error_message);
  if (result != MuonExecutorSupervisorReceiveResult::kMessage) {
    return result;
  }

  *message = {};
  message->type =
      static_cast<MuonExecutorSupervisorMessageType>(type_value);
  offset = 0;
  uint32_t value = 0;
  uint32_t secondary_value = 0;
  switch (message->type) {
    case MuonExecutorSupervisorMessageType::kConfig:
      if (!DecodeConfig(payload, &message->config, error_message)) {
        return MuonExecutorSupervisorReceiveResult::kError;
      }
      break;
    case MuonExecutorSupervisorMessageType::kReady:
      if (!ReadUint32(payload, &offset, &value) ||
          !ReadUint32(payload, &offset, &secondary_value) ||
          offset != payload.size()) {
        *error_message = "Malformed supervisor READY frame";
        return MuonExecutorSupervisorReceiveResult::kError;
      }
      message->value = static_cast<int32_t>(value);
      message->secondary_value = static_cast<int32_t>(secondary_value);
      break;
    case MuonExecutorSupervisorMessageType::kKill:
      if (!payload.empty()) {
        *error_message = "Malformed supervisor KILL frame";
        return MuonExecutorSupervisorReceiveResult::kError;
      }
      break;
    case MuonExecutorSupervisorMessageType::kAck:
    case MuonExecutorSupervisorMessageType::kExit:
      if (!ReadUint32(payload, &offset, &value) ||
          offset != payload.size()) {
        *error_message = "Malformed supervisor scalar frame";
        return MuonExecutorSupervisorReceiveResult::kError;
      }
      message->value = static_cast<int32_t>(value);
      break;
    case MuonExecutorSupervisorMessageType::kError:
      if (!ReadUint32(payload, &offset, &value) ||
          !ReadString(payload, &offset, &message->text) ||
          offset != payload.size()) {
        *error_message = "Malformed supervisor ERROR frame";
        return MuonExecutorSupervisorReceiveResult::kError;
      }
      message->value = static_cast<int32_t>(value);
      break;
  }
  return MuonExecutorSupervisorReceiveResult::kMessage;
}

}  // namespace muon_internal
