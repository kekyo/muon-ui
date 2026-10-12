/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace muon_internal {

/** Fixed control descriptor inherited by muon-executor-supervisor. */
inline constexpr int kMuonExecutorSupervisorControlFd = 3;
/** Fixed target stdin descriptor inherited by muon-executor-supervisor. */
inline constexpr int kMuonExecutorSupervisorTargetStdinFd = 4;
/** Fixed target stdout descriptor inherited by muon-executor-supervisor. */
inline constexpr int kMuonExecutorSupervisorTargetStdoutFd = 5;
/** Fixed target stderr descriptor inherited by muon-executor-supervisor. */
inline constexpr int kMuonExecutorSupervisorTargetStderrFd = 6;

/**
 * Identifies a frame exchanged with muon-executor-supervisor.
 */
enum class MuonExecutorSupervisorMessageType : uint16_t {
  /** Parent-to-supervisor target launch configuration. */
  kConfig = 1,
  /** Supervisor-to-parent target PID and process group announcement. */
  kReady = 2,
  /** Parent-to-supervisor explicit tree termination request. */
  kKill = 3,
  /** Supervisor-to-parent request acknowledgement. */
  kAck = 4,
  /** Supervisor-to-parent root target exit notification. */
  kExit = 5,
  /** Bidirectional protocol or launch failure notification. */
  kError = 6,
};

/**
 * Describes how the supervisor starts and owns a target process tree.
 */
struct MuonExecutorSupervisorConfig {
  /** Target executable name or path passed to execvp. */
  std::string command;
  /** Target arguments excluding argv[0]. */
  std::vector<std::string> arguments;
  /** Target working directory when has_cwd is true. */
  std::string cwd;
  /** Whether cwd must be applied before execvp. */
  bool has_cwd = false;
  /** Complete KEY=VALUE environment passed to the target. */
  std::vector<std::string> environment;
  /** Whether environment replaces the inherited environment. */
  bool has_environment = false;
  /** Whether control-channel EOF detaches instead of terminating the tree. */
  bool daemon = false;
};

/**
 * Represents one decoded supervisor protocol frame.
 */
struct MuonExecutorSupervisorMessage {
  /** Frame kind. */
  MuonExecutorSupervisorMessageType type =
      MuonExecutorSupervisorMessageType::kError;
  /** Type-specific primary signed value. */
  int32_t value = 0;
  /** Type-specific secondary signed value. */
  int32_t secondary_value = 0;
  /** CONFIG data, populated only for kConfig. */
  MuonExecutorSupervisorConfig config;
  /** Human-readable ERROR detail, populated only for kError. */
  std::string text;
};

/**
 * Reports the outcome of receiving a protocol frame.
 */
enum class MuonExecutorSupervisorReceiveResult {
  /** A complete valid message was decoded. */
  kMessage,
  /** The peer closed the channel before another frame began. */
  kClosed,
  /** I/O failed or a malformed/truncated frame was received. */
  kError,
};

/**
 * Writes a CONFIG frame.
 *
 * @param fd Connected supervisor control socket.
 * @param config Target launch configuration.
 * @param error_message Receives a diagnostic when false is returned.
 * @return true when the complete frame was written.
 */
bool SendMuonExecutorSupervisorConfig(
    int fd,
    const MuonExecutorSupervisorConfig& config,
    std::string* error_message);

/**
 * Writes a KILL frame.
 *
 * @param fd Connected supervisor control socket.
 * @param error_message Receives a diagnostic when false is returned.
 * @return true when the complete frame was written.
 */
bool SendMuonExecutorSupervisorKill(int fd, std::string* error_message);

/**
 * Writes a READY frame.
 *
 * @param fd Connected supervisor control socket.
 * @param process_id Root target process ID.
 * @param process_group_id Target process group ID.
 * @param error_message Receives a diagnostic when false is returned.
 * @return true when the complete frame was written.
 */
bool SendMuonExecutorSupervisorReady(
    int fd,
    int32_t process_id,
    int32_t process_group_id,
    std::string* error_message);

/**
 * Writes an acknowledgement frame for a request type.
 *
 * @param fd Connected supervisor control socket.
 * @param acknowledged_type Request type being acknowledged.
 * @param error_message Receives a diagnostic when false is returned.
 * @return true when the complete frame was written.
 */
bool SendMuonExecutorSupervisorAck(
    int fd,
    MuonExecutorSupervisorMessageType acknowledged_type,
    std::string* error_message);

/**
 * Writes an EXIT frame.
 *
 * @param fd Connected supervisor control socket.
 * @param exit_code Root target exit code, using 128 + signal for signals.
 * @param error_message Receives a diagnostic when false is returned.
 * @return true when the complete frame was written.
 */
bool SendMuonExecutorSupervisorExit(
    int fd,
    int32_t exit_code,
    std::string* error_message);

/**
 * Writes an ERROR frame.
 *
 * @param fd Connected supervisor control socket.
 * @param error_number Platform errno associated with the failure, or zero.
 * @param message Human-readable failure detail.
 * @param error_message Receives a diagnostic if the ERROR frame cannot be sent.
 * @return true when the complete frame was written.
 */
bool SendMuonExecutorSupervisorError(
    int fd,
    int32_t error_number,
    const std::string& message,
    std::string* error_message);

/**
 * Receives and decodes one complete protocol frame.
 *
 * @remarks This function blocks until one frame is complete, the channel is
 * closed, or an error occurs. Call it only after the descriptor becomes
 * readable when integrating it into an event loop.
 *
 * @param fd Connected supervisor control socket.
 * @param message Receives the decoded frame when kMessage is returned.
 * @param error_message Receives a diagnostic when kError is returned.
 * @return Receive outcome.
 */
MuonExecutorSupervisorReceiveResult ReceiveMuonExecutorSupervisorMessage(
    int fd,
    MuonExecutorSupervisorMessage* message,
    std::string* error_message);

}  // namespace muon_internal
