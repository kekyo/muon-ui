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

Android WebViewバックエンドは、WebViewの通常ネットワーク通信をMuonで包括的にinterceptしません。CEF版の`network.allow`、`network.authorizedOrigin`、`network.localAccess`は共通の`muon.json`へ残せますが、AndroidではWebViewの通信を許可または拒否する条件として適用されず、明示設定時に警告されます。

Androidでは、Manifestの`INTERNET`、端末とAPI levelに応じたローカルネットワーク権限、Network Security ConfigがMuonより外側のアプリ全体の制約として作用します。CORSやCSPなどのWeb platformの制約も通常どおり適用されます。`INTERNET`が付与された環境では、Muonの宛先allowlistがなくてもWebViewから外部へ通信できることを前提にしてください。

MuonプラグインRPCは、構成されたHTTPSアセットoriginのメインフレームだけから受理します。外部originのメインフレーム、iframe、同一originのサブフレームには公開しません。ただし、信頼したメインフレーム内で実行されるJavaScriptを取得元ファイルごとに区別することはできません。外部scriptを信頼したページへ読み込むと、そのscriptもページと同じ権限を持つため、CSPと依存関係の管理を併用してください。

既定のアセットURLは`https://main.asset.muon.invalid/`で、正式な構成モデルでは`https://{asset_name}.asset.muon.invalid/`を既定templateとします。構成済みのアセットhostはMuonがローカルで完結させ、存在しないアセットも外部ネットワークへfallbackさせません。ただし、誤ったhost設定、アセット専用ではない実在hostの使用、またはプラットフォーム上の想定外の処理がある場合は外部アクセスの可能性があります。templateを変更する場合は、アプリ開発者が所有する専用hostを使用し、`browser.startPage`、CEF版の`network.allow`、`plugin.pages`、originを参照するその他の設定、CSP、ソースコード、テストも整合させてください。関連する全項目は[ネットワークフィルタ検証記録](../../filter-limitation.md)に記載しています。

現在のAndroid版`muon.fs`は、Android OSが許可する実際のfilesystem pathだけを扱います。`content://` URIの直接指定と`muon.fs.dialogs`は後続作業です。`muon.launcher`、`muon.executor`、desktop用Node.js sidecar、runtime外部plugin loadも現在は公開しません。Node.js sidecarはnodejs-mobileを用いる別の実装として検討します。利用可能な関数の正確な一覧は[Android API対応方針](../../android-api-compatibility.md)を参照してください。

## LinuxにおけるCEF sandboxの有効化制限

Linuxにおいて、muonアプリが管理者権限で起動されない場合、CEFが必要とする `cef-sandbox` が正しく起動できません。
これは、 `dist-muon/` 配下に出力されたビルド成果物ファイルを、ユーザー権限で直接起動した場合に発生します。
正しく起動するには、管理者権限で起動するか、あるいは `deb` パッケージをAPTインストールして、 `cef-sandbox` を管理者権限で起動できるようにする必要があります。
