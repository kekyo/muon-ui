/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/builtin/muon_builtin_fs_dialogs.h"

#include "muon_cardio_post.h"

#include "muon_json_helpers.h"
#include "plugins/builtin/muon_builtin_fs_dialogs_plugin.h"
#include "plugins/builtin/muon_builtin_fs_helpers.h"
#include "ui/muon_ui_fs_dialogs.h"

#include "yyjson.h"

#include <cardio.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <shobjidl.h>
#include <windows.h>
#else
#include <gtk/gtk.h>
#endif

namespace muon_internal {

#if defined(_WIN32)
static void CloseWin32NativeDialog(IFileDialog* dialog) {
  if (dialog != nullptr) {
    dialog->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
  }
}

struct MuonWin32OwnedWindowCloseState {
  HWND owner = nullptr;
};

static BOOL CALLBACK PostCloseToMuonWin32OwnedWindow(HWND window,
                                                     LPARAM raw_state) {
  auto* state =
      reinterpret_cast<MuonWin32OwnedWindowCloseState*>(raw_state);
  if (state == nullptr || state->owner == nullptr || window == state->owner) {
    return TRUE;
  }
  if (GetWindow(window, GW_OWNER) == state->owner) {
    PostMessageW(window, WM_CLOSE, 0, 0);
  }
  return TRUE;
}

static void CloseWin32OwnedDialogWindows(std::uintptr_t owner_window_handle) {
  auto* owner = reinterpret_cast<HWND>(owner_window_handle);
  if (owner == nullptr) {
    return;
  }
  auto state = MuonWin32OwnedWindowCloseState{owner};
  EnumWindows(&PostCloseToMuonWin32OwnedWindow,
              reinterpret_cast<LPARAM>(&state));
}

static void CancelNativeDialog(void* native_dialog,
                               cardio::dispatcher* dispatcher,
                               std::uintptr_t owner_window_handle) {
  if (native_dialog != nullptr) {
    auto* dialog = static_cast<IFileDialog*>(native_dialog);
    if (dispatcher == nullptr ||
        dispatcher == cardio::unsafe_get_current_dispatcher()) {
      CloseWin32NativeDialog(dialog);
      CloseWin32OwnedDialogWindows(owner_window_handle);
      return;
    }
    dialog->AddRef();
    FireAndForgetOnDispatcher(dispatcher, [dialog, owner_window_handle]() {
      CloseWin32NativeDialog(dialog);
      CloseWin32OwnedDialogWindows(owner_window_handle);
      dialog->Release();
    });
  }
}
#else
static gboolean CancelGtkDialogOnMainContext(gpointer data) {
  auto* dialog = static_cast<GtkWidget*>(data);
  if (dialog != nullptr && GTK_IS_DIALOG(dialog) &&
      !gtk_widget_in_destruction(dialog)) {
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  }
  if (dialog != nullptr) {
    g_object_unref(dialog);
  }
  return G_SOURCE_REMOVE;
}

static void CancelNativeDialog(void* native_dialog) {
  auto* dialog = static_cast<GtkWidget*>(native_dialog);
  if (dialog != nullptr) {
    g_object_ref(dialog);
    g_main_context_invoke(nullptr, CancelGtkDialogOnMainContext, dialog);
  }
}
#endif

void MuonFsNativeDialogCancellation::Cancel() {
  void* native_dialog = nullptr;
  std::uintptr_t owner_window_handle = 0;
  cardio::dispatcher* native_dialog_dispatcher = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (canceled_) {
      return;
    }
    canceled_ = true;
    native_dialog = native_dialog_;
    owner_window_handle = owner_window_handle_;
    native_dialog_dispatcher = native_dialog_dispatcher_;
  }
#if defined(_WIN32)
  CancelNativeDialog(
      native_dialog, native_dialog_dispatcher, owner_window_handle);
#else
  CancelNativeDialog(native_dialog);
#endif
}

bool MuonFsNativeDialogCancellation::IsCanceled() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return canceled_;
}

bool MuonFsNativeDialogCancellation::AttachNativeDialog(
    void* native_dialog,
    std::uintptr_t owner_window_handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (canceled_) {
    return false;
  }
  native_dialog_ = native_dialog;
  owner_window_handle_ = owner_window_handle;
#if defined(_WIN32)
  native_dialog_dispatcher_ = cardio::unsafe_get_current_dispatcher();
#endif
  return true;
}

void MuonFsNativeDialogCancellation::DetachNativeDialog(void* native_dialog) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (native_dialog_ == native_dialog) {
    native_dialog_ = nullptr;
    owner_window_handle_ = 0;
    native_dialog_dispatcher_ = nullptr;
  }
}

class MuonNativeDialogCancellationAttachment final {
 public:
  MuonNativeDialogCancellationAttachment(
      std::shared_ptr<MuonFsNativeDialogCancellation> cancellation,
      void* native_dialog,
      std::uintptr_t owner_window_handle)
      : cancellation_(std::move(cancellation)),
        native_dialog_(native_dialog) {
    attached_ = cancellation_ == nullptr ||
                cancellation_->AttachNativeDialog(
                    native_dialog_, owner_window_handle);
  }

  ~MuonNativeDialogCancellationAttachment() {
    Detach();
  }

