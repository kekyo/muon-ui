/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#include <windows.h>
#include <initguid.h>
#include <uiautomation.h>
#include <atomic>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

// Native UIA sees Windows 11's XAML notification tree. The older managed
// System.Windows.Automation client omits this tree on the test machine.
template <typename T>
struct ComPointer {
  T* value = nullptr;
  ComPointer() = default;
  ComPointer(const ComPointer&) = delete;
  ComPointer& operator=(const ComPointer&) = delete;
  ComPointer(ComPointer&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
  ComPointer& operator=(ComPointer&& other) noexcept {
    if (value) value->Release();
    value = std::exchange(other.value, nullptr);
    return *this;
  }
  ~ComPointer() { if (value) value->Release(); }
};

static void Check(HRESULT result, const char* operation) {
  if (FAILED(result)) throw std::runtime_error(std::string(operation) +
      " failed: " + std::to_string(static_cast<unsigned long>(result)));
}

static std::wstring Name(IUIAutomationElement* element) {
  BSTR value = nullptr;
  Check(element->get_CurrentName(&value), "Read accessible name");
  const auto result = value ? std::wstring(value, SysStringLen(value)) : std::wstring{};
  SysFreeString(value);
  return result;
}

static ComPointer<IUIAutomationElement> FindProperty(IUIAutomation* automation,
    IUIAutomationElement* parent, PROPERTYID property, const std::wstring& value,
    TreeScope scope) {
  VARIANT argument;
  VariantInit(&argument);
  argument.vt = VT_BSTR;
  argument.bstrVal = SysAllocString(value.c_str());
  ComPointer<IUIAutomationCondition> condition;
  const auto result = automation->CreatePropertyCondition(property, argument, &condition.value);
  VariantClear(&argument);
  Check(result, "Create UIA condition");
  ComPointer<IUIAutomationElement> element;
  Check(parent->FindFirst(scope, condition.value, &element.value), "Find UIA element");
  return element;
}

static ComPointer<IUIAutomationElement> Center(IUIAutomation* automation,
    IUIAutomationElement* root) {
  return FindProperty(automation, root, UIA_NamePropertyId, L"Notification Center", TreeScope_Children);
}

static ComPointer<IUIAutomationElement> Notification(IUIAutomation* automation,
    IUIAutomationElement* center, const std::wstring& title, const std::wstring& app_name,
    RECT& group_bounds) {
  auto label = FindProperty(automation, center, UIA_NamePropertyId, title, TreeScope_Descendants);
  if (!label.value) return {};
  ComPointer<IUIAutomationTreeWalker> walker;
  Check(automation->get_ControlViewWalker(&walker.value), "Create UIA tree walker");
  ComPointer<IUIAutomationElement> container;
  auto current = std::move(label);
  while (current.value) {
    BOOL is_center = FALSE;
    Check(automation->CompareElements(current.value, center, &is_center), "Check notification boundary");
    if (is_center) break;
    const auto name = Name(current.value);
    if (!container.value && name.starts_with(title + L".")) {
      current.value->AddRef();
      container.value = current.value;
    }
    if (container.value && name.starts_with(L"Notifications from ")) {
      // The test machine uses the English Windows shell. Stop at the owning
      // group: a same-named header elsewhere in the center cannot authorize it.
      if (name != L"Notifications from " + app_name)
        throw std::runtime_error("Notification belongs to a different application");
      const auto app = FindProperty(automation, current.value, UIA_NamePropertyId,
          app_name, TreeScope_Descendants);
      if (app.value) {
        Check(current.value->get_CurrentBoundingRectangle(&group_bounds), "Read notification group bounds");
        return container;
      }
      break;
    }
    ComPointer<IUIAutomationElement> parent;
    Check(walker.value->GetParentElement(current.value, &parent.value), "Read UIA parent");
    current = std::move(parent);
  }
  throw std::runtime_error("Notification title did not belong to the expected application");
}

static void Invoke(IUIAutomationElement* element) {
  ComPointer<IUIAutomationInvokePattern> pattern;
  Check(element->GetCurrentPatternAs(UIA_InvokePatternId, IID_IUIAutomationInvokePattern,
      reinterpret_cast<void**>(&pattern.value)), "Get UIA invoke pattern");
  Check(pattern.value->Invoke(), "Invoke notification UI control");
}

static std::optional<ToggleState> Setting(IUIAutomation* automation,
    IUIAutomationElement* root, const std::wstring& desired, bool& toggled) {
  auto window = FindProperty(automation, root, UIA_NamePropertyId, L"Settings", TreeScope_Children);
  if (!window.value) return {};
  auto control = FindProperty(automation, window.value, UIA_AutomationIdPropertyId,
      L"SystemSettings_Notifications_ShowAppNotifications_ToggleSwitch", TreeScope_Descendants);
  if (!control.value) return {};
  ComPointer<IUIAutomationTogglePattern> pattern;
  Check(control.value->GetCurrentPatternAs(UIA_TogglePatternId, IID_IUIAutomationTogglePattern,
      reinterpret_cast<void**>(&pattern.value)), "Get notification setting pattern");
  ToggleState state;
  Check(pattern.value->get_CurrentToggleState(&state), "Read notification setting");
  if (state == ToggleState_Indeterminate)
    throw std::runtime_error("Notification setting is indeterminate");
  if (desired.empty()) return state;
  const auto requested = desired == L"1" ? ToggleState_On : ToggleState_Off;
  if (state != requested && !toggled) {
    Check(pattern.value->Toggle(), "Change notification setting");
    toggled = true;
    Check(pattern.value->get_CurrentToggleState(&state), "Read changed notification setting");
  }
  return state == requested ? std::optional{state} : std::nullopt;
}

class Changes final : public IUIAutomationStructureChangedEventHandler,
                      public IUIAutomationPropertyChangedEventHandler {
 public:
  HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** output) override {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (iid == IID_IUnknown || iid == IID_IUIAutomationStructureChangedEventHandler)
      *output = static_cast<IUIAutomationStructureChangedEventHandler*>(this);
    else if (iid == IID_IUIAutomationPropertyChangedEventHandler)
      *output = static_cast<IUIAutomationPropertyChangedEventHandler*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
  ULONG STDMETHODCALLTYPE Release() override {
    const auto remaining = --references_;
    if (!remaining) delete this;
    return remaining;
  }
  HRESULT STDMETHODCALLTYPE HandleStructureChangedEvent(IUIAutomationElement*,
      StructureChangeType, SAFEARRAY*) override {
    SetEvent(changed);
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE HandlePropertyChangedEvent(IUIAutomationElement*,
      PROPERTYID, VARIANT) override {
    SetEvent(changed);
    return S_OK;
  }
 private:
  ~Changes() { if (changed) CloseHandle(changed); }
  std::atomic<ULONG> references_{1};
};

static int Observe(const std::wstring& operation, const std::wstring& title,
    const std::wstring& app_name, const std::wstring& action) {
  ComPointer<IUIAutomation> automation;
  Check(CoCreateInstance(CLSID_CUIAutomation8, nullptr, CLSCTX_INPROC_SERVER,
      IID_IUIAutomation, reinterpret_cast<void**>(&automation.value)), "Create native UIA");
  ComPointer<IUIAutomationElement> root;
  Check(automation.value->GetRootElement(&root.value), "Get UIA root");
  if (operation == L"center") {
    std::cout << (Center(automation.value, root.value).value ? "true\n" : "false\n");
    return 0;
  }
  ComPointer<Changes> changes;
  changes.value = new Changes();
  if (!changes.value->changed) throw std::runtime_error("Create UIA change event");
  Check(automation.value->AddStructureChangedEventHandler(root.value, TreeScope_SubTree,
      nullptr, changes.value), "Subscribe to UIA structure changes");
  PROPERTYID properties[] = {UIA_IsOffscreenPropertyId, UIA_BoundingRectanglePropertyId,
      UIA_NamePropertyId, UIA_ToggleToggleStatePropertyId};
  Check(automation.value->AddPropertyChangedEventHandlerNativeArray(root.value,
      TreeScope_SubTree, nullptr, changes.value, properties, 4), "Subscribe to UIA properties");
  try {
    const auto deadline = GetTickCount64() + 30000;
    bool expanded = false;
    bool toggled = false;
    while (true) {
      // A Settings window can exist before its requested page and controls.
      // Observe structure/property events until the actual switch is ready.
      if (operation == L"setting") {
        const auto state = Setting(automation.value, root.value, action, toggled);
        if (state) {
          std::cout << (*state == ToggleState_On ? "1\n" : "0\n");
          break;
        }
      }
      auto center = operation == L"setting" ? ComPointer<IUIAutomationElement>{}
          : Center(automation.value, root.value);
      if (center.value) {
        RECT bounds{};
        auto notification = Notification(automation.value, center.value, title, app_name, bounds);
        if (operation == L"removed" && !notification.value) {
          std::cout << "null\n";
          break;
        }
        if (notification.value && operation != L"removed") {
          auto target = ComPointer<IUIAutomationElement>{};
          if (action.empty()) {
            notification.value->AddRef();
            target.value = notification.value;
          } else {
            target = FindProperty(automation.value, notification.value,
                UIA_NamePropertyId, action, TreeScope_Descendants);
            if (!target.value && !expanded) {
              const auto expand = FindProperty(automation.value, notification.value,
                  UIA_AutomationIdPropertyId, L"ExpandButton", TreeScope_Descendants);
              if (!expand.value) throw std::runtime_error("Dedicated notification expand control is absent");
              Invoke(expand.value);
              expanded = true;
              continue;
            }
          }
          if (target.value) {
            BOOL hidden = TRUE;
            Check(target.value->get_CurrentIsOffscreen(&hidden), "Read notification visibility");
            if (!hidden) {
              if (operation == L"activate") Invoke(target.value);
              std::cout << "{\"x\":" << bounds.left << ",\"y\":" << bounds.top
                  << ",\"width\":" << bounds.right - bounds.left << ",\"height\":"
                  << bounds.bottom - bounds.top << "}\n";
              break;
            }
          }
        }
      }
      const auto now = GetTickCount64();
      if (now >= deadline || WaitForSingleObject(changes.value->changed,
          static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0)
        throw std::runtime_error("Notification UI did not reach the requested state");
    }
    Check(automation.value->RemoveAllEventHandlers(), "Remove UIA events");
  } catch (...) {
    automation.value->RemoveAllEventHandlers();
    throw;
  }
  return 0;
}

int wmain(int argc, wchar_t** argv) {
  if (argc < 2) return 2;
  const auto initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  if (FAILED(initialized)) return 3;
  int result = 1;
  try {
    result = Observe(argv[1], argc > 2 ? argv[2] : L"",
        argc > 3 ? argv[3] : L"", argc > 4 ? argv[4] : L"");
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
  }
  CoUninitialize();
  return result;
}
