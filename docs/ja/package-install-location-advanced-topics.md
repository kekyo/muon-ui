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

## Windowsで長いパスを使う場合

Windowsの`user-state`方式では、配布元と実行場所を分けます。launcherは配布物を読み取り、CEF等の実行に必要なファイルを`%LOCALAPPDATA%\<appId>.<arch>\runtime\`へ準備します。配布元に長いパスを使用する場合も、runtime、プロファイル、一時ディレクトリには短い通常表記のパスを使用してください。`LOCALAPPDATA`自体が長ければ、この方式だけでは解決できません。`in-place`方式は配布元から実行するため、その場所に実行時の制約が適用されます。[CEF等の配置制約](./limitation.md#windowsの長いパス)

`browser.profilePath`で短いプロファイルの保存先を明示できます。この設定でruntimeの場所が変わることはありません。長さはWindowsの文字列表現であるUTF-16のコード単位で数え、終端文字を含めません。runtimeの作業ディレクトリが260以上になる場合、launcherは準備を開始する前にエラーを返します。muonが扱うデータ、設定ファイル、アセット、プラグインの長いパスに、OSのポリシー変更は不要です。

NSISのインストーラーは、OSの長いパスポリシーに依存せず扱える配置に制限しています。インストール先の上限は同梱ファイルの名前と階層によって変わります。ファイルの絶対パスは260未満、作成するディレクトリは248未満となるよう検査します。ショートカットの保存先と、ユーザーの状態ディレクトリ内に準備するruntimeの場所も検査します。いずれかが上限を超える場合は展開前に中止します。通常起動ではエラーを表示し、`/S`によるサイレント実行では終了コード`206`を返します。短いインストール先やアプリケーション名を選んでください。

NSISには`longPathAware`（長いパスを扱うことをWindowsへ通知する宣言）を追加していますが、ショートカットや削除処理の制約は残ります。アプリが後から状態ディレクトリに作成した任意の長さのファイルまで、NSISで削除できるとは保証しません。長いデータを保存するアプリは、そのデータを`muon.fs`で削除してからアンインストールしてください。[NSISの長いパスに関する注意](https://nsis.sourceforge.io/Docs/Chapter4.html#longpathaware)、[NSISの削除処理](https://github.com/kichik/nsis/blob/v309/Source/exehead/util.c#L176)