  MuonNativeDialogCancellationAttachment(
      const MuonNativeDialogCancellationAttachment&) = delete;
  MuonNativeDialogCancellationAttachment& operator=(
      const MuonNativeDialogCancellationAttachment&) = delete;

  bool attached() const {
    return attached_;
  }

  void Detach() {
    if (attached_ && cancellation_ != nullptr) {
      cancellation_->DetachNativeDialog(native_dialog_);
    }
    attached_ = false;
    native_dialog_ = nullptr;
  }

 private:
  std::shared_ptr<MuonFsNativeDialogCancellation> cancellation_;
  void* native_dialog_ = nullptr;
  bool attached_ = false;
};

struct MuonFsDialogFilter {
  std::string name;
  std::vector<std::string> extensions;
};

struct MuonFsGtkDialogOptions {
  bool local_only = false;
  bool create_folders = true;
  std::vector<std::string> mime_types;
};

struct MuonFsWin32DialogOptions {
  bool force_filesystem = true;
  bool no_dereference_links = false;
  bool dont_add_to_recent = false;
  bool no_validate = false;
  bool strict_file_types = false;
  bool path_must_exist = false;
  bool file_must_exist = false;
};

struct MuonFsDialogOptions {
  std::string title;
  std::string default_path;
  std::string button_label;
  std::string default_name;
  bool has_title = false;
  bool has_default_path = false;
  bool has_button_label = false;
  bool has_default_name = false;
  bool show_hidden = false;
  bool has_show_hidden = false;
  bool modal = true;
  bool confirm_overwrite = true;
  std::uintptr_t owner_window_handle = 0;
  std::vector<MuonFsDialogFilter> filters;
  MuonFsGtkDialogOptions gtk;
  MuonFsWin32DialogOptions win32;
};

static bool ReadOptionalString(yyjson_val* object,
                               const char* key,
                               bool* present,
                               std::string* target,
                               std::string* error_message) {
  const auto value = yyjson_obj_get(object, key);
  *present = false;
  target->clear();
  if (value == nullptr || yyjson_is_null(value)) {
    return true;
  }
  if (!yyjson_is_str(value)) {
    *error_message = std::string("options.") + key + " must be a string";
    return false;
  }
  *present = true;
  *target = ReadJsonString(value);
  return true;
}

static bool ReadOptionalBool(yyjson_val* object,
                             const char* key,
                             bool default_value,
                             bool* target,
                             std::string* error_message) {
  const auto value = yyjson_obj_get(object, key);
  if (value == nullptr || yyjson_is_null(value)) {
    *target = default_value;
    return true;
  }
  if (!yyjson_is_bool(value)) {
    *error_message = std::string("options.") + key + " must be a boolean";
    return false;
  }
  *target = yyjson_get_bool(value);
  return true;
}

static bool ReadOptionalStringArray(yyjson_val* object,
                                    const char* key,
                                    std::vector<std::string>* target,
                                    std::string* error_message) {
  const auto value = yyjson_obj_get(object, key);
  target->clear();
  if (value == nullptr || yyjson_is_null(value)) {
    return true;
  }
  if (!yyjson_is_arr(value)) {
    *error_message = std::string("options.") + key + " must be an array";
    return false;
  }
  size_t index = 0;
  size_t max = 0;
  yyjson_val* entry = nullptr;
  yyjson_arr_foreach(value, index, max, entry) {
    if (!yyjson_is_str(entry)) {
      *error_message = std::string("options.") + key +
                       " entries must be strings";
      return false;
    }
    target->push_back(ReadJsonString(entry));
  }
  return true;
}

static bool ReadOptionalNestedBool(yyjson_val* root,
                                   const char* object_key,
                                   const char* key,
                                   bool default_value,
                                   bool* target,
                                   std::string* error_message) {
  const auto object = yyjson_obj_get(root, object_key);
  if (object == nullptr || yyjson_is_null(object)) {
    *target = default_value;
    return true;
  }
  if (!yyjson_is_obj(object)) {
    *error_message = std::string("options.") + object_key +
                     " must be an object";
    return false;
  }
  const auto value = yyjson_obj_get(object, key);
  if (value == nullptr || yyjson_is_null(value)) {
    *target = default_value;
    return true;
  }
  if (!yyjson_is_bool(value)) {
    *error_message = std::string("options.") + object_key + "." + key +
                     " must be a boolean";
    return false;
  }
  *target = yyjson_get_bool(value);
  return true;
}

static bool ParseFilters(yyjson_val* root,
                         std::vector<MuonFsDialogFilter>* filters,
                         std::string* error_message) {
  const auto value = yyjson_obj_get(root, "filters");
  filters->clear();
  if (value == nullptr || yyjson_is_null(value)) {
    return true;
  }
  if (!yyjson_is_arr(value)) {
    *error_message = "options.filters must be an array";
    return false;
  }
  size_t index = 0;
  size_t max = 0;
  yyjson_val* entry = nullptr;
  yyjson_arr_foreach(value, index, max, entry) {
    if (!yyjson_is_obj(entry)) {
      *error_message = "options.filters entries must be objects";
      return false;
    }
    MuonFsDialogFilter filter;
    bool has_name = false;
    if (!ReadOptionalString(entry, "name", &has_name, &filter.name,
                            error_message)) {
      return false;
    }
    if (!has_name || filter.name.empty()) {
      *error_message = "options.filters entries require a name";
      return false;
    }
    if (!ReadOptionalStringArray(entry, "extensions", &filter.extensions,
                                 error_message)) {
      return false;
    }
    if (filter.extensions.empty()) {
      *error_message = "options.filters entries require extensions";
      return false;
    }
    filters->push_back(std::move(filter));
  }
  return true;
}

