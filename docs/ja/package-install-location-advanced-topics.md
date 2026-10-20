# パッケージのインストール先について (Advanced topics)

debパッケージのインストール先は `/usr/lib/<packageName>/` と `/usr/bin/<packageName>` です。
ランチャー表示用に `/usr/share/applications/<desktopId>.desktop` と `/usr/share/icons/hicolor/256x256/apps/<desktopId>.png` もpackage-owned fileとして配置します。
アンインストール時にユーザーのstate directoryは削除しませんが、system desktop entryとiconはdpkgにより削除されるため、ランチャー表示は消えます。

AppImageは利用者が選んだ場所へ置きます。起動時にCEFと必要なNode.js、アイコンなどを`$XDG_STATE_HOME/<appId>/runtime/`へ準備し、未指定なら`~/.local/state/<appId>/runtime/`を使用します。メニューの登録先は`$XDG_DATA_HOME/applications/<desktopId>.desktop`、未指定なら`~/.local/share/applications/<desktopId>.desktop`です。登録からの起動先は元のAppImageです。アイコンは通常、状態ディレクトリを参照し、通知有効時はユーザーのアイコンテーマへ登録します。

APIで自動起動を有効にすると、`$XDG_CONFIG_HOME/autostart/<appId>.desktop`、未指定なら`~/.config/autostart/<appId>.desktop`を作成します。AppImageを移動・改名した後は、一度直接起動して登録先を更新してください。無効にした自動起動や他のアプリの登録は変更しません。

AppImage自体を削除しても、メニュー、自動起動、保存データは残ります。削除する場合は先にアプリで自動起動を無効にし、通知有効時は後述の登録解除を実行してから、不要になった上記のファイルと状態ディレクトリを削除してください。アプリが別途指定したプロファイルや保存先は、その設定に従います。

nsisの既定のインストール先は `%LOCALAPPDATA%\Programs\<packageName>-<arch>` です。
表示名、Start Menu shortcut、アンインストールentryには `<packageName> (<arch>)` を使用します。
アンインストール時には `%LOCALAPPDATA%\<appId>.<arch>` のruntime stateも削除します。
`<arch>` は `amd64` または `i686` です。

## 通知の配布登録

