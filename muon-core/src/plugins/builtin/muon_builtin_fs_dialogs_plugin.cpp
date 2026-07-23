/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "plugins/builtin/muon_builtin_fs_dialogs_plugin.h"

#include "plugins/builtin/muon_builtin_completion.h"
#include "ui/muon_ui_fs_dialogs.h"

#include "yyjson.h"

#include <algorithm>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace muon_internal {

const muon_plugin_helpers* g_fs_dialogs_helpers = nullptr;

static const muon_type_descriptor type_void = {
    MUON_TYPE_VOID,
    nullptr,
};

static const muon_type_descriptor type_string = {
    MUON_TYPE_STRING,
    nullptr,
};

static const muon_function_signature cancel_operation_signature = {
    0,
    nullptr,
    &type_void,
};

static const muon_type_descriptor type_cancel_operation_function = {
    MUON_TYPE_FUNCTION,
    &cancel_operation_signature,
};

static const muon_type_descriptor abort_watcher_args[] = {
    type_cancel_operation_function,
};

static const muon_function_signature abort_watcher_signature = {
    1,
    abort_watcher_args,
    &type_void,
};

static const muon_type_descriptor type_abort_watcher_function = {
    MUON_TYPE_FUNCTION,
    &abort_watcher_signature,
};

static const muon_type_descriptor options_abort_args[] = {
    type_string,
    type_abort_watcher_function,
};

struct MuonFsDialogsAbortCallbackState {
  std::shared_ptr<struct MuonFsDialogsProviderOperation> operation;
};

struct MuonFsDialogsOperationState {
  ~MuonFsDialogsOperationState() {
    if (abort_cancel_function == nullptr) {
      return;
    }
    if (abort_helpers != nullptr &&
        abort_helpers->__release_plugin_function_pointer_impl != nullptr) {
      abort_helpers->release_plugin_function_pointer(abort_cancel_function);
    }
    abort_cancel_function = nullptr;
  }

  muon_completion_func completion = nullptr;
  muon_ui_fs_dialog_kind kind = MUON_UI_FS_DIALOG_SELECT_FILE;
  std::string options_json;
  int owner_browser_id = 0;
  std::shared_ptr<struct MuonFsDialogsProviderOperation> provider_operation;
  const muon_plugin_helpers* abort_helpers = nullptr;
  muon_native_function abort_cancel_function = nullptr;
};

struct MuonFsDialogsAbortWatcherCompletionState {
  const muon_plugin_helpers* helpers = nullptr;
  muon_completion_func watcher_completion = nullptr;
  std::shared_ptr<struct MuonFsDialogsProviderOperation> provider_operation;
  std::unique_ptr<MuonFsDialogsOperationState> operation;
};

struct MuonFsDialogsProviderOperation {
  muon_ui_fs_dialog_operation_handle handle = nullptr;
  bool cancel_requested = false;
  bool completed = false;
};

static void FinalizeMuonFsDialogsAbortCallbackState(void* raw_state) {
  delete static_cast<MuonFsDialogsAbortCallbackState*>(raw_state);
}

static int ReadOwnerBrowserId(const char* options_json) {
  if (options_json == nullptr) {
    return 0;
  }
  yyjson_read_err read_error = {};
  auto* document = yyjson_read_opts(
      const_cast<char*>(options_json), std::strlen(options_json),
      YYJSON_READ_NOFLAG, nullptr, &read_error);
  if (document == nullptr) {
    return 0;
  }
  auto* root = yyjson_doc_get_root(document);
  auto owner_browser_id = 0;
  if (root != nullptr && yyjson_is_obj(root)) {
    size_t index = 0;
    size_t max = 0;
    yyjson_val* key = nullptr;
    yyjson_val* value = nullptr;
    yyjson_obj_foreach(root, index, max, key, value) {
      if (yyjson_is_str(key) &&
          std::strcmp(
              yyjson_get_str(key),
              kMuonFsDialogsOwnerBrowserIdOption) == 0 &&
          yyjson_is_int(value)) {
        owner_browser_id = yyjson_get_int(value);
      }
    }
  }
  yyjson_doc_free(document);
  return owner_browser_id > 0 ? owner_browser_id : 0;
}