static bool ParseGtkOptions(yyjson_val* root,
                            MuonFsGtkDialogOptions* options,
                            std::string* error_message) {
  if (!ReadOptionalNestedBool(root, "gtk", "localOnly", false,
                              &options->local_only, error_message)) {
    return false;
  }
  if (!ReadOptionalNestedBool(root, "gtk", "createFolders", true,
                              &options->create_folders, error_message)) {
    return false;
  }
  const auto gtk = yyjson_obj_get(root, "gtk");
  if (gtk == nullptr || yyjson_is_null(gtk)) {
    options->mime_types.clear();
    return true;
  }
  return ReadOptionalStringArray(gtk, "mimeTypes", &options->mime_types,
                                 error_message);
}

static bool ParseWin32Options(yyjson_val* root,
                              MuonFsWin32DialogOptions* options,
                              std::string* error_message) {
  return ReadOptionalNestedBool(root, "win32", "forceFilesystem", true,
                                &options->force_filesystem, error_message) &&
         ReadOptionalNestedBool(root, "win32", "noDereferenceLinks", false,
                                &options->no_dereference_links,
                                error_message) &&
         ReadOptionalNestedBool(root, "win32", "dontAddToRecent", false,
                                &options->dont_add_to_recent, error_message) &&
         ReadOptionalNestedBool(root, "win32", "noValidate", false,
                                &options->no_validate, error_message) &&
         ReadOptionalNestedBool(root, "win32", "strictFileTypes", false,
                                &options->strict_file_types, error_message) &&
         ReadOptionalNestedBool(root, "win32", "pathMustExist", false,
                                &options->path_must_exist, error_message) &&
         ReadOptionalNestedBool(root, "win32", "fileMustExist", false,
                                &options->file_must_exist, error_message);
}

static bool ParseOwnerWindowHandle(yyjson_val* root,
                                   std::uintptr_t* owner_window_handle,
                                   std::string* error_message) {
  *owner_window_handle = 0;
  const auto value = yyjson_obj_get(root, kMuonFsDialogsOwnerWindowHandleOption);
  if (value == nullptr || yyjson_is_null(value)) {
    return true;
  }
  if (!yyjson_is_str(value)) {
    *error_message = std::string("options.") +
                     kMuonFsDialogsOwnerWindowHandleOption +
                     " must be a string";
    return false;
  }
  const auto* text = yyjson_get_str(value);
  char* end = nullptr;
  errno = 0;
  const auto parsed = std::strtoull(text, &end, 10);
  if (errno != 0 || end == text || (end != nullptr && *end != '\0')) {
    *error_message = std::string("options.") +
                     kMuonFsDialogsOwnerWindowHandleOption +
                     " is invalid";
    return false;
  }
  *owner_window_handle = static_cast<std::uintptr_t>(parsed);
  return true;
}

static bool ParseDialogOptions(const char* options_json,
                               MuonFsDialogOptions* options,
                               std::string* error_message) {
  MuonJsonDocument document(nullptr);
  yyjson_val* root = nullptr;
  if (!ParseJsonObjectOptions(options_json, &document, &root,
                              error_message)) {
    return false;
  }
  return ReadOptionalString(root, "title", &options->has_title,
                            &options->title, error_message) &&
         ReadOptionalString(root, "defaultPath", &options->has_default_path,
                            &options->default_path, error_message) &&
         ReadOptionalString(root, "buttonLabel", &options->has_button_label,
                            &options->button_label, error_message) &&
         ReadOptionalString(root, "defaultName", &options->has_default_name,
                            &options->default_name, error_message) &&
         ReadOptionalBool(root, "modal", true, &options->modal,
                          error_message) &&
         ReadOptionalBool(root, "showHidden", false, &options->show_hidden,
                          error_message) &&
         ReadOptionalBool(root, "confirmOverwrite", true,
                          &options->confirm_overwrite, error_message) &&
         ParseFilters(root, &options->filters, error_message) &&
         ParseGtkOptions(root, &options->gtk, error_message) &&
         ParseWin32Options(root, &options->win32, error_message) &&
         ParseOwnerWindowHandle(root, &options->owner_window_handle,
                                error_message);
}

static bool IsMultipleDialog(MuonFsDialogKind kind) {
  return kind == MuonFsDialogKind::SelectFiles ||
         kind == MuonFsDialogKind::SelectDirectories;
}

static bool IsDirectoryDialog(MuonFsDialogKind kind) {
  return kind == MuonFsDialogKind::SelectDirectory ||
         kind == MuonFsDialogKind::SelectDirectories;
}

static bool IsSaveDialog(MuonFsDialogKind kind) {
  return kind == MuonFsDialogKind::SelectSaveFile;
}

static std::string CreateStringArrayJson(
    const std::vector<std::string>& values) {
  auto result = std::string("[");
  auto first = true;
  for (const auto& value : values) {
    if (!first) {
      result += ",";
    }
    first = false;
    AppendJsonString(&result, value);
  }
  result += "]";
  return result;
}

