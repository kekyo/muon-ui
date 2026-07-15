# Package installation locations (Advanced topics)

deb packages install to `/usr/lib/<packageName>/` and `/usr/bin/<packageName>`.
For launcher display, `/usr/share/applications/<desktopId>.desktop` and `/usr/share/icons/hicolor/256x256/apps/<desktopId>.png` are also installed as package-owned files.
The user's state directory is not removed on uninstall, but the system desktop entry and icon are removed by dpkg, so the launcher display disappears.

The default nsis installation location is `%LOCALAPPDATA%\Programs\<packageName>-<arch>`.
The display name, Start Menu shortcut, and uninstall entry use `<packageName> (<arch>)`.
On uninstall, runtime state under `%LOCALAPPDATA%\<appId>.<arch>` is also removed.
`<arch>` is `amd64` or `i686`.