static void CancelProviderOperation(
    const std::shared_ptr<MuonFsDialogsProviderOperation>& operation) {
  if (!operation) {
    return;
  }
  muon_ui_fs_dialog_operation_handle handle = nullptr;
  if (operation->completed) {
    return;
  }
  operation->cancel_requested = true;
  handle = operation->handle;
  if (handle != nullptr) {
    muon_ui_fs_dialogs_cancel(handle);
  }
}

static void SetProviderOperationHandle(
    const std::shared_ptr<MuonFsDialogsProviderOperation>& operation,
    muon_ui_fs_dialog_operation_handle handle) {
  if (!operation) {
    return;
  }
  auto cancel_requested = false;
  if (operation->completed) {
    return;
  }
  operation->handle = handle;
  cancel_requested = operation->cancel_requested;
  if (cancel_requested && handle != nullptr) {
    muon_ui_fs_dialogs_cancel(handle);
  }
}

static void MarkProviderOperationCompleted(
    const std::shared_ptr<MuonFsDialogsProviderOperation>& operation) {
  if (!operation) {
    return;
  }
  operation->completed = true;
  operation->handle = nullptr;
}

extern "C" void muon_builtin_fs_dialogs_cancel_task(
    muon_completion_func completion,
    void* raw_state) {
  auto* state = static_cast<MuonFsDialogsAbortCallbackState*>(raw_state);
  if (state != nullptr) {
    CancelProviderOperation(state->operation);
  }
  CompleteMuonVoid(completion);
}

static void CompleteMuonFsDialogsProviderOperation(
    void* raw_state,
    const char* result_json,
    const char* error_message) {
  auto state = std::unique_ptr<MuonFsDialogsOperationState>(
      static_cast<MuonFsDialogsOperationState*>(raw_state));
  if (!state) {
    return;
  }
  MarkProviderOperationCompleted(state->provider_operation);
  if (error_message != nullptr) {
    CompleteMuonError(state->completion, error_message);
  } else {
    CompleteMuonString(state->completion,
                   result_json == nullptr ? "[]" : result_json);
  }
}

static void PostMuonFsDialogsOperation(
    std::unique_ptr<MuonFsDialogsOperationState> state) {
  if (state->provider_operation &&
      state->provider_operation->cancel_requested) {
    MarkProviderOperationCompleted(state->provider_operation);
    CompleteMuonError(state->completion, "Native dialog was canceled");
    return;
  }
  auto* raw_state = state.release();
  auto handle = muon_ui_fs_dialog_operation_handle{};
  const auto started = muon_ui_fs_dialogs_run(
      raw_state->kind, raw_state->options_json.c_str(),
      raw_state->owner_browser_id, &CompleteMuonFsDialogsProviderOperation,
      raw_state, &handle);
  if (started != 0) {
    state.reset(raw_state);
    MarkProviderOperationCompleted(state->provider_operation);
    CompleteMuonError(state->completion, "Native dialog failed to start");
    return;
  }
  SetProviderOperationHandle(raw_state->provider_operation, handle);
}

static void CompleteMuonFsDialogsAbortWatcherSetup(
    void* raw_state,
    const muon_completion_error* error) {
  auto* state =
      static_cast<MuonFsDialogsAbortWatcherCompletionState*>(raw_state);
  if (state == nullptr) {
    return;
  }
  auto operation = std::move(state->operation);
  if (error != nullptr) {
    CancelProviderOperation(state->provider_operation);
    if (operation) {
      MarkProviderOperationCompleted(operation->provider_operation);
      CompleteMuonError(
          operation->completion,
          error->message[0] == '\0'
              ? "AbortSignal watcher setup failed"
              : error->message);
    }
  } else {
    PostMuonFsDialogsOperation(std::move(operation));
  }
  if (state->helpers != nullptr &&
      state->helpers->__release_plugin_function_pointer_impl != nullptr &&
      state->watcher_completion != nullptr) {
    state->helpers->release_plugin_function_pointer(
        state->watcher_completion);
  }
  delete state;
}