#if defined(_WIN32)

class MuonComInitializer final {
 public:
  MuonComInitializer() {
    result_ = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  }

  ~MuonComInitializer() {
    if (result_ == S_OK || result_ == S_FALSE) {
      CoUninitialize();
    }
  }

  bool succeeded() const {
    return result_ == S_OK || result_ == S_FALSE ||
           result_ == RPC_E_CHANGED_MODE;
  }

 private:
  HRESULT result_ = E_FAIL;
};

template <typename T>
class MuonComPtr final {
 public:
  MuonComPtr() = default;

  ~MuonComPtr() {
    Reset();
  }

  MuonComPtr(const MuonComPtr&) = delete;
  MuonComPtr& operator=(const MuonComPtr&) = delete;

  T** Out() {
    Reset();
    return &value_;
  }

  T* get() const {
    return value_;
  }

  T* operator->() const {
    return value_;
  }

  void Reset() {
    if (value_ != nullptr) {
      value_->Release();
      value_ = nullptr;
    }
  }

 private:
  T* value_ = nullptr;
};

static std::wstring Utf8ToWide(const std::string& source) {
  if (source.empty()) {
    return std::wstring{};
  }
  const auto length = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, source.data(),
      static_cast<int>(source.size()), nullptr, 0);
  if (length <= 0) {
    throw std::runtime_error("String is not valid UTF-8");
  }
  auto target = std::wstring(static_cast<size_t>(length), L'\0');
  const auto converted = MultiByteToWideChar(
      CP_UTF8, MB_ERR_INVALID_CHARS, source.data(),
      static_cast<int>(source.size()), target.data(), length);
  if (converted != length) {
    throw std::runtime_error("String conversion failed");
  }
  return target;
}

static std::string WideToUtf8(const wchar_t* source) {
  if (source == nullptr || source[0] == L'\0') {
    return std::string{};
  }
  const auto length = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, source, -1, nullptr, 0, nullptr,
      nullptr);
  if (length <= 0) {
    throw std::runtime_error("String conversion failed");
  }
  auto target = std::string(static_cast<size_t>(length - 1), '\0');
  const auto converted = WideCharToMultiByte(
      CP_UTF8, WC_ERR_INVALID_CHARS, source, -1, target.data(), length,
      nullptr, nullptr);
  if (converted != length) {
    throw std::runtime_error("String conversion failed");
  }
  return target;
}

static std::string ReadShellItemPath(IShellItem* item) {
  PWSTR raw_path = nullptr;
  auto result = item->GetDisplayName(SIGDN_FILESYSPATH, &raw_path);
  if (FAILED(result)) {
    result = item->GetDisplayName(SIGDN_URL, &raw_path);
  }
  if (FAILED(result) || raw_path == nullptr) {
    throw std::runtime_error("Failed to read selected path");
  }
  auto path = WideToUtf8(raw_path);
  CoTaskMemFree(raw_path);
  return path;
}

static std::wstring CreateFilterPattern(
    const MuonFsDialogFilter& filter) {
  auto result = std::wstring{};
  auto first = true;
  for (const auto& extension : filter.extensions) {
    if (!first) {
      result += L";";
    }
    first = false;
    if (extension == "*") {
      result += L"*";
    } else {
      result += L"*.";
      result += Utf8ToWide(extension);
    }
  }
  return result;
}

static void ApplyWin32Filters(IFileDialog* dialog,
                              const MuonFsDialogOptions& options) {
  if (options.filters.empty()) {
    return;
  }
  std::vector<std::wstring> names;
  std::vector<std::wstring> patterns;
  std::vector<COMDLG_FILTERSPEC> specs;
  names.reserve(options.filters.size());
  patterns.reserve(options.filters.size());
  specs.reserve(options.filters.size());
  for (const auto& filter : options.filters) {
    names.push_back(Utf8ToWide(filter.name));
    patterns.push_back(CreateFilterPattern(filter));
  }
  for (auto index = size_t{0}; index < options.filters.size(); ++index) {
    specs.push_back({names[index].c_str(), patterns[index].c_str()});
  }
  dialog->SetFileTypes(static_cast<UINT>(specs.size()), specs.data());
}

