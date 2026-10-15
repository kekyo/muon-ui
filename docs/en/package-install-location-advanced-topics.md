# Package installation locations (Advanced topics)

deb packages install to `/usr/lib/<packageName>/` and `/usr/bin/<packageName>`.
For launcher display, `/usr/share/applications/<desktopId>.desktop` and `/usr/share/icons/hicolor/256x256/apps/<desktopId>.png` are also installed as package-owned files.
The user's state directory is not removed on uninstall, but the system desktop entry and icon are removed by dpkg, so the launcher display disappears.

Place an AppImage wherever the user chooses. Startup prepares CEF, any required Node.js runtime, and icons under `$XDG_STATE_HOME/<appId>/runtime/`, or `~/.local/state/<appId>/runtime/` when unset. The menu entry is `$XDG_DATA_HOME/applications/<desktopId>.desktop`, or `~/.local/share/applications/<desktopId>.desktop`. It launches the original AppImage and uses an icon in the state directory.

Enabling autostart through the API creates `$XDG_CONFIG_HOME/autostart/<appId>.desktop`, or `~/.config/autostart/<appId>.desktop`. After moving or renaming the AppImage, launch it directly once to refresh these paths. Disabled autostart entries and entries belonging to other applications are preserved.

Deleting the AppImage leaves menu entries, autostart entries, and saved data behind. Disable autostart in the application first, then remove the files and state directory above when no longer needed. Separately configured profiles and other storage follow the application's settings.

The default nsis installation location is `%LOCALAPPDATA%\Programs\<packageName>-<arch>`.
The display name, Start Menu shortcut, and uninstall entry use `<packageName> (<arch>)`.
On uninstall, runtime state under `%LOCALAPPDATA%\<appId>.<arch>` is also removed.
`<arch>` is `amd64` or `i686`.