static bool RegisterAbortWatcher(
    const muon_plugin_helpers* helpers,
    muon_native_function abort_watcher,
    const std::shared_ptr<MuonFsDialogsProviderOperation>& provider_operation,
    std::unique_ptr<MuonFsDialogsOperationState>* operation,
    std::string* error_message) {
  if (abort_watcher == nullptr) {
    return true;
  }
  if (helpers == nullptr ||
      helpers->__register_closure_impl == nullptr ||
      helpers->__create_completion_function_impl == nullptr ||
      helpers->__release_plugin_function_pointer_impl == nullptr) {
    *error_message = "AbortSignal helper is unavailable";
    return false;
  }

  auto* cancel_state = new MuonFsDialogsAbortCallbackState;
  cancel_state->operation = provider_operation;
  auto cancel_function = muon_native_function{};
  char cancel_error_storage[MUON_COMPLETION_ERROR_MESSAGE_CAPACITY] = "";
  auto cancel_error = muon_error_buffer{
      cancel_error_storage,
      static_cast<uint32_t>(sizeof(cancel_error_storage)),
  };
  if (!helpers->register_closure(
          &cancel_operation_signature,
          &muon_builtin_fs_dialogs_cancel_task,
          cancel_state,
          &FinalizeMuonFsDialogsAbortCallbackState,
          &cancel_function,
          &cancel_error)) {
    delete cancel_state;
    *error_message = cancel_error_storage[0] == '\0'
                         ? "AbortSignal cancel callback registration failed"
                         : cancel_error_storage;
    return false;
  }

  auto* completion_state = new MuonFsDialogsAbortWatcherCompletionState;
  completion_state->helpers = helpers;
  completion_state->provider_operation = provider_operation;
  char completion_error_storage[MUON_COMPLETION_ERROR_MESSAGE_CAPACITY] = "";
  auto completion_error = muon_error_buffer{
      completion_error_storage,
      static_cast<uint32_t>(sizeof(completion_error_storage)),
  };
  if (!helpers->create_completion_function(
          &type_void,
          &CompleteMuonFsDialogsAbortWatcherSetup,
          completion_state,
          &completion_state->watcher_completion,
          &completion_error)) {
    helpers->release_plugin_function_pointer(cancel_function);
    delete completion_state;
    *error_message = completion_error_storage[0] == '\0'
                         ? "AbortSignal watcher completion registration failed"
                         : completion_error_storage;
    return false;
  }

  completion_state->operation = std::move(*operation);
  completion_state->operation->abort_helpers = helpers;
  completion_state->operation->abort_cancel_function = cancel_function;
  using AbortWatcherFunction = void (*)(muon_completion_func,
                                        muon_native_function);
  reinterpret_cast<AbortWatcherFunction>(abort_watcher)(
      completion_state->watcher_completion, cancel_function);
  return true;
}

static void RunMuonFsDialogsOperation(muon_completion_func completion,
                                      muon_ui_fs_dialog_kind kind,
                                      const char* options_json,
                                      muon_native_function abort_watcher) {
  if (options_json == nullptr) {
    CompleteMuonError(completion, "Options JSON is required");
    return;
  }
  auto operation = std::make_unique<MuonFsDialogsOperationState>();
  operation->completion = completion;
  operation->kind = kind;
  operation->options_json = options_json;
  operation->owner_browser_id = ReadOwnerBrowserId(options_json);
  operation->provider_operation =
      std::make_shared<MuonFsDialogsProviderOperation>();
  auto abort_error = std::string{};
  if (!RegisterAbortWatcher(
          g_fs_dialogs_helpers, abort_watcher, operation->provider_operation,
          &operation, &abort_error)) {
    MarkProviderOperationCompleted(operation->provider_operation);
    CompleteMuonError(completion, abort_error.c_str());
    return;
  }
  if (operation) {
    PostMuonFsDialogsOperation(std::move(operation));
  }
}

