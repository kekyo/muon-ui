// muon - Multi-platform GUI application framework that uses CEF as its backend
// Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
// Under MIT.
// https://github.com/kekyo/muon-ui
#ifndef MUON_NOTIFICATION_WINDOWS_H
#define MUON_NOTIFICATION_WINDOWS_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Clears OS notification history owned by this installed launcher.
 * @param app_id Architecture-specific AppUserModelID, in UTF-8.
 * @param launcher_path Absolute path of this launcher, in UTF-8.
 * @param activator_clsid Stable registered COM activator identifier, in UTF-8.
 * @return Zero after the OS accepts clearing history; nonzero on missing or
 *     mismatched registration, invalid input, or an OS failure.
 * @remarks Reads the current user's registration before changing history. It
 *     does not unregister the application or delete its pending event store.
 *     The synchronous launcher maintenance entry owns all temporary resources.
 */
int muon_notification_clear_history(const char *app_id,
                                    const char *launcher_path,
                                    const char *activator_clsid);

#ifdef __cplusplus
}
#endif
#endif
