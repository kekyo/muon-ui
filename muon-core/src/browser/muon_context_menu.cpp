/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#include "browser/muon_context_menu.h"

#include "include/internal/cef_types.h"
#include "muon_json_helpers.h"

#include "yyjson.h"

#include <set>
#include <string>
#include <utility>

using muon_internal::MuonJsonDocument;
using muon_internal::ReadJsonString;

static constexpr char kMuonContextMenuTypeKey[] = "type";
static constexpr char kMuonContextMenuTypeItem[] = "item";
static constexpr char kMuonContextMenuTypeSeparator[] = "separator";
static constexpr char kMuonContextMenuIdKey[] = "id";
static constexpr char kMuonContextMenuLabelKey[] = "label";
static constexpr char kMuonContextMenuEnabledKey[] = "enabled";
static constexpr char kMuonContextMenuPlacementKey[] = "placement";
static constexpr char kMuonContextMenuPlacementStart[] = "start";
static constexpr char kMuonContextMenuPlacementAfterEdit[] = "afterEdit";
static constexpr char kMuonContextMenuPlacementEnd[] = "end";
static constexpr char kMuonContextMenuWhenKey[] = "when";
static constexpr char kMuonContextMenuWhenEditableKey[] = "editable";
static constexpr char kMuonContextMenuWhenSelectionKey[] = "selection";
static constexpr char kMuonContextMenuWhenLinkKey[] = "link";
static constexpr char kMuonContextMenuWhenImageKey[] = "image";
static constexpr char kMuonContextMenuWhenCanCopyKey[] = "canCopy";
static constexpr char kMuonContextMenuWhenCanPasteKey[] = "canPaste";