extern "C" void muon_builtin_fs_dialogs_select_file(
    muon_completion_func completion,
    const char* options_json,
    muon_native_function abort_watcher) {
  RunMuonFsDialogsOperation(
      completion, MUON_UI_FS_DIALOG_SELECT_FILE, options_json, abort_watcher);
}

extern "C" void muon_builtin_fs_dialogs_select_files(
    muon_completion_func completion,
    const char* options_json,
    muon_native_function abort_watcher) {
  RunMuonFsDialogsOperation(
      completion, MUON_UI_FS_DIALOG_SELECT_FILES, options_json, abort_watcher);
}

extern "C" void muon_builtin_fs_dialogs_select_directory(
    muon_completion_func completion,
    const char* options_json,
    muon_native_function abort_watcher) {
  RunMuonFsDialogsOperation(
      completion,
      MUON_UI_FS_DIALOG_SELECT_DIRECTORY,
      options_json,
      abort_watcher);
}

extern "C" void muon_builtin_fs_dialogs_select_directories(
    muon_completion_func completion,
    const char* options_json,
    muon_native_function abort_watcher) {
  RunMuonFsDialogsOperation(
      completion,
      MUON_UI_FS_DIALOG_SELECT_DIRECTORIES,
      options_json,
      abort_watcher);
}

extern "C" void muon_builtin_fs_dialogs_select_save_file(
    muon_completion_func completion,
    const char* options_json,
    muon_native_function abort_watcher) {
  RunMuonFsDialogsOperation(
      completion,
      MUON_UI_FS_DIALOG_SELECT_SAVE_FILE,
      options_json,
      abort_watcher);
}

static constexpr char fs_dialogs_setup_script[] = R"JS(
const createAbortError = (message) => {
  if (typeof globalThis.DOMException === "function") {
    return new globalThis.DOMException(message, "AbortError");
  }
  const error = new Error(message);
  error.name = "AbortError";
  return error;
};

const createAbortReason = (signal) => {
  if ("reason" in signal && signal.reason !== undefined) {
    return signal.reason;
  }
  return createAbortError("The operation was aborted");
};

const createOwnerCloseAbortReason = () =>
  createAbortError("The owner browser window was closed");

const abortControllerWithReason = (controller, reason) => {
  try {
    controller.abort(reason);
  } catch (_error) {
    controller.abort();
  }
};

const getAbortSignal = (options) => {
  if (options === undefined || options === null) {
    return null;
  }
  if (typeof options !== "object") {
    throw new TypeError("options must be an object");
  }
  const signal = options.signal;
  if (signal === undefined || signal === null) {
    return null;
  }
  if (
    typeof signal.aborted !== "boolean" ||
    typeof signal.addEventListener !== "function" ||
    typeof signal.removeEventListener !== "function"
  ) {
    throw new TypeError("options.signal must be an AbortSignal");
  }
  return signal;
};