共通通知APIは`notifications.enabled: true`でビルドしたアプリで利用できます。通知の表示、実行中の操作、終了済みアプリの起動は対応条件が異なります。`getCapabilities()`で`interaction`、`launch`、`persistence`をそれぞれ確認してください。[通知API](./muon-built-in-plugin-reference.md#共通通知api)

### Windows

NSISで導入すると、現在のユーザーに通知のアプリ識別子、名前、アイコン、起動先を登録します。32bit版と64bit版は別の識別子を使います。通常の展開実行では通知表示と実行中の操作を利用できますが、通知からの新規起動に必要な登録は行いません。登録不足は`launch.state: "unsupported"`と理由で確認できます。

通知を有効にしたまま更新する場合は、同じアプリIDとアーキテクチャを使ってください。更新前の通知と保存済みの未受領イベントを引き継ぎます。NSISで導入先を変更した場合は、登録を新しい起動先へ更新します。古い配置のアンインストーラーは、新しい配置が所有する通知登録を削除しません。

通知を無効にするNSIS更新では、旧版の通知履歴と登録を解除してから置き換えます。未受領イベントとプロファイルは保持します。通常のアンインストールでは、その配置が所有する通知を削除して起動登録を解除します。履歴の削除に失敗した場合は、古いアプリを残してエラーを返します。

### Linux

終了済みアプリを通知から起動するには、ホストアプリの登録APIを持つ`xdg-desktop-portal 1.20`以降が必要です。KDEでは、加えて`xdg-desktop-portal-kde 6.2.5`以降を必要とします。旧Portalは通知表示と実行中の操作を扱います。旧KDEでは終了済みアプリの起動を保証しません。[Portalのホスト登録API](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.host.portal.Registry.html)、[KDEの修正](https://github.com/KDE/xdg-desktop-portal-kde/commit/ddb05b229991e13a8b086bf4c006a2cd2ba55ab0)

`getCapabilities()`は登録とPortal APIの利用可否を確認しますが、KDE側の修正履歴までは判定できません。KDEでは上記のバージョン条件も確認してください。OSが保持する通知とMuonに保存済みの未受領イベントは別であり、通知サービスの再起動後にOS側の通知が残ることは保証しません。[Portal Notificationの仕様](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Notification.html)

| 配布形式 | 登録のタイミングと起動先 |
| --- | --- |
| tar.gzの展開実行 | ランチャーの初回起動時にユーザー単位で登録する。起動先は展開したランチャー |
| AppImage | AppImageの初回起動時にユーザー単位で登録する。起動先は元のAppImage |
| deb | パッケージ導入時にシステムへ登録する。起動先は`/usr/lib/<packageName>/<target>/<launcher>` |

ユーザー単位の登録先は、`$XDG_DATA_HOME`以下の`applications/<desktopId>.desktop`、`dbus-1/services/<desktopId>.service`、`icons/hicolor/256x256/apps/<desktopId>.png`です。`XDG_DATA_HOME`を省略した場合は`~/.local/share`になります。debは対応するファイルを`/usr/share`以下に配置し、dpkgが管理します。AppImageを手動展開したAppDirだけでは、元のAppImageへの通知起動登録は作成しません。

同じアプリIDを維持して更新してください。展開先やAppImageを移動・改名した場合は、新しい場所から一度直接起動すると登録先を更新します。起動前は古い登録先が残るため、通知からの起動先も自動では変わりません。

### Linuxの更新と削除

アプリを終了してから、現在の配置のランチャーで次のコマンドを実行できます。`./my-app`は実際のランチャー名またはAppImageのパスに置き換えてください。

```bash
# Remove this application's OS notifications.
./my-app --muon-notification-clear-history

# Remove OS notifications and this launcher's user registration.
./my-app --muon-notification-unregister
```

どちらも保存済みの未受領イベントとプロファイルは削除しません。登録の所有者をアプリIDと起動パスで確認し、別の配置が現在所有する登録は解除しません。通知の削除に失敗した場合は、登録を残してエラーを返します。

展開配布物やAppImageを削除するときは、先に`--muon-notification-unregister`を実行してください。通知機能を無効にする版へ更新する場合も、旧版で登録を解除してから置き換えます。通知を有効にしたまま更新する場合は、既存通知から起動できるよう登録を維持します。

debのシステム登録はdpkgが削除します。アンインストールや通知無効版への更新の前に、通知を投稿したデスクトップのユーザーとして`--muon-notification-clear-history`を実行してください。パッケージ削除処理が、各ユーザーのデスクトップセッションにある通知をまとめて消すことはありません。


## Windowsで長いパスを使う場合

Windowsの`user-state`方式では、配布元と実行場所を分けます。launcherは配布物を読み取り、CEF等の実行に必要なファイルを`%LOCALAPPDATA%\<appId>.<arch>\runtime\`へ準備します。配布元に長いパスを使用する場合も、runtime、プロファイル、一時ディレクトリには短い通常表記のパスを使用してください。`LOCALAPPDATA`自体が長ければ、この方式だけでは解決できません。`in-place`方式は配布元から実行するため、その場所に実行時の制約が適用されます。[CEF等の配置制約](./limitation.md#windowsの長いパス)

`browser.profilePath`で短いプロファイルの保存先を明示できます。この設定でruntimeの場所が変わることはありません。長さはWindowsの文字列表現であるUTF-16のコード単位で数え、終端文字を含めません。runtimeの作業ディレクトリが260以上になる場合、launcherは準備を開始する前にエラーを返します。muonが扱うデータ、設定ファイル、アセット、プラグインの長いパスに、OSのポリシー変更は不要です。

NSISのインストーラーは、OSの長いパスポリシーに依存せず扱える配置に制限しています。インストール先の上限は同梱ファイルの名前と階層によって変わります。ファイルの絶対パスは260未満、作成するディレクトリは248未満となるよう検査します。ショートカットの保存先と、ユーザーの状態ディレクトリ内に準備するruntimeの場所も検査します。いずれかが上限を超える場合は展開前に中止します。通常起動ではエラーを表示し、`/S`によるサイレント実行では終了コード`206`を返します。短いインストール先やアプリケーション名を選んでください。

NSISには`longPathAware`（長いパスを扱うことをWindowsへ通知する宣言）を追加していますが、ショートカットや削除処理の制約は残ります。アプリが後から状態ディレクトリに作成した任意の長さのファイルまで、NSISで削除できるとは保証しません。長いデータを保存するアプリは、そのデータを`muon.fs`で削除してからアンインストールしてください。[NSISの長いパスに関する注意](https://nsis.sourceforge.io/Docs/Chapter4.html#longpathaware)、[NSISの削除処理](https://github.com/kichik/nsis/blob/v309/Source/exehead/util.c#L176)