static void ApplyWin32CommonOptions(IFileDialog* dialog,
                                    MuonFsDialogKind kind,
                                    const MuonFsDialogOptions& options) {
  DWORD flags = 0;
  if (FAILED(dialog->GetOptions(&flags))) {
    throw std::runtime_error("Failed to read dialog options");
  }
  if (IsMultipleDialog(kind)) {
    flags |= FOS_ALLOWMULTISELECT;
  }
  if (IsDirectoryDialog(kind)) {
    flags |= FOS_PICKFOLDERS;
  }
  if (!IsSaveDialog(kind)) {
    flags |= FOS_PATHMUSTEXIST;
    if (!IsDirectoryDialog(kind)) {
      flags |= FOS_FILEMUSTEXIST;
    }
  }
  if (IsSaveDialog(kind) && options.confirm_overwrite) {
    flags |= FOS_OVERWRITEPROMPT;
  }
  if (options.win32.force_filesystem) {
    flags |= FOS_FORCEFILESYSTEM;
  }
  if (options.win32.no_dereference_links) {
    flags |= FOS_NODEREFERENCELINKS;
  }
  if (options.win32.dont_add_to_recent) {
    flags |= FOS_DONTADDTORECENT;
  }
  if (options.win32.no_validate) {
    flags |= FOS_NOVALIDATE;
  }
  if (options.win32.strict_file_types) {
    flags |= FOS_STRICTFILETYPES;
  }
  if (options.win32.path_must_exist) {
    flags |= FOS_PATHMUSTEXIST;
  }
  if (options.win32.file_must_exist) {
    flags |= FOS_FILEMUSTEXIST;
  }
  if (FAILED(dialog->SetOptions(flags))) {
    throw std::runtime_error("Failed to set dialog options");
  }
  if (options.has_title) {
    dialog->SetTitle(Utf8ToWide(options.title).c_str());
  }
  if (options.has_button_label) {
    dialog->SetOkButtonLabel(Utf8ToWide(options.button_label).c_str());
  }
  if (options.has_default_name) {
    dialog->SetFileName(Utf8ToWide(options.default_name).c_str());
  }
  ApplyWin32Filters(dialog, options);
}

static void ApplyWin32DefaultFolder(IFileDialog* dialog,
                                    const MuonFsDialogOptions& options) {
  if (!options.has_default_path || options.default_path.empty()) {
    return;
  }
  auto wide_path = Utf8ToWide(options.default_path);
  MuonComPtr<IShellItem> item;
  if (SUCCEEDED(SHCreateItemFromParsingName(
          wide_path.c_str(), nullptr, IID_PPV_ARGS(item.Out())))) {
    dialog->SetFolder(item.get());
  }
}

static std::vector<std::string> RunWin32NativeDialog(
    MuonFsDialogKind kind,
    const MuonFsDialogOptions& options,
    std::shared_ptr<MuonFsNativeDialogCancellation> cancellation) {
  auto com = MuonComInitializer();
  if (!com.succeeded()) {
    throw std::runtime_error("COM initialization failed");
  }
  MuonComPtr<IFileDialog> dialog;
  if (IsSaveDialog(kind)) {
    MuonComPtr<IFileSaveDialog> save_dialog;
    if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(save_dialog.Out())))) {
      throw std::runtime_error("Failed to create save dialog");
    }
    if (FAILED(save_dialog->QueryInterface(IID_PPV_ARGS(dialog.Out())))) {
      throw std::runtime_error("Failed to create save dialog");
    }
  } else {
    MuonComPtr<IFileOpenDialog> open_dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                                CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(open_dialog.Out())))) {
      throw std::runtime_error("Failed to create open dialog");
    }
    if (FAILED(open_dialog->QueryInterface(IID_PPV_ARGS(dialog.Out())))) {
      throw std::runtime_error("Failed to create open dialog");
    }
  }
  ApplyWin32CommonOptions(dialog.get(), kind, options);
  ApplyWin32DefaultFolder(dialog.get(), options);
  auto cancellation_attachment = MuonNativeDialogCancellationAttachment(
      std::move(cancellation), dialog.get(), options.owner_window_handle);
  if (!cancellation_attachment.attached()) {
    return {};
  }
  const auto owner_window =
      reinterpret_cast<HWND>(options.owner_window_handle);
  const auto shown = dialog->Show(owner_window);
  cancellation_attachment.Detach();
  if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
    return {};
  }
  if (FAILED(shown)) {
    throw std::runtime_error("Dialog failed");
  }
  if (IsMultipleDialog(kind)) {
    MuonComPtr<IFileOpenDialog> open_dialog;
    if (FAILED(dialog->QueryInterface(IID_PPV_ARGS(open_dialog.Out())))) {
      throw std::runtime_error("Failed to read dialog results");
    }
    MuonComPtr<IShellItemArray> items;
    if (FAILED(open_dialog->GetResults(items.Out()))) {
      throw std::runtime_error("Failed to read dialog results");
    }
    DWORD count = 0;
    if (FAILED(items->GetCount(&count))) {
      throw std::runtime_error("Failed to read dialog result count");
    }
    std::vector<std::string> paths;
    paths.reserve(count);
    for (DWORD index = 0; index < count; ++index) {
      MuonComPtr<IShellItem> item;
      if (SUCCEEDED(items->GetItemAt(index, item.Out()))) {
        paths.push_back(ReadShellItemPath(item.get()));
      }
    }
    return paths;
  }
  MuonComPtr<IShellItem> item;
  if (FAILED(dialog->GetResult(item.Out()))) {
    throw std::runtime_error("Failed to read dialog result");
  }
  return {ReadShellItemPath(item.get())};
}

#else

class MuonGObjectPtr final {
 public:
  explicit MuonGObjectPtr(gpointer source = nullptr)
      : value_(source) {}

  ~MuonGObjectPtr() {
    Reset();
  }

  MuonGObjectPtr(const MuonGObjectPtr&) = delete;
  MuonGObjectPtr& operator=(const MuonGObjectPtr&) = delete;

  gpointer get() const {
    return value_;
  }

  void Reset(gpointer next = nullptr) {
    if (value_ != nullptr) {
      g_object_unref(value_);
    }
    value_ = next;
  }

 private:
  gpointer value_ = nullptr;
};

