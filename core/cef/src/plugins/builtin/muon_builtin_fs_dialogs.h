/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace cardio {
class dispatcher;
}

namespace muon_internal {

/**
 * Native filesystem dialog operation type.
 */
enum class MuonFsDialogKind {
  SelectFile,
  SelectFiles,
  SelectDirectory,
  SelectDirectories,
  SelectSaveFile,
};

#if defined(_WIN32)
/**
 * Shared cancellation state for an active native filesystem dialog.
 */
class MuonFsNativeDialogCancellation final {
 public:
  MuonFsNativeDialogCancellation() = default;
  ~MuonFsNativeDialogCancellation() = default;

  MuonFsNativeDialogCancellation(
      const MuonFsNativeDialogCancellation&) = delete;
  MuonFsNativeDialogCancellation& operator=(
      const MuonFsNativeDialogCancellation&) = delete;

  /**
   * Requests cancellation of the currently attached native dialog.
   */
  void Cancel();

  /**
   * Returns true after cancellation has been requested.
   */
  bool IsCanceled() const;

  /**
   * Attaches a platform-native dialog pointer while it is active.
   *
   * @param native_dialog IFileDialog* on Win32.
   * @param owner_window_handle Native owner window handle on Win32, or 0.
   * @return false when cancellation was already requested.
   */
  bool AttachNativeDialog(void* native_dialog,
                          std::uintptr_t owner_window_handle);

  /**
   * Detaches the active native dialog pointer before it is destroyed.
   */
  void DetachNativeDialog(void* native_dialog);

 private:
  bool canceled_ = false;
  void* native_dialog_ = nullptr;
  std::uintptr_t owner_window_handle_ = 0;
  cardio::dispatcher* native_dialog_dispatcher_ = nullptr;
  mutable std::mutex mutex_;
};

#endif

}  // namespace muon_internal
