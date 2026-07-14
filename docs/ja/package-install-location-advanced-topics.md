# パッケージのインストール先について (Advanced topics)

debパッケージのインストール先は `/usr/lib/<packageName>/` と `/usr/bin/<packageName>` です。
ランチャー表示用に `/usr/share/applications/<desktopId>.desktop` と `/usr/share/icons/hicolor/256x256/apps/<desktopId>.png` もpackage-owned fileとして配置します。
アンインストール時にユーザーのstate directoryは削除しませんが、system desktop entryとiconはdpkgにより削除されるため、ランチャー表示は消えます。

nsisの既定のインストール先は `%LOCALAPPDATA%\Programs\<packageName>-<arch>` です。
表示名、Start Menu shortcut、アンインストールentryには `<packageName> (<arch>)` を使用します。
アンインストール時には `%LOCALAPPDATA%\<appId>.<arch>` のruntime stateも削除します。
`<arch>` は `amd64` または `i686` です。