const runAbortable = async (options, nativeCall) => {
  const signal = getAbortSignal(options);
  if (signal === null) {
    return await nativeCall(null);
  }
  if (signal.aborted) {
    throw createAbortReason(signal);
  }

  const nativeCancelRecords = new Map();
  let aborted = false;
  let settled = false;
  let rejectAbort = null;
  const abortPromise = new Promise((_resolve, reject) => {
    rejectAbort = reject;
  });

  const invokeNativeCancel = async (record) => {
    try {
      await record.cancel();
    } catch (_error) {
    }
  };
  const requestNativeCancel = async (record) => {
    if (record.released) {
      return;
    }
    if (record.cancelPromise === null) {
      record.cancelPromise = invokeNativeCancel(record);
    }
    await record.cancelPromise;
  };
  const releaseNativeCancel = async (record, shouldCancel) => {
    if (record.released) {
      return;
    }
    let cancelPromise = null;
    if (shouldCancel) {
      cancelPromise = requestNativeCancel(record);
    }
    try {
      record.cancel.release();
    } catch (_error) {
    }
    record.released = true;
    nativeCancelRecords.delete(record.cancel);
    if (cancelPromise !== null) {
      await cancelPromise;
    }
  };
  const getNativeCancelRecord = (cancel) => {
    const existing = nativeCancelRecords.get(cancel);
    if (existing !== undefined) {
      return existing;
    }
    const record = {
      cancel,
      cancelPromise: null,
      released: false,
    };
    nativeCancelRecords.set(cancel, record);
    return record;
  };
  const onAbort = async () => {
    if (aborted) {
      return;
    }
    aborted = true;
    rejectAbort(createAbortReason(signal));
    const records = Array.from(nativeCancelRecords.values());
    for (const record of records) {
      await requestNativeCancel(record);
    }
  };
  signal.addEventListener("abort", onAbort, { once: true });

  const abortWatcher = async (cancel) => {
    const record = getNativeCancelRecord(cancel);
    if (settled) {
      await releaseNativeCancel(record, aborted || signal.aborted);
      return;
    }
    if (aborted || signal.aborted) {
      await requestNativeCancel(record);
    }
  };

  const invokeNative = async () => {
    await Promise.resolve();
    return await nativeCall(abortWatcher);
  };
  try {
    return await Promise.race([invokeNative(), abortPromise]);
  } finally {
    settled = true;
    signal.removeEventListener("abort", onAbort);
    const shouldCancel = aborted || signal.aborted;
    const records = Array.from(nativeCancelRecords.values());
    for (const record of records) {
      await releaseNativeCancel(record, shouldCancel);
    }
  }
};

const parseNativeJson = async (source) => JSON.parse(await source);

const getOptionsObject = (options) => {
  if (options === undefined || options === null) {
    return {};
  }
  if (typeof options !== "object") {
    throw new TypeError("options must be an object");
  }
  return options;
};

const noop = () => {};
const ownerCloseEventName = "muon-owner-browser-close";
const modalOwnerAbortControllersKey =
  "__muonFsDialogModalOwnerAbortControllers";

const getModalOwnerAbortControllers = () => {
  if (
    !Object.prototype.hasOwnProperty.call(
      globalThis,
      modalOwnerAbortControllersKey,
    )
  ) {
    Object.defineProperty(globalThis, modalOwnerAbortControllersKey, {
      configurable: false,
      enumerable: false,
      writable: false,
      value: new Set(),
    });
  }
  return globalThis[modalOwnerAbortControllersKey];
};

const abortModalOwnerDialogs = () => {
  for (const controller of Array.from(getModalOwnerAbortControllers())) {
    abortControllerWithReason(controller, createOwnerCloseAbortReason());
  }
};

if (
  !Object.prototype.hasOwnProperty.call(
    globalThis,
    "__muonAbortModalFsDialogs",
  )
) {
  Object.defineProperty(globalThis, "__muonAbortModalFsDialogs", {
    configurable: false,
    enumerable: false,
    writable: false,
    value: abortModalOwnerDialogs,
  });
}

const addOwnerCloseAbortListener = (controller) => {
  if (
    typeof globalThis.addEventListener !== "function" ||
    typeof globalThis.removeEventListener !== "function"
  ) {
    return noop;
  }
  const onOwnerClose = () => {
    abortControllerWithReason(controller, createOwnerCloseAbortReason());
  };
  globalThis.addEventListener(ownerCloseEventName, onOwnerClose, {
    once: true,
  });
  globalThis.addEventListener("pagehide", onOwnerClose, { once: true });
  globalThis.addEventListener("beforeunload", onOwnerClose, { once: true });
  return () => {
    globalThis.removeEventListener(ownerCloseEventName, onOwnerClose);
    globalThis.removeEventListener("pagehide", onOwnerClose);
    globalThis.removeEventListener("beforeunload", onOwnerClose);
  };
};

