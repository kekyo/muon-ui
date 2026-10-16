# パッケージのインストール先について (Advanced topics)

debパッケージのインストール先は `/usr/lib/<packageName>/` と `/usr/bin/<packageName>` です。
ランチャー表示用に `/usr/share/applications/<desktopId>.desktop` と `/usr/share/icons/hicolor/256x256/apps/<desktopId>.png` もpackage-owned fileとして配置します。
アンインストール時にユーザーのstate directoryは削除しませんが、system desktop entryとiconはdpkgにより削除されるため、ランチャー表示は消えます。

AppImageは利用者が選んだ場所へ置きます。起動時にCEFと必要なNode.js、アイコンなどを`$XDG_STATE_HOME/<appId>/runtime/`へ準備し、未指定なら`~/.local/state/<appId>/runtime/`を使用します。メニューの登録先は`$XDG_DATA_HOME/applications/<desktopId>.desktop`、未指定なら`~/.local/share/applications/<desktopId>.desktop`です。登録からの起動先は元のAppImageで、アイコンは状態ディレクトリを参照します。

APIで自動起動を有効にすると、`$XDG_CONFIG_HOME/autostart/<appId>.desktop`、未指定なら`~/.config/autostart/<appId>.desktop`を作成します。AppImageを移動・改名した後は、一度直接起動して登録先を更新してください。無効にした自動起動や他のアプリの登録は変更しません。

AppImage自体を削除しても、メニュー、自動起動、保存データは残ります。削除する場合は先にアプリで自動起動を無効にし、不要になった上記のファイルと状態ディレクトリを削除してください。アプリが別途指定したプロファイルや保存先は、その設定に従います。

nsisの既定のインストール先は `%LOCALAPPDATA%\Programs\<packageName>-<arch>` です。
表示名、Start Menu shortcut、アンインストールentryには `<packageName> (<arch>)` を使用します。
アンインストール時には `%LOCALAPPDATA%\<appId>.<arch>` のruntime stateも削除します。
`<arch>` は `amd64` または `i686` です。
