/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon
 */

#pragma once

#include <cstddef>
#include <string>
#include <vector>

/**
 * muon browser context menu item kind.
 */
enum MuonBrowserContextMenuItemType {
  /** Command item that notifies JavaScript when selected. */
  kMuonBrowserContextMenuItemCommand = 0,
  /** Separator item. */
  kMuonBrowserContextMenuItemSeparator = 1,
};

/**
 * Placement slot for muon browser context menu items.
 */
enum MuonBrowserContextMenuPlacement {
  /** Insert before the existing menu content. */
  kMuonBrowserContextMenuPlacementStart = 0,
  /** Insert after the CEF edit command group, or at the end if absent. */
  kMuonBrowserContextMenuPlacementAfterEdit = 1,
  /** Insert after the existing menu content. */
  kMuonBrowserContextMenuPlacementEnd = 2,
};

/**
 * Optional boolean condition for context menu item visibility.
 */
enum MuonBrowserContextMenuCondition {
  /** Do not check the condition. */
  kMuonBrowserContextMenuConditionAny = 0,
  /** Require the state to be false. */
  kMuonBrowserContextMenuConditionFalse = 1,
  /** Require the state to be true. */
  kMuonBrowserContextMenuConditionTrue = 2,
};

/**
 * Visibility conditions attached to one context menu item.
 */
struct MuonBrowserContextMenuConditions {
  /** Whether the target is editable. */
  MuonBrowserContextMenuCondition editable =
      kMuonBrowserContextMenuConditionAny;
  /** Whether text is selected. */
  MuonBrowserContextMenuCondition selection =
      kMuonBrowserContextMenuConditionAny;
  /** Whether the target is inside a link. */
  MuonBrowserContextMenuCondition link = kMuonBrowserContextMenuConditionAny;
  /** Whether the target is an image. */
  MuonBrowserContextMenuCondition image = kMuonBrowserContextMenuConditionAny;
  /** Whether copy is available in the current context. */
  MuonBrowserContextMenuCondition can_copy =
      kMuonBrowserContextMenuConditionAny;
  /** Whether paste is available in the current context. */
  MuonBrowserContextMenuCondition can_paste =
      kMuonBrowserContextMenuConditionAny;
};

/**
 * Context menu state used to evaluate muon item visibility.
 */
struct MuonBrowserContextMenuState {
  /** Whether the target is editable. */
  bool editable = false;
  /** Whether text is selected. */
  bool selection = false;
  /** Whether the target is inside a link. */
  bool link = false;
  /** Whether the target is an image. */
  bool image = false;
  /** Whether copy is available in the current context. */
  bool can_copy = false;
  /** Whether paste is available in the current context. */
  bool can_paste = false;
};

/**
 * Parsed muon browser context menu item.
 */
struct MuonBrowserContextMenuItem {
  /** Item kind. */
  MuonBrowserContextMenuItemType type =
      kMuonBrowserContextMenuItemCommand;
  /** Application command id for command items. */
  std::string id;
  /** User-visible label for command items. */
  std::string label;
  /** Whether the command item is enabled. */
  bool enabled = true;
  /** Placement slot for this item. */
  MuonBrowserContextMenuPlacement placement =
      kMuonBrowserContextMenuPlacementEnd;
  /** Visibility conditions for this item. */
  MuonBrowserContextMenuConditions when;
};

/**
 * Parses and validates a JSON array of muon browser context menu items.
 *
 * @param items_json JSON array produced by the JavaScript wrapper.
 * @param items Receives normalized items on success.
 * @param error_message Receives a validation error on failure.
 * @return true when parsing succeeds.
 */
bool ParseMuonBrowserContextMenuItemsJson(
    const std::string& items_json,
    std::vector<MuonBrowserContextMenuItem>* items,
    std::string* error_message);

/**
 * Returns whether an item is visible for the given context menu state.
 *
 * @param item Parsed menu item.
 * @param state Context menu invocation state.
 * @return true when all configured conditions match.
 */
bool IsMuonBrowserContextMenuItemVisible(
    const MuonBrowserContextMenuItem& item,
    const MuonBrowserContextMenuState& state);

/**
 * Computes the insertion index for a placement against an existing menu model.
 *
 * @param placement Desired placement.
 * @param command_ids Existing command ids in menu order. Separators should be
 * represented as -1.
 * @return Menu insertion index.
 */
size_t GetMuonBrowserContextMenuInsertionIndex(
    MuonBrowserContextMenuPlacement placement,
    const std::vector<int>& command_ids);
