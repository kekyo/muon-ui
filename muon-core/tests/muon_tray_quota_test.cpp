/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_tray.h"

#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <utility>

struct FakeTrayPlatform {
  std::set<std::pair<int, std::string>> active_records;
  int create_attempts = 0;
  bool fail_next_create = false;

  bool CreateRecord(int browser_id,
                    const std::string& tray_id,
                    std::string* error_message) {
    ++create_attempts;
    if (fail_next_create) {
      fail_next_create = false;
      if (error_message != nullptr) {
        *error_message = "fake platform create failed";
      }
      return false;
    }
    active_records.insert({browser_id, tray_id});
    return true;
  }

  void DestroyRecord(int browser_id, const std::string& tray_id) {
    active_records.erase({browser_id, tray_id});
  }
};

static bool Expect(bool condition, const std::string& message) {
  if (!condition) {
    std::fprintf(stderr, "%s\n", message.c_str());
    return false;
  }
  return true;
}

static bool ExpectEqual(const std::string& actual,
                        const std::string& expected,
                        const std::string& message) {
  if (actual == expected) {
    return true;
  }
  std::fprintf(stderr, "%s: expected '%s', got '%s'\n", message.c_str(),
               expected.c_str(), actual.c_str());
  return false;
}

static bool ExpectEqualSize(size_t actual,
                            size_t expected,
                            const std::string& message) {
  if (actual == expected) {
    return true;
  }
  std::fprintf(stderr, "%s: expected %zu, got %zu\n", message.c_str(),
               expected, actual);
  return false;
}

static bool ExpectEqualInt(int actual,
                           int expected,
                           const std::string& message) {
  if (actual == expected) {
    return true;
  }
  std::fprintf(stderr, "%s: expected %d, got %d\n", message.c_str(), expected,
               actual);
  return false;
}

static MuonBrowserTrayIcon CreateIcon(uint8_t red) {
  MuonIconBitmap bitmap;
  bitmap.pixel_width = 1;
  bitmap.pixel_height = 1;
  bitmap.rgba = {red, 2, 3, 4};
  MuonBrowserTrayIcon icon;
  icon.bitmap = std::make_shared<const MuonIconBitmap>(std::move(bitmap));
  return icon;
}

static std::unique_ptr<MuonBrowserTrayService> CreateService(
    FakeTrayPlatform* platform,
    MuonBrowserTrayLimits limits) {
  MuonBrowserTrayServiceOptions options;
  options.linux_desktop_id = "muon-test";
  options.limits = limits;
  options.platform_hooks.create_record =
      [platform](int browser_id,
                 const std::string& tray_id,
                 std::string* error_message) {
        return platform->CreateRecord(browser_id, tray_id, error_message);
      };
  options.platform_hooks.destroy_record =
      [platform](int browser_id, const std::string& tray_id) {
        platform->DestroyRecord(browser_id, tray_id);
      };
  return CreateMuonBrowserTrayService(std::move(options));
}

static bool CreateTray(MuonBrowserTrayService* service,
                       int browser_id,
                       const std::string& requested_id,
                       uint8_t icon_seed,
                       std::string* created_id,
                       std::string* error_message) {
  MuonBrowserTrayOptions options;
  options.id = requested_id;
  created_id->clear();
  error_message->clear();
  return service->CreateTray(browser_id, options, CreateIcon(icon_seed),
                             [](const MuonBrowserTrayEvent&) {}, created_id,
                             error_message);
}

static bool TestPerBrowserLimitAndRelease() {
  FakeTrayPlatform platform;
  MuonBrowserTrayLimits limits;
  limits.max_per_browser = 2;
  limits.max_global = 8;
  auto service = CreateService(&platform, limits);

  std::string tray_id;
  std::string error_message;
  if (!Expect(CreateTray(service.get(), 7, "one", 1, &tray_id, &error_message),
              error_message) ||
      !Expect(CreateTray(service.get(), 7, "two", 2, &tray_id, &error_message),
              error_message) ||
      !ExpectEqualSize(platform.active_records.size(), 2,
                       "per-browser setup active count") ||
      !ExpectEqualInt(platform.create_attempts, 2,
                      "per-browser setup create attempts")) {
    return false;
  }

  if (!Expect(!CreateTray(service.get(), 7, "three", 3, &tray_id,
                          &error_message),
              "per-browser quota create unexpectedly succeeded") ||
      !ExpectEqual(error_message, "Browser tray limit exceeded",
                   "per-browser quota error") ||
      !ExpectEqualInt(platform.create_attempts, 2,
                      "per-browser quota called platform backend") ||
      !ExpectEqualSize(platform.active_records.size(), 2,
                       "per-browser quota active count")) {
    return false;
  }

  if (!Expect(service->RemoveTray(7, "one", &error_message),
              error_message) ||
      !ExpectEqualSize(platform.active_records.size(), 1,
                       "RemoveTray did not release one active record") ||
      !Expect(CreateTray(service.get(), 7, "three", 4, &tray_id,
                         &error_message),
              error_message) ||
      !ExpectEqualSize(platform.active_records.size(), 2,
                       "replacement create active count")) {
    return false;
  }

  service->RemoveTraysForBrowser(7);
  if (!ExpectEqualSize(platform.active_records.size(), 0,
                       "RemoveTraysForBrowser did not release browser records") ||
      !Expect(CreateTray(service.get(), 7, "four", 5, &tray_id,
                         &error_message),
              error_message) ||
      !ExpectEqualSize(platform.active_records.size(), 1,
                       "browser cleanup did not restore quota")) {
    return false;
  }

  service->RemoveTraysForBrowser(7);
  return ExpectEqualSize(platform.active_records.size(), 0,
                         "final browser cleanup active count");
}

