/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#if defined(_WIN32)
#if defined(MUON_UI_BUILDING_LIBRARY)
#define MUON_UI_API __declspec(dllexport)
#else
#define MUON_UI_API __declspec(dllimport)
#endif
#else
#define MUON_UI_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Native filesystem dialog operation type exposed by libmuon-ui.
 */
typedef enum muon_ui_fs_dialog_kind {
  MUON_UI_FS_DIALOG_SELECT_FILE = 0,
  MUON_UI_FS_DIALOG_SELECT_FILES = 1,
  MUON_UI_FS_DIALOG_SELECT_DIRECTORY = 2,
  MUON_UI_FS_DIALOG_SELECT_DIRECTORIES = 3,
  MUON_UI_FS_DIALOG_SELECT_SAVE_FILE = 4,
} muon_ui_fs_dialog_kind;

/**
 * Opaque handle for an active filesystem dialog operation.
 */
typedef struct muon_ui_fs_dialog_operation*
    muon_ui_fs_dialog_operation_handle;

/**
 * Completion callback for filesystem dialog operations.
 *
 * @param user_data Callback state supplied to muon_ui_fs_dialogs_run.
 * @param result_json JSON string array on success, or null on failure.
 * @param error_message UTF-8 error message on failure, or null on success.
 */
typedef void (*muon_ui_fs_dialog_completion)(
    void* user_data,
    const char* result_json,
    const char* error_message);

/**
 * Initializes libmuon-ui filesystem dialogs.
 *
 * @return 0 on success.
 */
MUON_UI_API int muon_ui_fs_dialogs_initialize(void);

/**
 * Shuts down libmuon-ui filesystem dialogs and cancels active operations.
 */
MUON_UI_API void muon_ui_fs_dialogs_shutdown(void);

/**
 * Runs a filesystem dialog asynchronously.
 *
 * @param kind Dialog operation to run.
 * @param options_json JSON object string with normalized dialog options.
 * @param owner_browser_id CEF browser identifier associated with the dialog.
 * @param completion Completion callback.
 * @param user_data Callback state passed to completion.
 * @param out_operation Receives an operation handle for cancellation.
 * @return 0 when the operation was accepted.
 */
MUON_UI_API int muon_ui_fs_dialogs_run(
    muon_ui_fs_dialog_kind kind,
    const char* options_json,
    int owner_browser_id,
    muon_ui_fs_dialog_completion completion,
    void* user_data,
    muon_ui_fs_dialog_operation_handle* out_operation);

/**
 * Cancels an active filesystem dialog operation.
 */
MUON_UI_API void muon_ui_fs_dialogs_cancel(
    muon_ui_fs_dialog_operation_handle operation);

/**
 * Cancels active filesystem dialogs owned by a browser.
 */
MUON_UI_API void muon_ui_fs_dialogs_cancel_owner_browser(
    int owner_browser_id);

#ifdef __cplusplus
}
#endif

