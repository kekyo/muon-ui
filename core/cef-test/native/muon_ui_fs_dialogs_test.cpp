/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "ui/muon_ui_fs_dialogs.h"

#include <gtk/gtk.h>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

struct Result {
  int calls = 0;
  std::string paths;
  std::string error;
  bool window_visible_on_completion = false;
  const char* title = "";
};

static GtkWidget* FindDialog(const char* title) {
  auto* windows = gtk_window_list_toplevels();
  GtkWidget* result = nullptr;
  for (auto* entry = windows; entry != nullptr; entry = entry->next) {
    auto* window = GTK_WINDOW(entry->data);
    if (g_strcmp0(gtk_window_get_title(window), title) == 0 &&
        gtk_widget_get_visible(GTK_WIDGET(window))) {
      result = GTK_WIDGET(window);
      break;
    }
  }
  g_list_free(windows);
  return result;
}

static void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}

static void Completed(void* data, const char* paths, const char* error) {
  auto& result = *static_cast<Result*>(data);
  ++result.calls;
  result.paths = paths == nullptr ? "" : paths;
  result.error = error == nullptr ? "" : error;
  result.window_visible_on_completion = FindDialog(result.title) != nullptr;
}

static void WaitFor(const std::function<bool()>& condition) {
  bool timed_out = false;
  const auto timeout = g_timeout_add_seconds(30, [](gpointer data) -> gboolean {
    *static_cast<bool*>(data) = true;
    return G_SOURCE_REMOVE;
  }, &timed_out);
  while (!condition() && !timed_out) {
    g_main_context_iteration(nullptr, TRUE);
  }
  Check(!timed_out, "Timed out waiting for a dialog state change");
  g_source_remove(timeout);
}

static muon_ui_fs_dialog_operation_handle Start(Result& result, int owner) {
  auto handle = muon_ui_fs_dialog_operation_handle{};
  const auto options = std::string("{\"title\":\"") + result.title + "\"}";
  Check(muon_ui_fs_dialogs_run(MUON_UI_FS_DIALOG_SELECT_FILE, options.c_str(),
                               owner, Completed, &result, &handle) == 0,
        "The dialog should be accepted");
  Check(handle != nullptr, "An accepted operation must have a handle");
  return handle;
}

static void TestDeferredStart() {
  Result result;
  result.title = "deferred";
  auto handle = muon_ui_fs_dialog_operation_handle{};
  // A synchronous implementation dispatches this rescue source from its nested
  // loop. An asynchronous implementation returns without dispatching it at all.
  const auto rescue = g_idle_add([](gpointer data) -> gboolean {
    muon_ui_fs_dialogs_cancel(
        *static_cast<muon_ui_fs_dialog_operation_handle*>(data));
    return G_SOURCE_CONTINUE;
  }, &handle);
  Check(g_main_context_acquire(nullptr), "Cannot acquire the UI context");
  Check(muon_ui_fs_dialogs_run(MUON_UI_FS_DIALOG_SELECT_FILE,
                               "{\"title\":\"deferred\"}", 1,
                               Completed, &result, &handle) == 0,
        "The dialog should be accepted");
  g_source_remove(rescue);
  g_main_context_release(nullptr);
  Check(result.calls == 0, "Starting a dialog must return before completion");
  Check(FindDialog(result.title) == nullptr,
        "Starting a dialog must defer creation until UI dispatch");
  muon_ui_fs_dialogs_cancel_owner_browser(1);
  WaitFor([&result]() { return result.calls == 1; });
  Check(result.paths == "[]" && result.error.empty(),
        "Canceling a queued dialog should return an empty selection");
  Check(FindDialog(result.title) == nullptr,
        "Canceling a queued dialog must prevent its presentation");
}

static void TestIndependentOwners() {
  Result first, second, other;
  first.title = "first owner first dialog";
  second.title = "first owner second dialog";
  other.title = "second owner";
  Start(first, 10);
  Start(second, 10);
  auto other_handle = Start(other, 20);
  WaitFor([&]() {
    return FindDialog(first.title) && FindDialog(second.title) &&
           FindDialog(other.title);
  });
  Check(!gtk_window_get_modal(GTK_WINDOW(FindDialog(first.title))) &&
            !gtk_window_get_modal(GTK_WINDOW(FindDialog(other.title))),
        "File dialogs must not grab GTK's unrelated windows");
  // Closing the earliest dialog must complete even while later ones remain.
  gtk_dialog_response(GTK_DIALOG(FindDialog(first.title)), GTK_RESPONSE_CANCEL);
  Check(first.calls == 1 && second.calls == 0 && other.calls == 0,
        "Dialog completion must be independent of presentation order");
  muon_ui_fs_dialogs_cancel_owner_browser(10);
  WaitFor([&]() { return second.calls == 1; });
  Check(other.calls == 0 && FindDialog(other.title),
        "Canceling one owner must preserve another owner's dialog");
  muon_ui_fs_dialogs_cancel(other_handle);
  WaitFor([&]() { return other.calls == 1; });
  Check(!first.window_visible_on_completion &&
            !second.window_visible_on_completion &&
            !other.window_visible_on_completion,
        "Completion must follow removal of the visible dialog");
}

