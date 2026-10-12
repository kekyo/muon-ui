/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "browser/muon_tray.h"

#include "muon_json_helpers.h"

#include "yyjson.h"

#include <set>
#include <string>
#include <utility>

using muon_internal::AppendJsonString;
using muon_internal::MuonJsonDocument;
using muon_internal::ReadJsonString;

static constexpr char kMuonTrayTypeKey[] = "type";
static constexpr char kMuonTrayTypeItem[] = "item";
static constexpr char kMuonTrayTypeSeparator[] = "separator";
static constexpr char kMuonTrayTypeCheckbox[] = "checkbox";
static constexpr char kMuonTrayTypeRadio[] = "radio";
static constexpr char kMuonTrayIdKey[] = "id";
static constexpr char kMuonTrayIconKey[] = "icon";
static constexpr char kMuonTrayTooltipKey[] = "tooltip";
static constexpr char kMuonTrayMenuKey[] = "menu";
static constexpr char kMuonTrayLabelKey[] = "label";
static constexpr char kMuonTrayEnabledKey[] = "enabled";
static constexpr char kMuonTrayCheckedKey[] = "checked";

static bool IsMuonBrowserTrayIdReserved(const std::string& id) {
  return id.rfind("muon.", 0) == 0 || id.rfind("standard.", 0) == 0;
}

static bool ContainsControlCharacter(const std::string& value) {
  for (const auto character : value) {
    if (static_cast<unsigned char>(character) < 0x20) {
      return true;
    }
  }
  return false;
}

bool IsValidMuonBrowserTrayId(const std::string& id) {
  return !id.empty() && !ContainsControlCharacter(id) &&
         !IsMuonBrowserTrayIdReserved(id);
}

static bool ParseTrayMenuItemType(yyjson_val* value,
                                  MuonBrowserTrayMenuItemType* type,
                                  std::string* error_message) {
  if (value == nullptr) {
    *type = kMuonBrowserTrayMenuItemCommand;
    return true;
  }
  if (!yyjson_is_str(value)) {
    *error_message = "Tray menu item type must be a string";
    return false;
  }
  const auto raw_type = ReadJsonString(value);
  if (raw_type == kMuonTrayTypeItem) {
    *type = kMuonBrowserTrayMenuItemCommand;
    return true;
  }
  if (raw_type == kMuonTrayTypeSeparator) {
    *type = kMuonBrowserTrayMenuItemSeparator;
    return true;
  }
  if (raw_type == kMuonTrayTypeCheckbox) {
    *type = kMuonBrowserTrayMenuItemCheckbox;
    return true;
  }
  if (raw_type == kMuonTrayTypeRadio) {
    *type = kMuonBrowserTrayMenuItemRadio;
    return true;
  }
  *error_message = "Tray menu item type has unknown value: " + raw_type;
  return false;
}

static bool ParseTrayMenuCommandItem(yyjson_val* value,
                                     MuonBrowserTrayMenuItem* item,
                                     std::set<std::string>* used_ids,
                                     std::string* error_message) {
  const auto id = yyjson_obj_get(value, kMuonTrayIdKey);
  if (!yyjson_is_str(id)) {
    *error_message = "Tray menu item id must be a string";
    return false;
  }
  item->id = ReadJsonString(id);
  if (!IsValidMuonBrowserTrayId(item->id)) {
    *error_message = "Tray menu item id is invalid: " + item->id;
    return false;
  }
  if (!used_ids->insert(item->id).second) {
    *error_message = "Tray menu item id is duplicated: " + item->id;
    return false;
  }

  const auto label = yyjson_obj_get(value, kMuonTrayLabelKey);
  if (!yyjson_is_str(label)) {
    *error_message = "Tray menu item label must be a string";
    return false;
  }
  item->label = ReadJsonString(label);
  if (item->label.empty()) {
    *error_message = "Tray menu item label must not be empty";
    return false;
  }

  const auto enabled = yyjson_obj_get(value, kMuonTrayEnabledKey);
  if (enabled != nullptr) {
    if (!yyjson_is_bool(enabled)) {
      *error_message = "Tray menu item enabled must be a boolean";
      return false;
    }
    item->enabled = yyjson_get_bool(enabled);
  }

  const auto checked = yyjson_obj_get(value, kMuonTrayCheckedKey);
  if (checked != nullptr) {
    if (!yyjson_is_bool(checked)) {
      *error_message = "Tray menu item checked must be a boolean";
      return false;
    }
    if (item->type == kMuonBrowserTrayMenuItemCommand) {
      *error_message = "Tray menu item checked requires checkbox or radio";
      return false;
    }
    item->checked = yyjson_get_bool(checked);
  }
  return true;
}

static bool ParseTrayMenuItem(yyjson_val* value,
                              MuonBrowserTrayMenuItem* item,
                              std::set<std::string>* used_ids,
                              std::string* error_message) {
  if (!yyjson_is_obj(value)) {
    *error_message = "Tray menu item must be an object";
    return false;
  }
  *item = {};
  if (!ParseTrayMenuItemType(yyjson_obj_get(value, kMuonTrayTypeKey),
                             &item->type, error_message)) {
    return false;
  }
  if (item->type == kMuonBrowserTrayMenuItemSeparator) {
    return true;
  }
  return ParseTrayMenuCommandItem(value, item, used_ids, error_message);
}

