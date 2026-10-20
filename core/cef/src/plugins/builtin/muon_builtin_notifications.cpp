/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include "plugins/builtin/muon_builtin_notifications.h"
#include "config/muon_config.h"

#if defined(__linux__)
#include "notifications/muon_notification_linux.h"
#include "plugins/builtin/muon_builtin_completion.h"
#include "muon_notification_renderer_generated.h"
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <type_traits>

using muon_internal::MuonNotificationService;
static std::shared_ptr<MuonNotificationService> notification_service;

// Arguments and shared service ownership remain in the coroutine until the
// native completion is called, including event waits that outlive a page.
template <typename Result, typename... Arguments>
static cardio::promise<void> InvokeNotification(
    std::shared_ptr<MuonNotificationService> service,
    cardio::promise<Result> (MuonNotificationService::*method)(Arguments...),
    muon_completion_func completion, Arguments... arguments) {
  try {
    if (!service) throw std::runtime_error("Notifications are disabled");
    if constexpr (std::is_void_v<Result>) {
      co_await ((*service).*method)(std::move(arguments)...);
      muon_internal::CompleteMuonVoid(completion);
    } else {
      const auto result = co_await ((*service).*method)(std::move(arguments)...);
      muon_internal::CompleteMuonString(completion, result);
    }
  } catch (const std::exception& error) {
    muon_internal::CompleteMuonError(completion, error.what());
  } catch (...) {
    muon_internal::CompleteMuonError(completion, "Notification operation failed");
  }
}
static void Post(muon_completion_func completion, const char* json) {
  cardio::fire_and_forget(InvokeNotification(notification_service,
      &MuonNotificationService::Post, completion, std::string(json == nullptr ? "" : json)));
}
static void Cancel(muon_completion_func completion, const char* id) {
  cardio::fire_and_forget(InvokeNotification(notification_service,
      &MuonNotificationService::Cancel, completion, std::string(id == nullptr ? "" : id)));
}
static void GetStatus(muon_completion_func completion, const char* json) {
  cardio::fire_and_forget(InvokeNotification(notification_service,
      &MuonNotificationService::GetStatus, completion, std::string(json == nullptr ? "" : json)));
}
static void GetCapabilities(muon_completion_func completion) {
  cardio::fire_and_forget(InvokeNotification(notification_service,
      &MuonNotificationService::GetCapabilities, completion));
}
static void GetPendingEvents(muon_completion_func completion) {
  cardio::fire_and_forget(InvokeNotification(notification_service,
      &MuonNotificationService::GetPendingEvents, completion));
}
static void AcknowledgeEvent(muon_completion_func completion, const char* token) {
  cardio::fire_and_forget(InvokeNotification(notification_service,
      &MuonNotificationService::AcknowledgeEvent, completion, std::string(token == nullptr ? "" : token)));
}
static void WatchEvents(muon_completion_func completion, const char* json, const char* owner) {
  cardio::fire_and_forget(InvokeNotification(notification_service,
      &MuonNotificationService::WatchEvents, completion, std::string(json == nullptr ? "" : json), std::string(owner == nullptr ? "" : owner)));
}
static cardio::promise<void> StopAsync(std::shared_ptr<MuonNotificationService> service,
    muon_plugin_stop_completion completion, void* data) {
  try { if (service) co_await service->Stop(); }
  catch (const std::exception& error) {
    std::fprintf(stderr, "muon notifications shutdown failed: %s\n", error.what());
  }
  completion(data);
}
static void Stop(muon_plugin_stop_completion completion, void* data) {
  cardio::fire_and_forget(StopAsync(notification_service, completion, data));
}
static void ReleaseOwner(const char* owner) {
  if (notification_service) notification_service->ReleaseOwner(owner);
}
static const muon_type_descriptor string_type = {MUON_TYPE_STRING, nullptr};
static const muon_type_descriptor void_type = {MUON_TYPE_VOID, nullptr};
static const muon_type_descriptor string_arguments[] = {string_type, string_type};
static const muon_plugin_function_metadata post_function = {
    "__post", reinterpret_cast<muon_native_function>(&Post), {1, string_arguments, &void_type}, "post"};
static const muon_plugin_function_metadata cancel_function = {
    "__cancel", reinterpret_cast<muon_native_function>(&Cancel), {1, string_arguments, &void_type}, "cancel"};
static const muon_plugin_function_metadata status_function = {
    "__getStatus", reinterpret_cast<muon_native_function>(&GetStatus), {1, string_arguments, &string_type}, "getStatus"};
static const muon_plugin_function_metadata capabilities_function = {
    "__getCapabilities", reinterpret_cast<muon_native_function>(&GetCapabilities), {0, nullptr, &string_type}, "getCapabilities"};
static const muon_plugin_function_metadata pending_function = {
    "__getPendingEvents", reinterpret_cast<muon_native_function>(&GetPendingEvents), {0, nullptr, &string_type}, "getPendingEvents"};
static const muon_plugin_function_metadata acknowledge_function = {
    "__acknowledgeEvent", reinterpret_cast<muon_native_function>(&AcknowledgeEvent), {1, string_arguments, &void_type}, "acknowledgeEvent"};
static const muon_plugin_function_metadata watch_function = {
    "__watchEvents", reinterpret_cast<muon_native_function>(&WatchEvents), {2, string_arguments, &string_type}, "watchEvents"};
static const muon_plugin_function_metadata* const functions[] = {
    &post_function, &cancel_function, &status_function, &capabilities_function,
    &pending_function, &acknowledge_function, &watch_function, nullptr};
static const muon_plugin_namespace notification_namespace = {
    "muon.notifications", kMuonNotificationSetupScript, functions};
static const muon_plugin_namespace* const namespaces[] = {&notification_namespace, nullptr};
static const muon_plugin_metadata metadata = {namespaces, &Stop, &ReleaseOwner};
#endif

void InitializeMuonBuiltinNotifications(const MuonConfig& config) {
#if defined(__linux__)
  notification_service.reset();
  if (config.notifications.enabled) {
    const auto path = (config.browser.profile / "notifications.json").string();
    auto backend = muon_internal::CreateMuonLinuxNotificationBackend(config.app_id, config.desktop_id);
    notification_service = std::make_shared<MuonNotificationService>(path,
        config.notifications.default_category, std::move(backend), [] {
          return std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count();
        });
    notification_service->Start();
  }
#else
  (void)config;
#endif
}
const muon_plugin_metadata* GetMuonBuiltinNotificationsMetadata() {
#if defined(__linux__)
  return notification_service ? &metadata : nullptr;
#else
  return nullptr;
#endif
}
void ShutdownMuonBuiltinNotifications() {
#if defined(__linux__)
  notification_service.reset();
#endif
}
