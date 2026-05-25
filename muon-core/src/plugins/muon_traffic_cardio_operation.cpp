/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/muon_traffic_cardio_operation.h"

#include <stdexcept>
#include <utility>

namespace muon_internal {

MuonTrafficCardioCancellation::MuonTrafficCardioCancellation() = default;

MuonTrafficCardioCancellation::~MuonTrafficCardioCancellation() = default;

cardio::cancellation MuonTrafficCardioCancellation::GetCancellation()
    const noexcept {
  return cancellation_source_.get_cancellation();
}

void MuonTrafficCardioCancellation::Cancel(std::string error_message) {
  CancelImpl(std::move(error_message), false);
}

void MuonTrafficCardioCancellation::ForceCancel(std::string error_message) {
  CancelImpl(std::move(error_message), true);
}

std::string MuonTrafficCardioCancellation::CancellationMessage(
    std::string fallback) const {
  return cancellation_error_.empty() ? std::move(fallback)
                                     : cancellation_error_;
}

void MuonTrafficCardioCancellation::CancelImpl(
    std::string error_message,
    bool overwrite) {
  if (overwrite || cancellation_error_.empty()) {
    cancellation_error_ = std::move(error_message);
  }
  (void)cancellation_source_.cancel();
}

std::string MuonTrafficCardioOperationExceptionMessage(
    const std::shared_ptr<MuonTrafficCardioCancellation>& cancellation,
    std::string generic_error,
    std::exception_ptr exception) {
  try {
    if (exception) {
      std::rethrow_exception(exception);
    }
  } catch (const cardio::canceled_exception&) {
    return cancellation ? cancellation->CancellationMessage(
                              std::move(generic_error))
                        : std::move(generic_error);
  } catch (const std::exception& error) {
    const auto* message = error.what();
    return message == nullptr || message[0] == '\0' ? std::move(generic_error)
                                                    : message;
  } catch (...) {
  }
  return generic_error;
}

}  // namespace muon_internal