static void TestTerminationSignals() {
  for (int mode = 0; mode < 3; ++mode) {
    Result result;
    result.title = "termination";
    Start(result, 30);
    WaitFor([&]() { return FindDialog(result.title) != nullptr; });
    auto* dialog = FindDialog(result.title);
    g_object_ref(dialog);
    if (mode == 0) {
      gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_DELETE_EVENT);
    } else if (mode == 1) {
      gtk_widget_hide(dialog);
    } else {
      gtk_widget_destroy(dialog);
    }
    gtk_widget_destroy(dialog);
    g_object_unref(dialog);
    WaitFor([&]() { return result.calls != 0; });
    Check(result.calls == 1 && result.paths == "[]" &&
              !result.window_visible_on_completion,
          "Response, hide and destroy must complete exactly once");
  }
}

static void TestFailureAndShutdown() {
  Result invalid;
  auto handle = muon_ui_fs_dialog_operation_handle{};
  Check(muon_ui_fs_dialogs_run(MUON_UI_FS_DIALOG_SELECT_FILE,
                               "{\"filters\":false}", 40,
                               Completed, &invalid, &handle) == 0,
        "Invalid options should fail through the completion callback");
  WaitFor([&]() { return invalid.calls == 1; });
  Check(!invalid.error.empty(), "Invalid options must report a failure");
  Result shown, queued;
  shown.title = "shutdown shown";
  queued.title = "shutdown queued";
  Start(shown, 40);
  WaitFor([&]() { return FindDialog(shown.title) != nullptr; });
  Start(queued, 50);
  muon_ui_fs_dialogs_shutdown();
  Check(shown.calls == 1 && queued.calls == 1,
        "Shutdown must finish shown and queued operations");
  while (g_main_context_pending(nullptr)) {
    g_main_context_iteration(nullptr, FALSE);
  }
  Check(shown.calls == 1 && queued.calls == 1 &&
            FindDialog(shown.title) == nullptr &&
            FindDialog(queued.title) == nullptr,
        "Pending dispatch must not revive operations after shutdown");
  Check(muon_ui_fs_dialogs_initialize() == 0, "Reinitialization failed");
}

static void TestReentrantShutdown() {
  Result first, second;
  first.title = "reentrant first";
  second.title = "reentrant second";
  auto handle = muon_ui_fs_dialog_operation_handle{};
  Check(muon_ui_fs_dialogs_run(MUON_UI_FS_DIALOG_SELECT_FILE,
                               "{\"title\":\"reentrant first\"}", 60,
                               [](void* data, const char* paths, const char* error) {
                                 Completed(data, paths, error);
                                 muon_ui_fs_dialogs_shutdown();
                               }, &first, &handle) == 0,
        "Reentrant operation should start");
  Start(second, 60);
  WaitFor([&]() { return FindDialog(first.title) && FindDialog(second.title); });
  muon_ui_fs_dialogs_cancel_owner_browser(60);
  Check(first.calls == 1 && second.calls == 1,
        "A completion callback may shut down other operations exactly once");
  Check(muon_ui_fs_dialogs_run(MUON_UI_FS_DIALOG_SELECT_FILE, "{}", 60,
                               Completed, &first, &handle) != 0,
        "Shutdown must reject further operations until reinitialized");
}

int main(int argc, char** argv) {
  if (argc == 2 && std::string(argv[1]) == "--no-display") {
    Check(muon_ui_fs_dialogs_initialize() == 0, "UI initialization failed");
    Result result;
    result.title = "creation failure";
    Start(result, 70);
    WaitFor([&]() { return result.calls != 0; });
    Check(result.calls == 1 && !result.error.empty(),
          "Creation failure must complete once with an error");
    muon_ui_fs_dialogs_cancel_owner_browser(70);
    muon_ui_fs_dialogs_shutdown();
    Check(result.calls == 1, "Creation failure must release its owner");
    return 0;
  }
  Check(gtk_init_check(&argc, &argv), "GTK initialization failed");
  Check(muon_ui_fs_dialogs_initialize() == 0, "UI initialization failed");
  TestDeferredStart();
  TestIndependentOwners();
  TestTerminationSignals();
  TestFailureAndShutdown();
  TestReentrantShutdown();
  muon_ui_fs_dialogs_shutdown();
  return 0;
}