static bool TestGlobalLimitAcrossBrowsers() {
  FakeTrayPlatform platform;
  MuonBrowserTrayLimits limits;
  limits.max_per_browser = 2;
  limits.max_global = 3;
  auto service = CreateService(&platform, limits);

  std::string tray_id;
  std::string error_message;
  if (!Expect(CreateTray(service.get(), 7, "one", 1, &tray_id, &error_message),
              error_message) ||
      !Expect(CreateTray(service.get(), 7, "two", 2, &tray_id, &error_message),
              error_message) ||
      !Expect(CreateTray(service.get(), 8, "three", 3, &tray_id,
                         &error_message),
              error_message)) {
    return false;
  }

  if (!Expect(!CreateTray(service.get(), 8, "four", 4, &tray_id,
                          &error_message),
              "global quota create unexpectedly succeeded") ||
      !ExpectEqual(error_message, "Global tray limit exceeded",
                   "global quota error") ||
      !ExpectEqualInt(platform.create_attempts, 3,
                      "global quota called platform backend") ||
      !ExpectEqualSize(platform.active_records.size(), 3,
                       "global quota active count")) {
    return false;
  }

  if (!Expect(service->RemoveTray(7, "one", &error_message),
              error_message) ||
      !Expect(CreateTray(service.get(), 8, "four", 5, &tray_id,
                         &error_message),
              error_message) ||
      !ExpectEqualSize(platform.active_records.size(), 3,
                       "global release replacement active count")) {
    return false;
  }

  service->RemoveTraysForBrowser(7);
  service->RemoveTraysForBrowser(8);
  return ExpectEqualSize(platform.active_records.size(), 0,
                         "final global cleanup active count");
}

static bool TestPlatformFailureRollback() {
  FakeTrayPlatform platform;
  platform.fail_next_create = true;
  MuonBrowserTrayLimits limits;
  limits.max_per_browser = 2;
  limits.max_global = 4;
  auto service = CreateService(&platform, limits);

  std::string tray_id;
  std::string error_message;
  if (!Expect(!CreateTray(service.get(), 7, "flaky", 1, &tray_id,
                          &error_message),
              "fake platform failure unexpectedly succeeded") ||
      !ExpectEqual(error_message, "fake platform create failed",
                   "fake platform failure error") ||
      !ExpectEqualSize(platform.active_records.size(), 0,
                       "failed platform create left active resources")) {
    return false;
  }

  if (!Expect(CreateTray(service.get(), 7, "flaky", 2, &tray_id,
                         &error_message),
              error_message) ||
      !ExpectEqualSize(platform.active_records.size(), 1,
                       "retry after platform failure active count") ||
      !ExpectEqualInt(platform.create_attempts, 2,
                      "retry after platform failure create attempts")) {
    return false;
  }

  service->RemoveTraysForBrowser(7);
  return ExpectEqualSize(platform.active_records.size(), 0,
                         "platform failure cleanup active count");
}

static bool TestDuplicateAndGeneratedIds() {
  FakeTrayPlatform platform;
  MuonBrowserTrayLimits limits;
  limits.max_per_browser = 1;
  limits.max_global = 4;
  auto service = CreateService(&platform, limits);

  std::string tray_id;
  std::string error_message;
  if (!Expect(CreateTray(service.get(), 7, "same", 1, &tray_id,
                         &error_message),
              error_message)) {
    return false;
  }

  if (!Expect(!CreateTray(service.get(), 7, "same", 2, &tray_id,
                          &error_message),
              "duplicate id unexpectedly succeeded") ||
      !ExpectEqual(error_message, "Tray id is duplicated: same",
                   "duplicate id error") ||
      !ExpectEqualInt(platform.create_attempts, 1,
                      "duplicate id called platform backend")) {
    return false;
  }

  if (!Expect(!CreateTray(service.get(), 7, "", 3, &tray_id, &error_message),
              "generated id quota create unexpectedly succeeded") ||
      !ExpectEqual(error_message, "Browser tray limit exceeded",
                   "generated id quota error") ||
      !ExpectEqual(tray_id, "", "generated id quota returned an id") ||
      !ExpectEqualInt(platform.create_attempts, 1,
                      "generated id quota called platform backend")) {
    return false;
  }

  if (!Expect(service->RemoveTray(7, "same", &error_message),
              error_message) ||
      !Expect(CreateTray(service.get(), 7, "", 4, &tray_id, &error_message),
              error_message) ||
      !ExpectEqual(tray_id, "tray-7-1",
                   "generated id quota failure consumed an id")) {
    return false;
  }

  service->RemoveTraysForBrowser(7);
  return ExpectEqualSize(platform.active_records.size(), 0,
                         "generated id cleanup active count");
}

int main() {
  if (!TestPerBrowserLimitAndRelease() ||
      !TestGlobalLimitAcrossBrowsers() ||
      !TestPlatformFailureRollback() ||
      !TestDuplicateAndGeneratedIds()) {
    return 1;
  }
  return 0;
}
