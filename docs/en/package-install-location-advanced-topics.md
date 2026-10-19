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

## Using long paths on Windows

The Windows `user-state` layout separates the distribution source from the execution location. The launcher reads the distribution and prepares CEF and other required files in `%LOCALAPPDATA%\<appId>.<arch>\runtime\`. Even when the distribution source has a long path, use short ordinary paths for the runtime, profile, and temporary directory. This layout alone does not solve a long `LOCALAPPDATA` path. The `in-place` layout runs from the distribution directory, which must satisfy execution restrictions. See [CEF placement restrictions](./limitation.md#long-paths-on-windows).

Use `browser.profilePath` to select a short profile location. This setting does not change the runtime location. Lengths are counted in UTF-16 code units, the Windows string representation, excluding the terminator. The launcher rejects a runtime working directory of 260 units or more before preparing files. Long paths for Muon data, configuration files, assets, and plugins do not require OS policy changes.

NSIS installers restrict placement to paths usable without the Windows long-path policy. The installation directory limit depends on packaged filenames and directory depth. Absolute file paths must stay below 260 units, and directories to create below 248. The installer also checks the shortcut location and the runtime location under the user's state directory. Unsupported paths are rejected before extraction. Interactive execution shows an error; silent execution with `/S` returns exit code `206`. Choose a shorter installation directory or application name.

The installer includes `longPathAware`, a declaration telling Windows it supports long paths, but shell and removal restrictions remain. NSIS is not guaranteed to remove arbitrarily long files that the application later creates in its state directory. Applications storing such data should remove it through `muon.fs` before uninstalling. See the [NSIS long-path notes](https://nsis.sourceforge.io/Docs/Chapter4.html#longpathaware) and [NSIS removal implementation](https://github.com/kichik/nsis/blob/v309/Source/exehead/util.c#L176).