class MuonGtkWidgetPtr final {
 public:
  explicit MuonGtkWidgetPtr(GtkWidget* source = nullptr)
      : value_(source) {}

  ~MuonGtkWidgetPtr() {
    Reset();
  }

  MuonGtkWidgetPtr(const MuonGtkWidgetPtr&) = delete;
  MuonGtkWidgetPtr& operator=(const MuonGtkWidgetPtr&) = delete;

  GtkWidget* get() const {
    return value_;
  }

  void Reset(GtkWidget* next = nullptr) {
    if (value_ != nullptr) {
      gtk_widget_destroy(value_);
    }
    value_ = next;
  }

 private:
  GtkWidget* value_ = nullptr;
};

static void EnsureGtkInitialized() {
  static bool initialized = false;
  static bool attempted = false;
  if (initialized) {
    return;
  }
  if (attempted) {
    throw std::runtime_error("GTK initialization failed");
  }
  attempted = true;
  int argc = 0;
  char** argv = nullptr;
  initialized = gtk_init_check(&argc, &argv);
  if (!initialized) {
    throw std::runtime_error("GTK initialization failed");
  }
}

static std::string ReadGFilePathOrUri(GFile* file) {
  if (file == nullptr) {
    return {};
  }
  auto* path = g_file_get_path(file);
  if (path != nullptr) {
    auto result = std::string(path);
    g_free(path);
    return result;
  }
  auto* uri = g_file_get_uri(file);
  if (uri == nullptr) {
    return {};
  }
  auto result = std::string(uri);
  g_free(uri);
  return result;
}

static GtkFileChooserAction GetGtkAction(MuonFsDialogKind kind) {
  if (IsSaveDialog(kind)) {
    return GTK_FILE_CHOOSER_ACTION_SAVE;
  }
  if (IsDirectoryDialog(kind)) {
    return GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER;
  }
  return GTK_FILE_CHOOSER_ACTION_OPEN;
}

static const char* GetDefaultAcceptLabel(MuonFsDialogKind kind) {
  if (IsSaveDialog(kind)) {
    return "_Save";
  }
  if (IsDirectoryDialog(kind)) {
    return "_Select";
  }
  return "_Open";
}

static std::string NormalizeGtkPattern(const std::string& extension) {
  if (extension == "*") {
    return "*";
  }
  return "*." + extension;
}

static void ApplyGtkFilters(GtkFileChooser* chooser,
                            const MuonFsDialogOptions& options) {
  for (const auto& source_filter : options.filters) {
    auto* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, source_filter.name.c_str());
    for (const auto& extension : source_filter.extensions) {
      const auto pattern = NormalizeGtkPattern(extension);
      gtk_file_filter_add_pattern(filter, pattern.c_str());
    }
    gtk_file_chooser_add_filter(chooser, filter);
  }
  if (!options.gtk.mime_types.empty()) {
    auto* filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Allowed MIME types");
    for (const auto& mime_type : options.gtk.mime_types) {
      gtk_file_filter_add_mime_type(filter, mime_type.c_str());
    }
    gtk_file_chooser_add_filter(chooser, filter);
  }
}

static void ApplyGtkDefaultPath(GtkFileChooser* chooser,
                                MuonFsDialogKind kind,
                                const MuonFsDialogOptions& options) {
  if (options.has_default_path && !options.default_path.empty()) {
    auto file = MuonGObjectPtr(CreateGFileFromPathOrUri(
        options.default_path));
    if (IsSaveDialog(kind) && options.has_default_name) {
      gtk_file_chooser_set_current_folder_file(
          chooser, static_cast<GFile*>(file.get()), nullptr);
    } else {
      gtk_file_chooser_set_file(
          chooser, static_cast<GFile*>(file.get()), nullptr);
    }
  }
  if (IsSaveDialog(kind) && options.has_default_name) {
    gtk_file_chooser_set_current_name(chooser, options.default_name.c_str());
  }
}

static std::vector<std::string> ReadGtkDialogResults(GtkFileChooser* chooser,
                                                     MuonFsDialogKind kind) {
  std::vector<std::string> results;
  if (IsMultipleDialog(kind)) {
    auto* files = gtk_file_chooser_get_files(chooser);
    for (auto* entry = files; entry != nullptr; entry = entry->next) {
      auto* file = static_cast<GFile*>(entry->data);
      auto value = ReadGFilePathOrUri(file);
      if (!value.empty()) {
        results.push_back(std::move(value));
      }
      g_object_unref(file);
    }
    g_slist_free(files);
    return results;
  }
  auto file = MuonGObjectPtr(gtk_file_chooser_get_file(chooser));
  auto value = ReadGFilePathOrUri(static_cast<GFile*>(file.get()));
  if (!value.empty()) {
    results.push_back(std::move(value));
  }
  return results;
}

