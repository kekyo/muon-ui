#include "browser/show_dev_tools_task.h"

#include "include/cef_client.h"
#include "include/wrapper/cef_helpers.h"

ShowDevToolsTask::ShowDevToolsTask(CefRefPtr<CefBrowser> browser)
    : browser_(browser) {}

void ShowDevToolsTask::Execute() {
  CEF_REQUIRE_UI_THREAD();

  if (!browser_) {
    return;
  }

  CefWindowInfo window_info;
  CefRefPtr<CefClient> devtools_client;
  CefBrowserSettings browser_settings;
  browser_->GetHost()->ShowDevTools(window_info, devtools_client,
                                    browser_settings, CefPoint());
}
