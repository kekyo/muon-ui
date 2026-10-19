# 制約

## Windowsの長いパス

Windows版の`muon.fs`は、従来の`MAX_PATH`（終端文字を含め260文字）を超えるローカルパスを通常の表記で扱えます。長いパスのために管理者権限、`LongPathsEnabled`の変更、再起動を求めることはありません。builderによる取得・展開・コピーにも同じ方針を適用しています。長さはUTF-16のコード単位で数えます。例えば、日本語の多くは1、絵文字の多くは2となります。

この対応には、次の配置上の制約が残ります。

| 対象 | 利用条件 |
| --- | --- |
| CEFを実行するruntime、プロファイル、一時ディレクトリ | 短い通常表記のパスを使用してください。内部ファイル名を追加する余裕も必要です。長いデータパスへアクセスできることは、CEF自体を同じ場所から起動できる保証にはなりません。 |
| `browser.profilePath` | 解決後のパスが260以上、または`\\?\`で始まる場合は起動を拒否します。保存先を別の場所へ自動変更することはありません。 |
| launcherのruntime、`muon.executor`の`cwd`、`node.project` | Windowsの作業ディレクトリとして使うパスは260未満が必要です。Node.jsで深い階層のモジュールを使う場合は、短いプロジェクトルートから相対パスで読み込んでください。 |
| NSISとショートカット | インストール先は同梱ファイル名を含めた長さで制限されます。[インストール先の説明](./package-install-location-advanced-topics.md)を参照してください。 |
| 自動起動 | 引用符を含むRunキーの登録コマンド全体が260以内である必要があります。 |

CEFの配置条件は、参照Chromiumに残る[実行場所の固定長処理](https://github.com/chromium/chromium/blob/147.0.7727.138/base/base_paths_win.cc)と[一時ディレクトリ等の処理](https://github.com/chromium/chromium/blob/147.0.7727.138/base/files/file_util_win.cc)によるものです。作業ディレクトリとプロセス起動の関係は[Windowsの仕様](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setcurrentdirectory)、自動起動の上限は[Runキーの仕様](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)を参照してください。

ファイルシステムには、パス要素ごとの長さや拡張長パス全体の上限もあります。muonは名前を切り詰めて処理を続けません。`\\?\`で始まる拡張長表記は解析規則も異なるため、ファイルI/O用の表記をCEF設定、シェル、URI、外部プロセスの引数へ一律に付けないでください。[Windowsのパス長と解析規則](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation)

## WaylandとVulkan

現在のCEF (`147.0.14+g76d2442`) は、Linux+Wayland環境において、Vulkanとの併用を完全にサポートしていません。多くの場合、この組み合わせは問題を引き起こすとの事なので、muonではLinux表示バックエンドがWaylandと判定された場合に、自動的にCEFのVulkan関連機能を無効化します。
例えば、 `XDG_SESSION_TYPE=wayland` や `WAYLAND_DISPLAY` 、または `--ozone-platform=wayland` が検出された場合が対象です。
一方で、 `--ozone-platform=x11` のようにX11バックエンドが明示されている場合は、この無効化を行いません。

また、GL/ANGLEバックエンドが明示指定されていないWayland環境では、Vulkan経由のANGLEを避けるため、CEFがANGLEのOpenGLバックエンドを使用するように調整します。

## virtual moduleインポートのフィルタ機能

muonプラグインのvirtual moduleインポートのフィルタ機能は、サプライチェーン攻撃を完全に排除するものではないことに注意してください。
これは、実行時にmuonコードへ要求を発生させる場合に、Viteが生成したランダムなcapability IDを使用して、関数呼び出しをフィルタします。
このIDは推測されにくい値ですが、ロードされたbundleの動的解析や、同じページ上で実行されている悪意あるコードによってIDを特定された場合は、フィルタを突破される可能性があります。

一方で、ページURLのフィルタ（`validate`モードでは自動構成・`simple`モードでは手動構成）は、muonオブジェクトそのものをJavaScriptから参照できなくするため、強固に作用します。
したがって、不要なプラグイン関数を露出させないように常に注意してください。

## Android WebViewバックエンド

公開CLIによるAPKの作成方法と検証済み環境は[Androidアプリのビルド・配布](./android.md)を参照してください。公開アプリではFCMとQuickJSを提供していません。以下で触れるQuickJSは試作host専用です。

Android WebViewバックエンドは、WebViewの通常ネットワーク通信をMuonで包括的にinterceptしません。CEF版の`network.allow`、`network.authorizedOrigin`、`network.localAccess`は共通の`muon.json`へ残せますが、AndroidではWebViewの通信を許可または拒否する条件として適用されず、明示設定時に警告されます。

Androidでは、Manifestの`INTERNET`、端末とAPI levelに応じたローカルネットワーク権限、Network Security ConfigがMuonより外側のアプリ全体の制約として作用します。CORSやCSPなどのWeb platformの制約も通常どおり適用されます。`INTERNET`が付与された環境では、Muonの宛先allowlistがなくてもWebViewから外部へ通信できることを前提にしてください。

MuonプラグインRPCは、信頼するHTTPSアセットoriginのメインフレームだけから受理します。外部originのメインフレーム、外部iframe、同一originのiframeからの直接RPCは拒否します。ただし、同一originのiframeから親ページのオブジェクトへアクセスする経路まで隔離する保証はありません。同じページ内のJavaScriptを取得元ファイルごとに認証することもできません。simpleモードでは公開API、validateモードでは取得した有効なcapabilityの許可範囲で呼出しが可能です。CSPと依存関係の管理を併用してください。[WebMessageListenerが通知する送信元情報](https://developer.android.com/reference/androidx/webkit/WebViewCompat.WebMessageListener)

validateモードの`sources`・`packages`はViteによる直接importの検査です。ネイティブ側ではcapability IDと許可関数を検査します。`plugin.pages`は省略、空配列、`asset://main/**`、`https://main.asset.muon.invalid/**`に対応し、その他の条件はビルド時に拒否します。空配列はブリッジを停止します。origin規則はパスを含む完全URLのフィルタではないため、CEFのページURL globとの完全互換は提供しません。[WebViewCompatの注入規則](https://developer.android.com/reference/androidx/webkit/WebViewCompat)

