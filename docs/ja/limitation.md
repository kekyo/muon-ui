# 制約

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

Linuxにおいて、muonアプリが管理者権限で起動されない場合、CEFが必要とする `cef-sandbox` が正しく起動できません。
これは、 `dist-muon/` 配下に出力されたビルド成果物ファイルを、ユーザー権限で直接起動した場合に発生します。
正しく起動するには、管理者権限で起動するか、あるいは `deb` パッケージをAPTインストールして、 `cef-sandbox` を管理者権限で起動できるようにする必要があります。