const createModalOwnerAbortOptions = (options) => {
  const source = getOptionsObject(options);
  if (
    source.modal === false ||
    typeof globalThis.AbortController !== "function"
  ) {
    return { options, cleanup: noop };
  }

  const userSignal = getAbortSignal(source);
  const ownerController = new globalThis.AbortController();
  getModalOwnerAbortControllers().add(ownerController);
  const cleanupOwner = addOwnerCloseAbortListener(ownerController);
  if (userSignal === null) {
    return {
      options: { ...source, signal: ownerController.signal },
      cleanup: () => {
        cleanupOwner();
        getModalOwnerAbortControllers().delete(ownerController);
      },
    };
  }

  const compositeController = new globalThis.AbortController();
  const abortFromUser = () => {
    abortControllerWithReason(
      compositeController,
      createAbortReason(userSignal),
    );
  };
  const abortFromOwner = () => {
    abortControllerWithReason(
      compositeController,
      createAbortReason(ownerController.signal),
    );
  };
  if (userSignal.aborted) {
    abortFromUser();
  } else {
    userSignal.addEventListener("abort", abortFromUser, { once: true });
  }
  if (ownerController.signal.aborted) {
    abortFromOwner();
  } else {
    ownerController.signal.addEventListener("abort", abortFromOwner, {
      once: true,
    });
  }
  return {
    options: { ...source, signal: compositeController.signal },
    cleanup: () => {
      cleanupOwner();
      getModalOwnerAbortControllers().delete(ownerController);
      userSignal.removeEventListener("abort", abortFromUser);
      ownerController.signal.removeEventListener("abort", abortFromOwner);
    },
  };
};

const runDialogWithOwnerCloseAbort = (options, operation) => {
  let configured;
  try {
    configured = createModalOwnerAbortOptions(options);
  } catch (error) {
    return Promise.reject(error);
  }
  return Promise.resolve()
    .then(() => operation(configured.options))
    .finally(configured.cleanup);
};

const normalizeStringOption = (source, target, key) => {
  if (source[key] === undefined || source[key] === null) {
    return;
  }
  if (typeof source[key] !== "string") {
    throw new TypeError("options." + key + " must be a string");
  }
  target[key] = source[key];
};

const normalizeBooleanOption = (source, target, key) => {
  if (source[key] === undefined || source[key] === null) {
    return;
  }
  if (typeof source[key] !== "boolean") {
    throw new TypeError("options." + key + " must be a boolean");
  }
  target[key] = source[key];
};

const normalizeStringArray = (value, name) => {
  if (!Array.isArray(value)) {
    throw new TypeError(name + " must be an array");
  }
  return value.map((entry) => {
    if (typeof entry !== "string" || entry.length === 0) {
      throw new TypeError(name + " entries must be non-empty strings");
    }
    return entry;
  });
};

const normalizeDialogFilterExtension = (value) => {
  if (typeof value !== "string") {
    throw new TypeError(
      "options.filters extensions entries must be strings",
    );
  }
  let normalized = value.trim();
  if (normalized.startsWith("*.")) {
    normalized = normalized.slice(2);
  } else if (normalized.startsWith(".")) {
    normalized = normalized.slice(1);
  }
  if (normalized === "*") {
    return normalized;
  }
  if (
    normalized.length === 0 ||
    normalized.includes("*") ||
    normalized.includes("/") ||
    normalized.includes("\\") ||
    normalized.includes("\u0000")
  ) {
    throw new TypeError("options.filters extensions entries are invalid");
  }
  return normalized;
};

const normalizeDialogFilters = (filters) => {
  if (filters === undefined || filters === null) {
    return undefined;
  }
  if (!Array.isArray(filters)) {
    throw new TypeError("options.filters must be an array");
  }
  return filters.map((entry) => {
    if (typeof entry !== "object" || entry === null) {
      throw new TypeError("options.filters entries must be objects");
    }
    if (typeof entry.name !== "string" || entry.name.length === 0) {
      throw new TypeError(
        "options.filters entries require a non-empty name",
      );
    }
    if (!Array.isArray(entry.extensions) || entry.extensions.length === 0) {
      throw new TypeError(
        "options.filters entries require extensions",
      );
    }
    return {
      name: entry.name,
      extensions: entry.extensions.map(normalizeDialogFilterExtension),
    };
  });
};