公開APKのアセットoriginは`https://main.asset.muon.invalid`に固定しています。このhostはMuonがローカルで処理し、存在しないアセットも外部ネットワークへ転送しません。複数アセットhostや独自hostへの変更は公開ビルドでは未対応です。過去の構成案と検証の経緯は[ネットワークフィルタ検証記録](../../filter-limitation.md)を参照してください。

現在のAndroid版`muon.fs`は、Android OSが許可する実際のfilesystem pathだけを扱います。`content://` URIの直接指定と`muon.fs.dialogs`は未対応です。`muon.launcher`、`muon.executor`、desktop用Node.js sidecarも公開しません。外部ネイティブプラグインはAPKへ同梱したものをsimple・validateの両モードで使えますが、インストール後にAPK外から追加する機能はありません。[外部プラグインの設定](./android.md#事前ビルド済みネイティブプラグイン)

Android試作hostの`muon.node.createNode()`は、別process Serviceの組み込みQuickJSを生成し、限定したNode.js風moduleを提供します。Node.js、npm package、CommonJS、標準library全体との互換性はありません。利用可能なplugin関数の一覧は[Android API対応方針](../../android-api-compatibility.md)、QuickJSのmodule、ネットワーク境界、資源上限は[Android試作host](../../core/android-poc/README.md)を参照してください。

## LinuxにおけるCEF sandboxの有効化制限

通常のmuon配布物ではCEF sandboxを無効にしています。setuid方式を使用する場合は、`muon pack --type deb --linux-sandbox=setuid`で生成したdebをAPTからインストールしてください。この方式ではシステム領域への配置と、root所有のsandbox補助プログラムが必要です。[CEF sandboxのLinux設定](https://chromiumembedded.github.io/cef/sandbox_setup.html#linux)

AppImageとtar.gzはsetuid方式に対応せず、上記の指定をpack時に拒否します。AppImageの起動時に管理者権限を取得したり、ホストのsandbox設定を変更したりする機能はありません。
