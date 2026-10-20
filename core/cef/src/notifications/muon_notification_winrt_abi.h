/* muon - Multi-platform GUI application framework that uses CEF as its backend
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/muon-ui
 */
#pragma once

#include <inspectable.h>
#include <winstring.h>

// The MinGW SDK lacks these public WinRT declarations. These local declarations
// describe only the vtable prefixes used here; opaque WinRT objects are passed
// through IInspectable without accessing undeclared members. The method order,
// IIDs and UniversalApiContract v1 availability were checked against Microsoft's
// SDK declarations and their interface comments:
// https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/winrt/windows.ui.notifications.h
// https://github.com/microsoft/win32metadata/blob/main/generation/WinSDK/RecompiledIdlHeaders/winrt/windows.data.xml.dom.h
namespace muon_internal::notification_abi {

/** Public IXmlDocumentIO prefix; the document owns the parsed XML. */
struct XmlDocumentIo : IInspectable {
  /** Parses a complete XML document. */
  virtual HRESULT STDMETHODCALLTYPE LoadXml(HSTRING xml) = 0;
};

/** Public IToastNotificationFactory interface. */
struct ToastFactory : IInspectable {
  /** Creates a toast from an IXmlDocument interface. */
  virtual HRESULT STDMETHODCALLTYPE CreateToastNotification(
      IInspectable* content, IInspectable** value) = 0;
};

/** Public IToastNotification2 prefix for stable history addressing. */
struct ToastIdentity : IInspectable {
  /** Sets the nonempty tag, limited here to 16 characters. */
  virtual HRESULT STDMETHODCALLTYPE put_Tag(HSTRING value) = 0;
  /** Gets the tag. */
  virtual HRESULT STDMETHODCALLTYPE get_Tag(HSTRING* value) = 0;
  /** Sets the group, limited here to 16 characters. */
  virtual HRESULT STDMETHODCALLTYPE put_Group(HSTRING value) = 0;
  /** Gets the group. */
  virtual HRESULT STDMETHODCALLTYPE get_Group(HSTRING* value) = 0;
};

/** Public IToastNotifier prefix. */
struct ToastNotifier : IInspectable {
  /** Requests presentation; success does not guarantee visible delivery. */
  virtual HRESULT STDMETHODCALLTYPE Show(IInspectable* notification) = 0;
  /** Hides a retained notification object. */
  virtual HRESULT STDMETHODCALLTYPE Hide(IInspectable* notification) = 0;
  /** Reads the OS NotificationSetting enumeration. */
  virtual HRESULT STDMETHODCALLTYPE get_Setting(INT32* value) = 0;
};

/** Public IToastNotificationManagerStatics prefix. */
struct ToastManager : IInspectable {
  /** Creates a notifier for the packaged caller. */
  virtual HRESULT STDMETHODCALLTYPE CreateToastNotifier(ToastNotifier** value) = 0;
  /** Creates a notifier for an explicitly registered desktop AUMID. */
  virtual HRESULT STDMETHODCALLTYPE CreateToastNotifierWithId(
      HSTRING app_id, ToastNotifier** value) = 0;
};

/** Public IToastNotificationHistory prefix. */
struct ToastHistory : IInspectable {
  /** Removes a group owned by the packaged caller. */
  virtual HRESULT STDMETHODCALLTYPE RemoveGroup(HSTRING group) = 0;
  /** Removes a group for an explicit application. */
  virtual HRESULT STDMETHODCALLTYPE RemoveGroupWithId(HSTRING group, HSTRING app_id) = 0;
  /** Removes one exact tag/group belonging to an explicit application. */
  virtual HRESULT STDMETHODCALLTYPE RemoveGroupedTagWithId(
      HSTRING tag, HSTRING group, HSTRING app_id) = 0;
};

/** Public IToastNotificationManagerStatics2 interface. */
struct ToastHistoryFactory : IInspectable {
  /** Obtains the user's history manager. */
  virtual HRESULT STDMETHODCALLTYPE get_History(ToastHistory** value) = 0;
};

/** Public NOTIFICATION_USER_INPUT_DATA ABI; this API does not expose text input. */
struct NotificationUserInput {
  /** Input identifier supplied by Windows. */
  LPCWSTR key;
  /** User-provided value supplied by Windows. */
  LPCWSTR value;
};

/**
 * Public INotificationActivationCallback ABI for desktop ToastGeneric actions.
 * @see https://learn.microsoft.com/windows/win32/api/notificationactivationcallback/nn-notificationactivationcallback-inotificationactivationcallback
 */
struct NotificationActivation : IUnknown {
  /** Receives only an app identity and arguments; app data remains in the store. */
  virtual HRESULT STDMETHODCALLTYPE Activate(
      LPCWSTR app_id, LPCWSTR arguments,
      const NotificationUserInput* inputs, ULONG input_count) = 0;
};

}  // namespace muon_internal::notification_abi