static bool ParseTrayMenuItemsArray(yyjson_val* root,
                                    std::vector<MuonBrowserTrayMenuItem>* items,
                                    std::string* error_message) {
  if (!yyjson_is_arr(root)) {
    *error_message = "Tray menu items JSON root must be an array";
    return false;
  }

  std::set<std::string> used_ids;
  const auto item_count = yyjson_arr_size(root);
  for (auto index = size_t{0}; index < item_count; ++index) {
    MuonBrowserTrayMenuItem item;
    if (!ParseTrayMenuItem(yyjson_arr_get(root, index), &item, &used_ids,
                           error_message)) {
      return false;
    }
    items->push_back(std::move(item));
  }
  return true;
}

bool ParseMuonBrowserTrayMenuItemsJson(
    const std::string& items_json,
    std::vector<MuonBrowserTrayMenuItem>* items,
    std::string* error_message) {
  if (items == nullptr || error_message == nullptr) {
    return false;
  }
  items->clear();
  yyjson_read_err read_error = {};
  MuonJsonDocument document(yyjson_read_opts(
      const_cast<char*>(items_json.data()), items_json.size(),
      YYJSON_READ_NOFLAG, nullptr, &read_error));
  if (document.get() == nullptr) {
    *error_message = "Tray menu items JSON is invalid";
    return false;
  }
  return ParseTrayMenuItemsArray(yyjson_doc_get_root(document.get()), items,
                                 error_message);
}

bool ParseMuonBrowserTrayOptionsJson(const std::string& options_json,
                                     MuonBrowserTrayOptions* options,
                                     std::string* error_message) {
  if (options == nullptr || error_message == nullptr) {
    return false;
  }
  *options = {};
  yyjson_read_err read_error = {};
  MuonJsonDocument document(yyjson_read_opts(
      const_cast<char*>(options_json.data()), options_json.size(),
      YYJSON_READ_NOFLAG, nullptr, &read_error));
  if (document.get() == nullptr) {
    *error_message = "Tray options JSON is invalid";
    return false;
  }
  auto* root = yyjson_doc_get_root(document.get());
  if (!yyjson_is_obj(root)) {
    *error_message = "Tray options JSON root must be an object";
    return false;
  }

  const auto id = yyjson_obj_get(root, kMuonTrayIdKey);
  if (id != nullptr) {
    if (!yyjson_is_str(id)) {
      *error_message = "Tray id must be a string";
      return false;
    }
    options->id = ReadJsonString(id);
    if (!IsValidMuonBrowserTrayId(options->id)) {
      *error_message = "Tray id is invalid: " + options->id;
      return false;
    }
  }

  const auto icon = yyjson_obj_get(root, kMuonTrayIconKey);
  if (icon == nullptr) {
    options->follow_title_bar_icon = true;
  } else {
    if (!yyjson_is_str(icon)) {
      *error_message = "Tray icon must be a string";
      return false;
    }
    options->icon_path = ReadJsonString(icon);
    if (options->icon_path.empty()) {
      *error_message = "Tray icon must not be empty";
      return false;
    }
  }

  const auto tooltip = yyjson_obj_get(root, kMuonTrayTooltipKey);
  if (tooltip != nullptr) {
    if (!yyjson_is_str(tooltip)) {
      *error_message = "Tray tooltip must be a string";
      return false;
    }
    options->tooltip = ReadJsonString(tooltip);
  }

  const auto menu = yyjson_obj_get(root, kMuonTrayMenuKey);
  if (menu != nullptr &&
      !ParseTrayMenuItemsArray(menu, &options->menu_items, error_message)) {
    return false;
  }
  return true;
}

std::string CreateMuonBrowserTrayEventDetailJson(
    const std::string& token,
    const MuonBrowserTrayEvent& event) {
  std::string json = "{";
  json += "\"token\":";
  AppendJsonString(&json, token);
  json += ",\"trayId\":";
  AppendJsonString(&json, event.tray_id);
  json += ",\"type\":";
  switch (event.type) {
    case kMuonBrowserTrayEventActivate:
      AppendJsonString(&json, "activate");
      json += ",\"x\":" + std::to_string(event.x) +
              ",\"y\":" + std::to_string(event.y);
      break;
    case kMuonBrowserTrayEventSecondaryActivate:
      AppendJsonString(&json, "secondaryActivate");
      json += ",\"x\":" + std::to_string(event.x) +
              ",\"y\":" + std::to_string(event.y);
      break;
    case kMuonBrowserTrayEventMenu:
      AppendJsonString(&json, "menu");
      json += ",\"id\":";
      AppendJsonString(&json, event.menu_id);
      json += event.checked ? ",\"checked\":true" : ",\"checked\":false";
      break;
  }
  json += "}";
  return json;
}
