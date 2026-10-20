/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "notifications/muon_notification_windows_async.h"

#include <windows.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace muon_internal {

struct WindowsNotificationMetadataTask {
  std::function<void()> work;
  cardio::promise_source<std::string> completion;
};

static void CALLBACK CompleteMetadata(PTP_CALLBACK_INSTANCE, void* context) {
  auto task = std::unique_ptr<WindowsNotificationMetadataTask>(
      static_cast<WindowsNotificationMetadataTask*>(context));
  auto error = std::string{};
  try { task->work(); }
  catch (const std::exception& exception) { error = exception.what(); }
  catch (...) { error = "Windows notification metadata operation failed"; }
  // Pass text rather than an exception object between the OS pool and the
  // application dispatcher. The callback owns the task through completion.
  task->completion.resolve(std::move(error));
}

cardio::promise<void> RunMuonWindowsNotificationTask(std::function<void()> work) {
  auto task = std::make_unique<WindowsNotificationMetadataTask>();
  task->work = std::move(work);
  auto result = task->completion.get_promise();
  // Win32 has no OVERLAPPED open, mkdir, flush, or rename API. Use its existing
  // default pool for these calls, without creating a per-operation thread or
  // parking a worker while a separate asynchronous operation finishes.
  if (!TrySubmitThreadpoolCallback(&CompleteMetadata, task.get(), nullptr))
    throw std::runtime_error("Submit notification metadata: Windows error " + std::to_string(GetLastError()));
  task.release();
  auto error = co_await result;
  if (!error.empty()) throw std::runtime_error(std::move(error));
}

}  // namespace muon_internal