static std::vector<std::string> RunGtkNativeDialog(
    MuonFsDialogKind kind,
    const MuonFsDialogOptions& options,
    std::shared_ptr<MuonFsNativeDialogCancellation> cancellation) {
  EnsureGtkInitialized();
  const auto title = options.has_title ? options.title : std::string{};
  const auto accept_label = options.has_button_label
                                ? options.button_label.c_str()
                                : GetDefaultAcceptLabel(kind);
  auto* dialog = gtk_file_chooser_dialog_new(
      title.empty() ? nullptr : title.c_str(), nullptr, GetGtkAction(kind),
      "_Cancel", GTK_RESPONSE_CANCEL, accept_label, GTK_RESPONSE_ACCEPT,
      nullptr);
  if (dialog == nullptr) {
    throw std::runtime_error("Failed to create GTK file chooser dialog");
  }
  auto dialog_holder = MuonGtkWidgetPtr(dialog);
  auto* chooser = GTK_FILE_CHOOSER(dialog);
  gtk_file_chooser_set_select_multiple(chooser, IsMultipleDialog(kind));
  gtk_file_chooser_set_local_only(chooser, options.gtk.local_only);
  gtk_file_chooser_set_create_folders(chooser, options.gtk.create_folders);
  if (options.has_show_hidden) {
    gtk_file_chooser_set_show_hidden(chooser, options.show_hidden);
  }
  if (IsSaveDialog(kind)) {
    gtk_file_chooser_set_do_overwrite_confirmation(
        chooser, options.confirm_overwrite);
  }
  ApplyGtkFilters(chooser, options);
  ApplyGtkDefaultPath(chooser, kind, options);
  gtk_widget_show_all(dialog);
  auto cancellation_attachment = MuonNativeDialogCancellationAttachment(
      std::move(cancellation), dialog, 0);
  if (!cancellation_attachment.attached()) {
    return {};
  }
  const auto response = gtk_dialog_run(GTK_DIALOG(dialog));
  cancellation_attachment.Detach();
  if (response != GTK_RESPONSE_ACCEPT) {
    return {};
  }
  auto results = ReadGtkDialogResults(chooser, kind);
  return results;
}

#endif

std::string RunMuonFsNativeDialog(MuonFsDialogKind kind,
                                  const char* options_json,
                                  std::shared_ptr<
                                      MuonFsNativeDialogCancellation>
                                      cancellation) {
  MuonFsDialogOptions options;
  std::string error_message;
  if (!ParseDialogOptions(options_json, &options, &error_message)) {
    throw std::runtime_error(error_message);
  }
#if defined(_WIN32)
  return CreateStringArrayJson(
      RunWin32NativeDialog(kind, options, std::move(cancellation)));
#else
  return CreateStringArrayJson(
      RunGtkNativeDialog(kind, options, std::move(cancellation)));
#endif
}

}  // namespace muon_internal

struct muon_ui_fs_dialog_operation {
  muon_ui_fs_dialog_kind kind = MUON_UI_FS_DIALOG_SELECT_FILE;
  std::string options_json;
  int owner_browser_id = 0;
  muon_ui_fs_dialog_completion completion = nullptr;
  void* user_data = nullptr;
  std::shared_ptr<muon_internal::MuonFsNativeDialogCancellation> cancellation;
  bool completed = false;
};

namespace {

std::vector<std::unique_ptr<muon_ui_fs_dialog_operation>>
    g_muon_ui_fs_dialog_operations;
std::map<int, std::vector<muon_ui_fs_dialog_operation*>>
    g_muon_ui_fs_dialog_operations_by_owner;

muon_internal::MuonFsDialogKind ToMuonFsDialogKind(
    muon_ui_fs_dialog_kind kind) {
  switch (kind) {
    case MUON_UI_FS_DIALOG_SELECT_FILE:
      return muon_internal::MuonFsDialogKind::SelectFile;
    case MUON_UI_FS_DIALOG_SELECT_FILES:
      return muon_internal::MuonFsDialogKind::SelectFiles;
    case MUON_UI_FS_DIALOG_SELECT_DIRECTORY:
      return muon_internal::MuonFsDialogKind::SelectDirectory;
    case MUON_UI_FS_DIALOG_SELECT_DIRECTORIES:
      return muon_internal::MuonFsDialogKind::SelectDirectories;
    case MUON_UI_FS_DIALOG_SELECT_SAVE_FILE:
      return muon_internal::MuonFsDialogKind::SelectSaveFile;
  }
  throw std::runtime_error("Unsupported filesystem dialog kind");
}

bool IsValidMuonUiFsDialogKind(muon_ui_fs_dialog_kind kind) {
  switch (kind) {
    case MUON_UI_FS_DIALOG_SELECT_FILE:
    case MUON_UI_FS_DIALOG_SELECT_FILES:
    case MUON_UI_FS_DIALOG_SELECT_DIRECTORY:
    case MUON_UI_FS_DIALOG_SELECT_DIRECTORIES:
    case MUON_UI_FS_DIALOG_SELECT_SAVE_FILE:
      return true;
  }
  return false;
}

void UnregisterMuonUiFsDialogOperation(muon_ui_fs_dialog_operation* operation) {
  if (operation == nullptr || operation->owner_browser_id <= 0) {
    return;
  }
  const auto owner_iterator =
      g_muon_ui_fs_dialog_operations_by_owner.find(operation->owner_browser_id);
  if (owner_iterator == g_muon_ui_fs_dialog_operations_by_owner.end()) {
    return;
  }
  auto& operations = owner_iterator->second;
  operations.erase(std::remove(operations.begin(), operations.end(), operation),
                   operations.end());
  if (operations.empty()) {
    g_muon_ui_fs_dialog_operations_by_owner.erase(owner_iterator);
  }
}

void CompleteMuonUiFsDialogOperation(muon_ui_fs_dialog_operation* operation,
                                     const char* result_json,
                                     const char* error_message) {
  if (operation == nullptr) {
    return;
  }
  muon_ui_fs_dialog_completion completion = nullptr;
  void* user_data = nullptr;
  if (operation->completed) {
    return;
  }
  operation->completed = true;
  completion = operation->completion;
  user_data = operation->user_data;
  UnregisterMuonUiFsDialogOperation(operation);
  if (completion != nullptr) {
    completion(user_data, result_json, error_message);
  }
}

void RunMuonUiFsDialogOperation(muon_ui_fs_dialog_operation* operation) {
  if (operation == nullptr) {
    return;
  }
  try {
    auto result_json = muon_internal::RunMuonFsNativeDialog(
        ToMuonFsDialogKind(operation->kind), operation->options_json.c_str(),
        operation->cancellation);
    CompleteMuonUiFsDialogOperation(operation, result_json.c_str(), nullptr);
  } catch (const std::exception& error) {
    CompleteMuonUiFsDialogOperation(operation, nullptr, error.what());
  } catch (...) {
    CompleteMuonUiFsDialogOperation(operation, nullptr, "Native dialog failed");
  }
}

#if defined(_WIN32)
void PostMuonUiFsDialogOperation(muon_ui_fs_dialog_operation* operation) {
  auto* dispatcher = cardio::unsafe_get_current_dispatcher();
  if (dispatcher == nullptr) {
    CompleteMuonUiFsDialogOperation(
        operation, nullptr, "Muon main dispatcher is unavailable");
    return;
  }
  muon_internal::FireAndForgetOnDispatcher(
      dispatcher, [operation]() { RunMuonUiFsDialogOperation(operation); });
}
#else
gboolean RunMuonUiFsDialogOperationOnMainContext(gpointer raw_operation) {
  RunMuonUiFsDialogOperation(
      static_cast<muon_ui_fs_dialog_operation*>(raw_operation));
  return G_SOURCE_REMOVE;
}

void PostMuonUiFsDialogOperation(muon_ui_fs_dialog_operation* operation) {
  g_main_context_invoke(
      nullptr, RunMuonUiFsDialogOperationOnMainContext, operation);
}
#endif

}  // namespace