static bool IsMuonContextMenuCommandIdReserved(const std::string& id) {
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

static bool IsValidMuonContextMenuCommandId(const std::string& id) {
  return !id.empty() && !ContainsControlCharacter(id) &&
         !IsMuonContextMenuCommandIdReserved(id);
}

static bool ParseContextMenuItemType(
    yyjson_val* value,
    MuonBrowserContextMenuItemType* type,
    std::string* error_message) {
  if (value == nullptr) {
    *type = kMuonBrowserContextMenuItemCommand;
    return true;
  }
  if (!yyjson_is_str(value)) {
    *error_message = "Context menu item type must be a string";
    return false;
  }
  const auto raw_type = ReadJsonString(value);
  if (raw_type == kMuonContextMenuTypeItem) {
    *type = kMuonBrowserContextMenuItemCommand;
    return true;
  }
  if (raw_type == kMuonContextMenuTypeSeparator) {
    *type = kMuonBrowserContextMenuItemSeparator;
    return true;
  }
  *error_message = "Context menu item type has unknown value: " + raw_type;
  return false;
}

static bool ParseContextMenuPlacement(
    yyjson_val* value,
    MuonBrowserContextMenuPlacement* placement,
    std::string* error_message) {
  if (value == nullptr) {
    *placement = kMuonBrowserContextMenuPlacementEnd;
    return true;
  }
  if (!yyjson_is_str(value)) {
    *error_message = "Context menu item placement must be a string";
    return false;
  }
  const auto raw_placement = ReadJsonString(value);
  if (raw_placement == kMuonContextMenuPlacementStart) {
    *placement = kMuonBrowserContextMenuPlacementStart;
    return true;
  }
  if (raw_placement == kMuonContextMenuPlacementAfterEdit) {
    *placement = kMuonBrowserContextMenuPlacementAfterEdit;
    return true;
  }
  if (raw_placement == kMuonContextMenuPlacementEnd) {
    *placement = kMuonBrowserContextMenuPlacementEnd;
    return true;
  }
  *error_message =
      "Context menu item placement has unknown value: " + raw_placement;
  return false;
}

static bool ReadContextMenuCondition(
    yyjson_val* when,
    const char* key,
    MuonBrowserContextMenuCondition* condition,
    std::string* error_message) {
  const auto value = yyjson_obj_get(when, key);
  if (value == nullptr) {
    *condition = kMuonBrowserContextMenuConditionAny;
    return true;
  }
  if (!yyjson_is_bool(value)) {
    *error_message =
        "Context menu item when." + std::string(key) + " must be a boolean";
    return false;
  }
  *condition = yyjson_get_bool(value) ? kMuonBrowserContextMenuConditionTrue
                                      : kMuonBrowserContextMenuConditionFalse;
  return true;
}

static bool ParseContextMenuConditions(
    yyjson_val* value,
    MuonBrowserContextMenuConditions* conditions,
    std::string* error_message) {
  *conditions = {};
  if (value == nullptr) {
    return true;
  }
  if (!yyjson_is_obj(value)) {
    *error_message = "Context menu item when must be an object";
    return false;
  }
  return ReadContextMenuCondition(value, kMuonContextMenuWhenEditableKey,
                                  &conditions->editable, error_message) &&
         ReadContextMenuCondition(value, kMuonContextMenuWhenSelectionKey,
                                  &conditions->selection, error_message) &&
         ReadContextMenuCondition(value, kMuonContextMenuWhenLinkKey,
                                  &conditions->link, error_message) &&
         ReadContextMenuCondition(value, kMuonContextMenuWhenImageKey,
                                  &conditions->image, error_message) &&
         ReadContextMenuCondition(value, kMuonContextMenuWhenCanCopyKey,
                                  &conditions->can_copy, error_message) &&
         ReadContextMenuCondition(value, kMuonContextMenuWhenCanPasteKey,
                                  &conditions->can_paste, error_message);
}

static bool ParseContextMenuCommandItem(
    yyjson_val* value,
    MuonBrowserContextMenuItem* item,
    std::set<std::string>* used_ids,
    std::string* error_message) {
  const auto id = yyjson_obj_get(value, kMuonContextMenuIdKey);
  if (!yyjson_is_str(id)) {
    *error_message = "Context menu item id must be a string";
    return false;
  }
  item->id = ReadJsonString(id);
  if (!IsValidMuonContextMenuCommandId(item->id)) {
    *error_message = "Context menu item id is invalid: " + item->id;
    return false;
  }
  if (!used_ids->insert(item->id).second) {
    *error_message = "Context menu item id is duplicated: " + item->id;
    return false;
  }

  const auto label = yyjson_obj_get(value, kMuonContextMenuLabelKey);
  if (!yyjson_is_str(label)) {
    *error_message = "Context menu item label must be a string";
    return false;
  }
  item->label = ReadJsonString(label);
  if (item->label.empty()) {
    *error_message = "Context menu item label must not be empty";
    return false;
  }

  const auto enabled = yyjson_obj_get(value, kMuonContextMenuEnabledKey);
  if (enabled != nullptr) {
    if (!yyjson_is_bool(enabled)) {
      *error_message = "Context menu item enabled must be a boolean";
      return false;
    }
    item->enabled = yyjson_get_bool(enabled);
  }
  return true;
}

static bool ParseContextMenuItem(
    yyjson_val* value,
    MuonBrowserContextMenuItem* item,
    std::set<std::string>* used_ids,
    std::string* error_message) {
  if (!yyjson_is_obj(value)) {
    *error_message = "Context menu item must be an object";
    return false;
  }
  *item = {};
  if (!ParseContextMenuItemType(
          yyjson_obj_get(value, kMuonContextMenuTypeKey), &item->type,
          error_message) ||
      !ParseContextMenuPlacement(
          yyjson_obj_get(value, kMuonContextMenuPlacementKey),
          &item->placement, error_message) ||
      !ParseContextMenuConditions(yyjson_obj_get(value, kMuonContextMenuWhenKey),
                                  &item->when, error_message)) {
    return false;
  }
  if (item->type == kMuonBrowserContextMenuItemCommand) {
    return ParseContextMenuCommandItem(value, item, used_ids, error_message);
  }
  return true;
}

static bool MatchesContextMenuCondition(
    MuonBrowserContextMenuCondition condition,
    bool value) {
  switch (condition) {
    case kMuonBrowserContextMenuConditionAny:
      return true;
    case kMuonBrowserContextMenuConditionFalse:
      return !value;
    case kMuonBrowserContextMenuConditionTrue:
      return value;
  }
  return false;
}

static bool IsEditCommandId(int command_id) {
  return command_id >= MENU_ID_UNDO && command_id <= MENU_ID_SELECT_ALL;
}

bool ParseMuonBrowserContextMenuItemsJson(
    const std::string& items_json,
    std::vector<MuonBrowserContextMenuItem>* items,
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
    *error_message = "Context menu items JSON is invalid";
    return false;
  }
  auto* root = yyjson_doc_get_root(document.get());
  if (!yyjson_is_arr(root)) {
    *error_message = "Context menu items JSON root must be an array";
    return false;
  }

  constexpr auto command_capacity = static_cast<size_t>(
      MENU_ID_USER_LAST - MENU_ID_USER_FIRST + 1);
  std::set<std::string> used_ids;
  const auto item_count = yyjson_arr_size(root);
  for (auto index = size_t{0}; index < item_count; ++index) {
    MuonBrowserContextMenuItem item;
    if (!ParseContextMenuItem(yyjson_arr_get(root, index), &item, &used_ids,
                              error_message)) {
      return false;
    }
    if (item.type == kMuonBrowserContextMenuItemCommand &&
        used_ids.size() > command_capacity) {
      *error_message = "Too many context menu command items";
      return false;
    }
    items->push_back(std::move(item));
  }
  return true;
}

bool IsMuonBrowserContextMenuItemVisible(
    const MuonBrowserContextMenuItem& item,
    const MuonBrowserContextMenuState& state) {
  return MatchesContextMenuCondition(item.when.editable, state.editable) &&
         MatchesContextMenuCondition(item.when.selection, state.selection) &&
         MatchesContextMenuCondition(item.when.link, state.link) &&
         MatchesContextMenuCondition(item.when.image, state.image) &&
         MatchesContextMenuCondition(item.when.can_copy, state.can_copy) &&
         MatchesContextMenuCondition(item.when.can_paste, state.can_paste);
}

size_t GetMuonBrowserContextMenuInsertionIndex(
    MuonBrowserContextMenuPlacement placement,
    const std::vector<int>& command_ids) {
  switch (placement) {
    case kMuonBrowserContextMenuPlacementStart:
      return 0;
    case kMuonBrowserContextMenuPlacementEnd:
      return command_ids.size();
    case kMuonBrowserContextMenuPlacementAfterEdit:
      break;
  }

  auto edit_end_index = size_t{0};
  auto found_edit_command = false;
  for (auto index = size_t{0}; index < command_ids.size(); ++index) {
    if (IsEditCommandId(command_ids[index])) {
      edit_end_index = index + 1;
      found_edit_command = true;
    }
  }
  return found_edit_command ? edit_end_index : command_ids.size();
}