const normalizeNestedBooleanOptions = (source, target, key, booleanKeys) => {
  const nested = source[key];
  if (nested === undefined || nested === null) {
    return;
  }
  if (typeof nested !== "object") {
    throw new TypeError("options." + key + " must be an object");
  }
  const normalized = {};
  for (const nestedKey of booleanKeys) {
    if (nested[nestedKey] !== undefined && nested[nestedKey] !== null) {
      if (typeof nested[nestedKey] !== "boolean") {
        throw new TypeError(
          "options." + key + "." + nestedKey + " must be a boolean",
        );
      }
      normalized[nestedKey] = nested[nestedKey];
    }
  }
  target[key] = normalized;
};

const normalizeDialogOptions = (options, save) => {
  const source = getOptionsObject(options);
  const target = {};
  normalizeStringOption(source, target, "title");
  normalizeStringOption(source, target, "defaultPath");
  normalizeStringOption(source, target, "buttonLabel");
  normalizeBooleanOption(source, target, "modal");
  normalizeBooleanOption(source, target, "showHidden");
  if (save) {
    normalizeStringOption(source, target, "defaultName");
    normalizeBooleanOption(source, target, "confirmOverwrite");
  }
  const filters = normalizeDialogFilters(source.filters);
  if (filters !== undefined) {
    target.filters = filters;
  }
  normalizeNestedBooleanOptions(source, target, "gtk", [
    "localOnly",
    "createFolders",
  ]);
  if (source.gtk !== undefined && source.gtk !== null) {
    const mimeTypes = source.gtk.mimeTypes;
    if (mimeTypes !== undefined && mimeTypes !== null) {
      target.gtk.mimeTypes = normalizeStringArray(
        mimeTypes,
        "options.gtk.mimeTypes",
      );
    }
  }
  normalizeNestedBooleanOptions(source, target, "win32", [
    "forceFilesystem",
    "noDereferenceLinks",
    "dontAddToRecent",
    "noValidate",
    "strictFileTypes",
    "pathMustExist",
    "fileMustExist",
  ]);
  return target;
};

const parseDialogResults = async (source) => {
  const values = await parseNativeJson(source);
  if (
    !Array.isArray(values) ||
    values.some((entry) => typeof entry !== "string")
  ) {
    throw new TypeError("Native dialog result is invalid");
  }
  return values;
};

const parseSingleDialogResult = async (source) => {
  const values = await parseDialogResults(source);
  return values.length === 0 ? null : values[0];
};

const runSingleDialog = (nativeFunction, options, save) =>
  runDialogWithOwnerCloseAbort(options, (operationOptions) =>
    runAbortable(operationOptions, (abortWatcher) =>
      parseSingleDialogResult(
        nativeFunction(
          JSON.stringify(normalizeDialogOptions(operationOptions, save)),
          abortWatcher,
        ),
      ),
    ),
  );

const runMultipleDialog = (nativeFunction, options) =>
  runDialogWithOwnerCloseAbort(options, (operationOptions) =>
    runAbortable(operationOptions, (abortWatcher) =>
      parseDialogResults(
        nativeFunction(
          JSON.stringify(normalizeDialogOptions(operationOptions, false)),
          abortWatcher,
        ),
      ),
    ),
  );

