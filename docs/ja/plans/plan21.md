# muonアプリの配布物に含めるランタイムファイルの選択

## 目的と対象

アプリが許可した機能と宣言したサービスから、muonが配布物に入れるファイルを決める。アプリ開発者は既存の設定だけで必要な機能を配布できる。Webアセット、追加の配布ファイル、Node.jsサービスのソースやデータの選択はアプリ開発者が担当する。

対象はデスクトップの `dist-muon/<target>/` とそこから作るパッケージ、およびAndroidのAPKである。開発環境にインストールするnpmパッケージには、各構成のアプリを作るための部品とビルド時の検査データを置く。

## 現在の構成と実測値

調査時点は `595c6828`。デスクトップのビルダーはターゲット別の固定リストに挙げたファイルを検査してコピーし、別の一覧に従って実行権限を設定する。Linuxでは `muon-core` が直接リンクする `libmuon-ui.so` と `libcardio.so` に加え、サービスホストと外部プロセス起動用の補助プログラムも全アプリに入る。Node.jsサービス用ブリッジ、Windowsの通知アイコン、宣言されたネイティブプラグインは、すでに設定に応じてコピーしている。CEF本体と、Node.jsサービスが使うNode.js本体はランチャーが起動時に準備する。[ターゲット別のファイル一覧](../../../ui/cef/targets.ts)・[配布ディレクトリの生成](../../../ui/cef/build.ts)・[ランタイムの配置](../getting-started.md#パッケージ生成)

### ファイル単位で省ける容量

HTMLが1枚でサービスもプラグインも宣言しないアプリを使った。デスクトップはLinux・Windowsともにamd64、Androidはarm64-v8aとx86_64を含むdebug APKで測った。

| 対象 | 展開時またはAPKの容量 | 圧縮した配布物での削減量 |
| --- | --- | --- |
| Linuxの依存検査用 `linux-dependencies.json` | 13.83 MiB | tar.gzで約0.83 MiB |
| Linuxの `muon-executor-supervisor` | 80.4 KiB | tar.gzで約33 KiB |
| デスクトップの `muon-service-host` | Linuxで3.85 MiB、Windowsで20.39 MiB | Linuxのtar.gzで1.59 MiB、WindowsのZIPで3.27 MiB |
| Androidの `libmuon_android_services.so` | 両ABIから除くとAPK全体が14.00 MiBから7.55 MiBに縮小 | APKで6.45 MiB |

デスクトップで全機能をリンクしたまま、配布用の `muon-core` と `muon-service-host` の両方から `strip --strip-unneeded` で不要なシンボルを除くと、tar.gzは0.37 MiB、ZIPは2.95 MiB小さくなった。ホストを丸ごと省く場合には、そのホストのシンボルを除いた分も削減量に含まれる。各変更を組み合わせた効果は最終パッケージで測る。

### 組み込み機能を一つずつ外した場合

Releaseビルドの登録箇所と専用の実装を一つずつ外して再リンクし、同じ最小アプリを `muon pack` でパッケージ化した。表の値は変更前からの削減量（KiB）。「展開時」は対象の実行ファイル、「tar.gz」と「ZIP」はパッケージ全体の差を示す。[デスクトップのリンク対象](../../../core/cef/CMakeLists.txt)

| 外した機能 | Linux 展開時 | Linux tar.gz | Windows 展開時 | Windows ZIP |
| --- | ---: | ---: | ---: | ---: |
| ページの `muon.server` | 423 | 170 | 594 | 190 |
| 常駐サービスの `io.server.listen` | 377 | 152 | 2,071 | 336 |
| `muon.fs` のファイル操作・監視（ダイアログは維持） | 1,221 | 251 | 2,313 | 301 |
| `muon.notifications` | 534 | 199 | 505 | 126 |
| `muon.executor` の `spawn` と `loadLibrary` | 248 | 102 | 412 | 118 |
| `muon.launcher` のAPI | 31 | 14 | 42 | 16 |
| `muon.environments` のAPI | 31 | 12 | 42 | 14 |

ページと常駐サービスは同じHTTP・WebSocket実装を別々の実行ファイルにリンクしている。両方から外すとLinuxのtar.gzで322 KiB、WindowsのZIPで526 KiB減った。先にシンボルを除いて比較した場合は、ページ側だけで157 KiBと159 KiB、両方で295 KiBと324 KiBだった。Androidでは常駐サービスだけがこの実装を使う。待受を外して他のサービス機能を残すと、配布用の `libmuon_android_services.so` はarm64-v8aで476 KiB、x86_64で460 KiB小さくなった。[共通のサーバー実装](../../../core/common/cmake/MuonServer.cmake)・[ページ側の登録](../../../core/cef/src/plugins/muon_plugin_runtime_desktop.cpp)・[サービス側の登録](../../../core/service-host/muon_service_io_desktop.cpp)・[Androidのリンク対象](../../../core/android/runtime/src/main/cpp/CMakeLists.txt)

調査では一時ビルドのリンクとパッケージ生成を確認した。サービス用ライブラリを除いたAndroidの試作APKは署名検証と16 KiB境界の整列検査に通った。実際にアプリが起動すること、更新後も設定が働くことは以下の各段階で検証する。

## 実装段階と完了条件

各段階で対象の配布物を生成できる状態を保つ。変更前に利用アプリで差を確認できるテストを追加し、変更後に同じテストが通ったところで段階ごとにコミットする。

### 段階1　Linuxの依存検査データを開発環境から読む

debとAppImageの作成時は、有効な `packageDirectory` の `runtime/<target>/linux-dependencies.json` を読み、アプリの配布ディレクトリにある実際のELFファイルと照合する。これによりdebの `Depends` 算出とAppImageのABI検査を続けながら、ビルド時のJSONを配布物から省ける。[依存検査とパッケージ生成](../../../ui/cef/pack.ts)・[既存のパッケージ試験](../../../ui/cef-test/pack.test.ts)

- [ ] 配布ディレクトリ、tar.gz、deb、AppImageに検査用JSONが入らず、最小アプリが起動する。
- [ ] `packageDirectory` を切り替えても正しいターゲットのデータを使う。追加の共有ライブラリが必要とする依存はdebに反映され、ABIが合わなければパッケージ作成時に検出できる。

### 段階2　サービスを宣言しないAndroidアプリのAPKを小さくする

ターゲット向けに解決したサービス件数から、現在マニフェストの生成に使っている `servicesEnabled` をGradleにも渡す。0件のときは `packaging.jniLibs.excludes` で `libmuon_android_services.so` を除く。画面の通信に使う `libmuon_android_rpc.so` と、それが必要とする `libcardio.so`、`libc++_shared.so` は選択した各ABIに含める。[Androidプロジェクトの生成](../../../ui/android/build.ts)・[アプリのGradleテンプレート](../../../ui/android/templates/app/build.gradle)・[Android Gradle PluginのJNIライブラリ除外設定](https://developer.android.com/reference/tools/gradle-api/9.2/com/android/build/api/dsl/JniLibsPackaging)

- [ ] サービスなしのAPKをインストールしてページを開き、画面からのRPCが動く。両ABIの収録内容、APK署名、16 KiB整列を確認する。
- [ ] nativeまたはQuickJSのサービスを宣言したAPKにはサービス用ライブラリが入り、画面からサービスに接続できる。自動起動を有効にしたサービスは画面を開く前にも起動する。

### 段階3　デスクトップの配布用バイナリから不要なシンボルを除く

muonのRelease成果物をnpmパッケージ用に用意する段階で、各ターゲット用の `strip` を `muon-core` と `muon-service-host` の配布用コピーに適用する。シンボル付きのビルド成果物はデバッグに使える形で保管する。Windowsではこの処理をアプリのリソース編集と署名の前に済ませる。アプリ開発者はこれまでどおりnpmパッケージからビルドできる。[Release成果物の配置](../../../ui/scripts/stage-muon-builder.mjs)・[GNU stripの仕様](https://sourceware.org/binutils/docs/binutils/strip.html)

- [ ] 同じアプリの展開後と圧縮後の容量を変更前後で測り、LinuxとWindowsで配布物から起動する。ネイティブプラグインを呼び出し、サービスホスト経由でも応答を得られる。

### 段階4　Linuxで外部プロセスを起動するアプリにsupervisorを同梱する

`muon.json` とViteの設定を統合した後、ターゲットの基本ファイルに、許可した機能のファイルと依存先を加える。この一覧を入力ファイルの検査、コピー、実行権限の設定で共用する。最初の適用先を `muon-executor-supervisor` とする。`internal` プラグインで `muon.executor.spawn` に一致する許可があれば同梱する。`simple` は `allow`、`validate` は `imports[].allow` の解決結果を使い、ワイルドカードも既存の規則で照合する。[設定の解決](../../../ui/common/plugin-access.ts)・[許可の照合](../../../ui/common/capability.ts)・[supervisorを使う処理](../../../core/cef/src/plugins/builtin/muon_builtin_executor.cpp)

- [ ] 両モードで、明示またはワイルドカードで許可した `spawn` が配布アプリからプロセスを起動し、結果を返す。`loadLibrary` だけを許可した構成ではsupervisorを省いてもライブラリを呼び出せる。
- [ ] `vite build`、`muon build`、`muon pack` で同じ選択が働く。許可を外して再ビルドすると出力からsupervisorが消え、最小アプリが起動する。

### 段階5　デスクトップのサービスホストをサービスの有無で選ぶ

現在は通常起動でもホストに接続する。ホストはサービスの実行に加え、UIと各サービスの自動起動設定をまとめ、いずれかがONならOSのログイン時起動を登録する。サービスが0件のときは、ランタイムが既存のUI設定（`services/.ui-autostart`）を読み書きし、ログイン時起動の登録とUI表示をホストなしで処理できるようにする。通常起動、登録用の `--muon-register-autostart`、ログイン時の `--muon-autostart` に同じ判断を適用する。サービスを一つでも宣言したアプリには、画面の許可や自動起動の初期値にかかわらずホストを同梱する。[通常起動とUI設定](../../../core/cef/src/plugins/builtin/muon_builtin_services.cpp)・[ログイン時の起動](../../../core/cef/src/main.cpp)・[設定の保存先](../../../core/service-host/muon_service_startup.cpp)・[OSへの登録](../../../core/service-host/muon_service_login.cpp)

- [ ] LinuxとWindowsで、サービスなしのアプリにホストを同梱せず、通常起動で画面が開く。`muon.environments.setAutostart` で保存したON/OFFを再起動後に `getAutostart` で取得でき、登録を更新した後もログイン時のUI表示が設定に従う。
- [ ] UIの自動起動をOFF、サービスの自動起動をONにしてログインすると、サービスだけが起動する。後から画面を開いて同じサービスに接続できる。サービスを追加または削除するアプリ更新の後も、保存済みのUI設定が働く。

### 段階6　配布サイズを再測定し、サーバー分離の採否を決める

シンボルと不要なホストを除いた段階5の配布物を比較の基準にする。ページ側は `internal` プラグインが `muon.server.createServer` を許可するとき、サービス側はいずれかのサービスが `io.server.listen: true` を宣言するときに待受を使える。分離する場合は、共通のHTTP・WebSocket実装を一つの共有ライブラリにして、どちらかが待受を使えるアプリに同梱する案を検討する。[ページのローカルサーバー](../local-server.md)・[サービスのI/O設定](../../../ui/common/service-io.ts)

- [ ] ページ側だけを外した場合の削減量は圧縮後で約160 KiBだった。段階5の配布物で差を再測定し、分離の採否と理由を記録する。共有ライブラリを採用する場合は、試作で「両方なし」「ページのみ」「サービスのみ」「両方あり」の4構成を比較し、ライブラリ本体と読み込み処理を含めた展開時・圧縮後の容量を根拠にする。

`muon.launcher` と `muon.environments` の個別分離は、圧縮後の削減量が各12〜16 KiBのため優先度を下げる。他の候補の判断には上表の値を使う。通知を今後分離する場合には、通知を無効化する更新でも以前の通知履歴を消せることを条件とする。[通知履歴の削除経路](../../../core/cef/src/main.cpp)

## 完了条件

公開用npmパッケージから最小構成と各機能を使う構成をビルドし、各段階の完了条件と照合する。リポジトリ直下で `npm test` を実行し、対象OSでの起動・更新の結果、最終的な配布サイズ、サーバー分離の判断をこの計画に記録する。