extern "C" int muon_ui_fs_dialogs_initialize(void) {
  return 0;
}

extern "C" void muon_ui_fs_dialogs_shutdown(void) {
  std::vector<muon_ui_fs_dialog_operation*> operations;
  for (const auto& operation : g_muon_ui_fs_dialog_operations) {
    operations.push_back(operation.get());
  }
  for (auto* operation : operations) {
    muon_ui_fs_dialogs_cancel(operation);
  }
  g_muon_ui_fs_dialog_operations_by_owner.clear();
}

extern "C" int muon_ui_fs_dialogs_run(
    muon_ui_fs_dialog_kind kind,
    const char* options_json,
    int owner_browser_id,
    muon_ui_fs_dialog_completion completion,
    void* user_data,
    muon_ui_fs_dialog_operation_handle* out_operation) {
  if (out_operation != nullptr) {
    *out_operation = nullptr;
  }
  if (!IsValidMuonUiFsDialogKind(kind) || options_json == nullptr ||
      completion == nullptr) {
    return 1;
  }
#if defined(_WIN32)
  if (cardio::unsafe_get_current_dispatcher() == nullptr) {
    return 1;
  }
#endif

  auto operation = std::make_unique<muon_ui_fs_dialog_operation>();
  operation->kind = kind;
  operation->options_json = options_json;
  operation->owner_browser_id = owner_browser_id > 0 ? owner_browser_id : 0;
  operation->completion = completion;
  operation->user_data = user_data;
  operation->cancellation =
      std::make_shared<muon_internal::MuonFsNativeDialogCancellation>();
  auto* raw_operation = operation.get();
  if (operation->owner_browser_id > 0) {
    g_muon_ui_fs_dialog_operations_by_owner[
        operation->owner_browser_id].push_back(raw_operation);
  }
  g_muon_ui_fs_dialog_operations.push_back(std::move(operation));
  if (out_operation != nullptr) {
    *out_operation = raw_operation;
  }
  PostMuonUiFsDialogOperation(raw_operation);
  return 0;
}

extern "C" void muon_ui_fs_dialogs_cancel(
    muon_ui_fs_dialog_operation_handle operation) {
  if (operation == nullptr) {
    return;
  }
  auto cancellation =
      std::shared_ptr<muon_internal::MuonFsNativeDialogCancellation>();
  if (operation->completed) {
    return;
  }
  cancellation = operation->cancellation;
  if (cancellation) {
    cancellation->Cancel();
  }
}

extern "C" void muon_ui_fs_dialogs_cancel_owner_browser(
    int owner_browser_id) {
  if (owner_browser_id <= 0) {
    return;
  }
  std::vector<muon_ui_fs_dialog_operation*> operations;
  const auto owner_iterator =
      g_muon_ui_fs_dialog_operations_by_owner.find(owner_browser_id);
  if (owner_iterator == g_muon_ui_fs_dialog_operations_by_owner.end()) {
    return;
  }
  operations = owner_iterator->second;
  for (auto* operation : operations) {
    muon_ui_fs_dialogs_cancel(operation);
  }
}
