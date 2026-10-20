# Package installation locations (Advanced topics)

deb packages install to `/usr/lib/<packageName>/` and `/usr/bin/<packageName>`.
For launcher display, `/usr/share/applications/<desktopId>.desktop` and `/usr/share/icons/hicolor/256x256/apps/<desktopId>.png` are also installed as package-owned files.
The user's state directory is not removed on uninstall, but the system desktop entry and icon are removed by dpkg, so the launcher display disappears.

Place an AppImage wherever the user chooses. Startup prepares CEF, any required Node.js runtime, and icons under `$XDG_STATE_HOME/<appId>/runtime/`, or `~/.local/state/<appId>/runtime/` when unset. The menu entry is `$XDG_DATA_HOME/applications/<desktopId>.desktop`, or `~/.local/share/applications/<desktopId>.desktop`. It launches the original AppImage. Its icon normally comes from the state directory; notification-enabled builds register it in the user icon theme.

Enabling autostart through the API creates `$XDG_CONFIG_HOME/autostart/<appId>.desktop`, or `~/.config/autostart/<appId>.desktop`. After moving or renaming the AppImage, launch it directly once to refresh these paths. Disabled autostart entries and entries belonging to other applications are preserved.

Deleting the AppImage leaves menu entries, autostart entries, and saved data behind. Disable autostart in the application first and, when notifications are enabled, unregister them as described below. Then remove the files and state directory above when no longer needed. Separately configured profiles and other storage follow the application's settings.

The default nsis installation location is `%LOCALAPPDATA%\Programs\<packageName>-<arch>`.
The display name, Start Menu shortcut, and uninstall entry use `<packageName> (<arch>)`.
On uninstall, runtime state under `%LOCALAPPDATA%\<appId>.<arch>` is also removed.
`<arch>` is `amd64` or `i686`.

## Notification deployment registration

Applications built with `notifications.enabled: true` can use the shared notification API. Display, interaction with a running process, and starting an exited application have different requirements. Check `interaction`, `launch`, and `persistence` separately through `getCapabilities()`. See the [notification API](./muon-built-in-plugin-reference.md#shared-notification-api).

### Windows

NSIS installs the notification application identity, name, icon, and launch destination for the current user. The 32-bit and 64-bit versions use separate identities. Ordinary unpacked execution supports notification display and running-process interaction, but does not register a destination for starting an exited application. Missing registration is reported through `launch.state: "unsupported"` and its reason.

Keep the application ID and architecture unchanged when updating with notifications enabled. Existing notifications and saved pending events are retained. Changing the installation location through NSIS updates the launch registration. The old installation's uninstaller does not delete notification registration now owned by the new location.

An NSIS update that disables notifications clears the old notification history and registration before replacement, preserving pending events and profiles. Ordinary uninstall clears notifications owned by that installation and removes its launch registration. If history cleanup fails, the old application is kept and an error is returned.

### Linux

Starting an exited application from a notification requires `xdg-desktop-portal 1.20` or later, which provides the host registration API. KDE additionally requires `xdg-desktop-portal-kde 6.2.5` or later. Older Portal versions support display and running-process interaction. Starting an exited application is not guaranteed on older KDE backends. See the [Portal host registration API](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.host.portal.Registry.html) and the [KDE fix](https://github.com/KDE/xdg-desktop-portal-kde/commit/ddb05b229991e13a8b086bf4c006a2cd2ba55ab0).

`getCapabilities()` checks registration and Portal API availability but cannot determine KDE's patch history. Check the version requirement separately on KDE. OS notifications and pending events saved by Muon are separate; OS notification retention across notification-service restarts is not guaranteed. See the [Portal Notification specification](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Notification.html).

| Distribution | Registration and launch destination |
| --- | --- |
| Unpacked tar.gz, zip, and similar distributions | User registration on first launcher execution; launches the extracted launcher |
| AppImage | User registration on first execution; launches the original AppImage |
| deb | System registration during installation; launches `/usr/lib/<packageName>/<target>/<launcher>` |

User registration uses `applications/<desktopId>.desktop`, `dbus-1/services/<desktopId>.service`, and `icons/hicolor/256x256/apps/<desktopId>.png` under `$XDG_DATA_HOME`, or `~/.local/share` when unset. deb installs corresponding files under `/usr/share`, owned by dpkg. A manually extracted AppDir alone does not create notification launch registration for the original AppImage.

Preserve the application ID across updates. After moving or renaming an extracted application or AppImage, launch it directly once from the new location to update registration. Until then, notification activation still points to the old registered path.

### Updating and removing Linux applications

After stopping the application, run these commands through its current launcher. Replace `./my-app` with the actual launcher or AppImage path.

```bash
# Remove this application's OS notifications.
./my-app --muon-notification-clear-history

# Remove OS notifications and this launcher's user registration.
./my-app --muon-notification-unregister
```

Both preserve saved pending events and profiles. Registration ownership is checked using the application ID and launch path; registration now owned by another location is not removed. Notification cleanup failure leaves registration intact and returns an error.

Before deleting an extracted distribution or AppImage, run `--muon-notification-unregister`. Also unregister through the old version before replacing it with a notification-disabled build. When updating with notifications enabled, preserve registration so existing notifications can still launch the application.

dpkg removes deb's system registration. Before uninstalling or updating to a notification-disabled version, run `--muon-notification-clear-history` as the desktop user who posted the notifications. Package removal does not clear notifications from every user's desktop session.


## Using long paths on Windows

The Windows `user-state` layout separates the distribution source from the execution location. The launcher reads the distribution and prepares CEF and other required files in `%LOCALAPPDATA%\<appId>.<arch>\runtime\`. Even when the distribution source has a long path, use short ordinary paths for the runtime, profile, and temporary directory. This layout alone does not solve a long `LOCALAPPDATA` path. The `in-place` layout runs from the distribution directory, which must satisfy execution restrictions. See [CEF placement restrictions](./limitation.md#long-paths-on-windows).

Use `browser.profilePath` to select a short profile location. This setting does not change the runtime location. Lengths are counted in UTF-16 code units, the Windows string representation, excluding the terminator. The launcher rejects a runtime working directory of 260 units or more before preparing files. Long paths for Muon data, configuration files, assets, and plugins do not require OS policy changes.

NSIS installers restrict placement to paths usable without the Windows long-path policy. The installation directory limit depends on packaged filenames and directory depth. Absolute file paths must stay below 260 units, and directories to create below 248. The installer also checks the shortcut location and the runtime location under the user's state directory. Unsupported paths are rejected before extraction. Interactive execution shows an error; silent execution with `/S` returns exit code `206`. Choose a shorter installation directory or application name.

The installer includes `longPathAware`, a declaration telling Windows it supports long paths, but shell and removal restrictions remain. NSIS is not guaranteed to remove arbitrarily long files that the application later creates in its state directory. Applications storing such data should remove it through `muon.fs` before uninstalling. See the [NSIS long-path notes](https://nsis.sourceforge.io/Docs/Chapter4.html#longpathaware) and [NSIS removal implementation](https://github.com/kichik/nsis/blob/v309/Source/exehead/util.c#L176).
