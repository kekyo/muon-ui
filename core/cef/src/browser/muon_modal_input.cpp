/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */

#include "browser/muon_modal_input.h"

#include "include/views/cef_button_delegate.h"
#include "include/views/cef_label_button.h"

#include <map>
#include <utility>

struct MuonModalInput {
  CefRefPtr<CefWindow> owner;
  CefRefPtr<CefOverlayController> overlay;
  std::function<void()> present_dialog;
};

// CEF can return different wrappers for the same native window. Route by its
// handle, and unregister before that handle can be reused after destruction.
static std::map<CefWindowHandle, MuonModalInput> g_muon_modal_inputs;

class MuonModalInputDelegate final : public CefButtonDelegate {
 public:
  explicit MuonModalInputDelegate(CefWindow* window) : window_(window) {}

  void OnButtonPressed(CefRefPtr<CefButton> button) override {
    (void)button;
    PresentMuonModalDialog(window_);
  }

 private:
  // The window owns this overlay. CefView::GetWindow on overlay contents would
  // return the overlay's separate window instead of the browser owner.
  CefWindow* const window_;
  IMPLEMENT_REFCOUNTING(MuonModalInputDelegate);
};

CefRefPtr<CefOverlayController> CreateMuonModalInputOverlay(
    CefRefPtr<CefWindow> window,
    std::function<void()> present_dialog) {
  const auto blocker = CefLabelButton::CreateLabelButton(
      new MuonModalInputDelegate(window.get()), CefString());
  blocker->SetInkDropEnabled(false);
  blocker->SetFocusable(true);
  blocker->SetBackgroundColor(CefColorSetARGB(1, 0, 0, 0));
  const auto overlay = window->AddOverlayView(
      blocker, CEF_DOCKING_MODE_CUSTOM, true);
  if (overlay) {
    g_muon_modal_inputs[window->GetWindowHandle()] = {
        window, overlay, std::move(present_dialog)};
    UpdateMuonModalInputBounds(window);
    overlay->SetVisible(true);
    blocker->RequestFocus();
  }
  return overlay;
}

bool IsMuonModalInputBlocked(CefRefPtr<CefWindow> window) {
  return window && window->IsValid() &&
         g_muon_modal_inputs.contains(window->GetWindowHandle());
}

bool PresentMuonModalDialog(CefRefPtr<CefWindow> window) {
  if (!window || !window->IsValid()) {
    return false;
  }
  const auto found = g_muon_modal_inputs.find(window->GetWindowHandle());
  if (found == g_muon_modal_inputs.end()) {
    return false;
  }
  // Copy before calling out: a completion can unregister this owner reentrantly.
  const auto present = found->second.present_dialog;
  if (window->IsValid() && !window->IsClosed() && window->IsVisible()) {
    present();
  }
  return true;
}

void UpdateMuonModalInputBounds(CefRefPtr<CefWindow> window) {
  if (!window || !window->IsValid()) {
    return;
  }
  const auto found = g_muon_modal_inputs.find(window->GetWindowHandle());
  if (found == g_muon_modal_inputs.end() ||
      !found->second.overlay->IsValid()) {
    return;
  }
  const auto size = window->GetSize();
  const auto bounds = CefRect(0, 0, size.width, size.height);
  if (found->second.overlay->GetBounds() != bounds) {
    found->second.overlay->SetBounds(bounds);
  }
}

void UnregisterMuonModalInput(CefRefPtr<CefWindow> window) {
  if (window && window->IsValid()) {
    g_muon_modal_inputs.erase(window->GetWindowHandle());
  }
  // OnWindowDestroyed can arrive after the native handle has been cleared.
  std::erase_if(g_muon_modal_inputs, [](const auto& entry) {
    return !entry.second.owner->IsValid() || entry.second.owner->IsClosed();
  });
}