const properties = {};
if (isAllowed("selectFile")) {
  properties.selectFile = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (options) => runSingleDialog(namespace.__selectFile, options, false),
  };
}
if (isAllowed("selectFiles")) {
  properties.selectFiles = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (options) => runMultipleDialog(namespace.__selectFiles, options),
  };
}
if (isAllowed("selectDirectory")) {
  properties.selectDirectory = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (options) =>
      runSingleDialog(namespace.__selectDirectory, options, false),
  };
}
if (isAllowed("selectDirectories")) {
  properties.selectDirectories = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (options) =>
      runMultipleDialog(namespace.__selectDirectories, options),
  };
}
if (isAllowed("selectSaveFile")) {
  properties.selectSaveFile = {
    enumerable: true,
    configurable: false,
    writable: false,
    value: (options) =>
      runSingleDialog(namespace.__selectSaveFile, options, true),
  };
}
Object.defineProperties(namespace, properties);
)JS";

static const muon_plugin_function_metadata fs_dialog_functions[] = {
    {
        "__selectFile",
        reinterpret_cast<muon_native_function>(
            &muon_builtin_fs_dialogs_select_file),
        {2, options_abort_args, &type_string},
        "selectFile",
    },
    {
        "__selectFiles",
        reinterpret_cast<muon_native_function>(
            &muon_builtin_fs_dialogs_select_files),
        {2, options_abort_args, &type_string},
        "selectFiles",
    },
    {
        "__selectDirectory",
        reinterpret_cast<muon_native_function>(
            &muon_builtin_fs_dialogs_select_directory),
        {2, options_abort_args, &type_string},
        "selectDirectory",
    },
    {
        "__selectDirectories",
        reinterpret_cast<muon_native_function>(
            &muon_builtin_fs_dialogs_select_directories),
        {2, options_abort_args, &type_string},
        "selectDirectories",
    },
    {
        "__selectSaveFile",
        reinterpret_cast<muon_native_function>(
            &muon_builtin_fs_dialogs_select_save_file),
        {2, options_abort_args, &type_string},
        "selectSaveFile",
    },
};

static const muon_plugin_function_metadata* const
    fs_dialog_function_pointers[] = {
    &fs_dialog_functions[0],
    &fs_dialog_functions[1],
    &fs_dialog_functions[2],
    &fs_dialog_functions[3],
    &fs_dialog_functions[4],
    nullptr,
};

}  // namespace muon_internal

bool InitializeMuonBuiltinFsDialogs(const muon_plugin_init_context* context,
                                    std::string* error_message) {
  if (error_message == nullptr) {
    return false;
  }
  const auto* helpers = context == nullptr ? nullptr : context->helpers;
  if (helpers == nullptr) {
    *error_message = "Filesystem dialog helpers are unavailable";
    return false;
  }
  if (muon_ui_fs_dialogs_initialize() != 0) {
    *error_message = "Filesystem dialog provider initialization failed";
    return false;
  }
  muon_internal::g_fs_dialogs_helpers = helpers;
  return true;
}

void ShutdownMuonBuiltinFsDialogs() {
  muon_internal::g_fs_dialogs_helpers = nullptr;
}

extern "C" void muon_builtin_fs_dialogs_cancel_owner_browser(
    int owner_browser_id) {
  muon_ui_fs_dialogs_cancel_owner_browser(owner_browser_id);
}

const muon_plugin_namespace kMuonBuiltinFsDialogsNamespace = {
    "muon.fs.dialogs",
    muon_internal::fs_dialogs_setup_script,
    muon_internal::fs_dialog_function_pointers,
};

static const muon_plugin_namespace* const fs_dialog_namespaces[] = {
    &kMuonBuiltinFsDialogsNamespace,
    nullptr,
};

static const muon_plugin_metadata fs_dialog_metadata = {
    fs_dialog_namespaces,
    nullptr,
};

const muon_plugin_metadata* GetMuonBuiltinFsDialogsPluginMetadata() {
  return &fs_dialog_metadata;
}

extern "C" const muon_plugin_metadata* muon_init_plugin(
    const muon_plugin_init_context* context) {
  auto error_message = std::string{};
  if (!InitializeMuonBuiltinFsDialogs(context, &error_message)) {
    return nullptr;
  }
  return GetMuonBuiltinFsDialogsPluginMetadata();
}
