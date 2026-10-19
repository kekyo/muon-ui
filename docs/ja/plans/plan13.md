# Android内蔵プラグインとAndroid・CEF共通通知API

## 目的と採用方針

Androidのシステムバー制御、通知、OS設定画面との連携を、muonの内蔵プラグインとして提供する。Android専用の機能を名前空間ごとに分ける案Bと、共通操作を共通APIへ置く案Cを組み合わせる。

共通の`muon.notifications`は通知の投稿、更新、取り消し、対応機能と投稿可否の確認、通知操作の受け取りを担当する。Android専用の`muon.android.*`はシステムバーと安全領域、通知チャンネルと権限、設定画面への遷移を担当する。既存の`muon.browser`の全画面操作はAndroid専用のシステムバー制御と実装・状態を共有する。

CEF版の通知には共通APIとOS固有拡張を組み合わせる案2を採用する。Android、Windows、Linuxで基本操作をそろえ、OS固有の操作が必要になった場合は`muon.windows.notifications`などの専用名前空間へ追加する。CEFは描画基盤なので、OS固有の通知機能を`muon.cef.notifications`へまとめない。Windows・Linux専用APIの具体的な機能追加は後続対象とし、空の名前空間や未実装の関数を先に公開しない。

実装はAndroidから始め、CEFのLinux・Windowsへ進める。CEFでは実行中の通知操作を先に完成させ、次に通知からのアプリ起動を追加する。対象OSの通知実装ができるまでは関数一覧へ登録せず、実装途中の制限と実行環境による差は`getCapabilities()`で示す。

作成・更新日は2026年10月19日。Android調査の基準コミットは`7194a156ce13ebaeee1317fbe3a19b7a5afc70d0`、CEF通知の追加調査は`a23e3cc`を基準とする。本書は実装前の計画であり、以下のAPI名、設定項目、動作は追加する仕様を示す。現在の作業は計画書の更新までとし、実装と検証の結果は実施後に記録する。

## 用語と対象範囲

システムバーは上端のステータスバーと下端のナビゲーションバーを指す。ノッチなど、センサーのために表示できない部分をディスプレイカットアウトと呼ぶ。InsetsはシステムUIやカットアウトと重ならないために必要な各辺の余白である。画面端まで背景などを描く配置をedge-to-edgeと呼び、システムバーを隠す全画面表示とは区別する。

通知チャンネルは、Androidでユーザーが通知音や重要度を設定する単位である。共通通知APIでは、アプリが通知を用途別に分けるキーを`category`と呼ぶ。AndroidではこのキーをチャンネルIDに直接対応させる。CEFではアプリ内の用途分類として扱い、Androidのチャンネル登録を要求しない。

本計画に含めるのは、Androidのバー表示と配色、安全領域の取得と変更通知、AndroidとCEFの即時ローカル通知、通知操作による起動とデータの受け渡し、Androidの自アプリ設定画面への遷移である。ローカル通知は、アプリが端末内で発行する通知を意味する。CEFの対象は現在の[デスクトップ対象一覧](../../../ui/cef/targets.ts)にあるWindowsとLinuxとする。

通知の保持、実行中のクリック受信、プロセスが存在しない状態からの起動を区別する。通知がOS上に残っていても、クリックによる起動に対応しているとは限らない。対応状況にはOS、通知サービス、アプリの配布・登録状態が影響する。

次の機能は後続の検討対象として残す。

- Windows・Linux専用APIの具体的な追加、macOS向けの通知。
- Web標準の`Notification`やService Worker通知をmuonの通知へ接続する互換機能。
- 指定時刻や周期による予約通知、再起動後の予約復元、正確な時刻のアラーム。
- Firebase Cloud Messaging（FCM）など、サーバーからのプッシュ通知。
- フォアグラウンドサービス、バックグラウンドJavaScriptの継続実行。
- 通知からの直接返信、メディア通知、通話通知、バブル、全画面Intentなど、個別の用途や権限を伴う通知。
- OSの通知設定からアプリ独自の設定画面へ入る連携、アプリ別言語設定、Wi-Fiなどの端末全体の設定。

通知を表示し続けることと、アプリの処理を継続実行することは別の機能とする。OS設定への遷移はユーザーが設定を変更するための導線であり、muonからOS設定を直接書き換える機能にはしない。

## 現在のコードと拡張する箇所

| 対象 | 現在の実装 | 本計画での変更 |
| --- | --- | --- |
| [Androidの公開関数一覧](../../../core/android/function-paths.ts) | `browser`、`environments`、`fs`を登録する。 | 共通通知とAndroid専用機能を、実装できた段階ごとに追加する。 |
| [AndroidのAPI公開処理](../../../core/android/renderer/bootstrap.ts) | 内蔵関数の解決と許可フィルターが`muon.<名前空間>.<関数>`の深さを前提としている。 | `muon.android.systemBars.*`などの階層を、simple・validateの両モードで扱う。 |
| [Androidの公開API](../../../core/android/renderer/android-api.ts) | 公開引数とRPCへ渡す値を変換する。 | 通知、構造化された設定・状態、購読の型と変換を追加する。 |
| [Javaのプラットフォーム処理](../../../core/android/runtime/src/main/java/dev/muon/runtime/MuonAndroidPlatformService.java) | 全画面操作で上下のバーをまとめて表示・非表示にする。ファイル操作は別サービスへ委譲する。 | 画面、通知、設定の担当サービスへ処理を分ける。全画面操作の状態を一本化する。 |
| [Activity](../../../core/android/runtime/src/main/java/dev/muon/runtime/MuonAppActivity.java)と[WebViewホスト](../../../core/android/runtime/src/main/java/dev/muon/runtime/MuonWebViewHost.java) | Activityの生成・破棄に合わせてWebViewとブリッジを管理する。 | 権限要求の結果、設定からの復帰、初期Intentと後続Intentを各サービスへ渡す。 |
| [JNI経由のRPC](../../../core/android/runtime/src/main/cpp/muon_android_rpc_jni.cpp)と[Javaブリッジ](../../../core/android/runtime/src/main/java/dev/muon/runtime/MuonRpcBridge.java) | 許可された関数をJavaへ中継する。 | 新しい関数の登録に加え、Javaからの変更・操作イベントを購読先へ渡す。 |
| [Androidの許可設定](../../../ui/android/plugin-access.ts)と[関数カタログ](../../../ui/android/plugin-catalog.ts) | `internal`内蔵プラグインと関数ごとの許可を扱う。 | 新しい階層の関数を既存の許可設定に含める。 |
| [Android設定型](../../../ui/android.d.ts)、[ビルド設定](../../../ui/android/target.ts)、[Manifestテンプレート](../../../ui/android/templates/AndroidManifest.xml.fc) | 権限の宣言を生成できる。FCMは無効。起動時fullscreenの指定は製品ビルドで拒否する。 | 初期画面設定、通知チャンネル、通知アイコン、必要な権限宣言を生成する。起動時の全画面設定も接続する。 |
| [CEFの内蔵登録](../../../core/cef/src/plugins/builtin/muon_builtin.cpp)と[デスクトップ実行処理](../../../core/cef/src/plugins/muon_plugin_runtime_desktop.cpp) | 内蔵メタデータ、初期化・終了、ページの実行コンテキスト解放を扱う。通知サービスは未実装。 | 通知名前空間とアプリ単位のサービスを登録し、ページの購読だけをコンテキスト解放へ接続する。 |
| [CEFのAPI公開](../../../core/cef/src/app/muon_app.cpp)、[共通の関数一覧と変換](../../../ui/common/capability.ts)、[公開型](../../../ui/muon.d.ts)、[仮想モジュール型](../../../ui/muon-virtual-modules.d.ts) | メタデータからsimpleの関数を作り、validateでは一覧と型を使う。 | 両モードへ共通通知と構造化引数、イベント購読を接続する。 |
| [CEFのプラットフォーム配送](../../../core/cef/src/browser/muon_client.cpp) | Platform経路の識別子をbrowser専用の関数種別として扱う。trayもBrowser単位で所有する。 | 通知は内蔵プラグインの処理へ登録し、OS操作を専用サービスへ委譲する。browser専用の識別子へ通知操作を混在させない。 |
| [CEF設定](../../../core/cef/src/config/muon_cef_settings.cpp)と[アプリ](../../../core/cef/src/app/muon_app.h) | `root_cache_path`を設定する。既存プロセスへの再起動通知を受け取る`OnAlreadyRunningAppRelaunch`は未実装。 | 起動時と既存プロセスへの通知情報配送を接続する。 |
| [アプリ識別子](../../../ui/cef/targets.ts)、[Windowsパッケージ](../../../ui/cef/pack.ts)、[Linux desktop連携](../../../ui/cef/linux-desktop.ts)、[launcher](../../../builder/src/muon_launcher.c) | 安定したappId、NSISのショートカット、`.desktop`生成、追加引数の引き渡しがある。 | 通知用のアプリ登録、クリック起動、更新・アンインストール時の処理を追加する。 |

外部プラグインを読み込む形式へ変更する必要はない。登録名は`internal`のままとし、公開APIとOS側の処理を機能別に分ける。外部プラグイン用のABIや、サブモジュール・ベンダーコードは変更しない。

[現在のツールチェーン](../../../core/android/toolchain.json)は`minSdk: 24`、`compileSdk: 37`、`targetSdk: 37`である。本機能のためだけに最低APIレベルを引き上げず、OSに存在しない機能は利用可否として示す。未対応の指定を成功扱いで無視しない。

## 公開APIの役割分担

関数は既存APIと同様にPromiseを返す。以下の表は名前空間と責務を決めるもので、TypeScriptの型定義は各実装段階で追加する。

| 名前空間 | 主な関数案 | 担当する操作 |
| --- | --- | --- |
| `muon.browser` | `enterFullscreen()`、`exitFullscreen()`、`toggleFullscreen()` | 既存の共通全画面操作。Androidでは同じシステムバーサービスを使う。 |
| `muon.notifications` | `post(options)`、`cancel(id)`、`getStatus(options?)` | 共通形式での投稿・更新・取り消し、アプリ全体と用途分類別の投稿可否。 |
| `muon.notifications` | `getCapabilities()` | 現在の実装と実行環境で利用できる通知機能。 |
| `muon.notifications` | `getPendingEvents()`、`watchEvents(handler)`、`acknowledgeEvent(eventId)` | 起動時と実行中の通知操作を、同じイベント形式で受け取る。 |
| `muon.android.systemBars` | `configure(options)`、`getState()`、`getInsets()`、`watchInsets(handler)` | バー別の表示と配色、配置方針、実際の表示状態、安全領域。 |
| `muon.android.notifications` | `getPermissionState()`、`requestPermission()`、`ensureChannel(options)`、`getChannels()` | Androidの通知権限とチャンネルの作成・状態取得。 |
| `muon.android.settings` | `isAvailable(target)`、`open(target, options?)` | 自アプリの情報、通知全体、指定チャンネルの設定画面への遷移。 |
| `muon.windows.notifications`、`muon.linux.notifications` | 具体的な操作が必要になった時点で定義する。 | 共通形式で表現できないOS固有操作。今回の初期公開には含めない。 |

OS専用側へ`post()`、`cancel()`、通知イベントの別APIを追加しない。Androidの設定画面を開く操作は`muon.android.settings`へ集約する。将来デスクトップの設定画面連携を追加する場合も、設定操作を担当する名前空間へ分ける。

validateモードでは`muon:notifications`、`muon:android.systemBars`、`muon:android.notifications`、`muon:android.settings`からインポートする。simpleモードでは対応する`window.muon.*`の階層を公開する。関数が提供されていない対象では、validateのビルドでそのインポートを拒否し、simpleのオブジェクトにも関数を作らない。

CEFでも共通通知は`muon:notifications`からインポートし、simpleでは`window.muon.notifications`を公開する。OS固有のチャンネルや権限を共通APIの必須引数にしない。既存のデスクトップ全画面操作の意味は変えない。

```ts
import { post, cancel } from "muon:notifications";

await post({
  id: "export-completed",
  category: "jobs",
  title: "Export completed",
  body: "Your file is ready.",
  data: { screen: "exports", exportId: "123" },
});

await cancel("export-completed");
```

### CEF向けの公開案と採用理由

| 検討した案 | 公開方法 | 判断 |
| --- | --- | --- |
| 案1 共通APIのみ | OS差を内部へ閉じ込め、共通操作だけを公開する。 | APIは小さくできるが、OS固有の要求を追加しにくい。 |
| 案2 共通APIとOS固有拡張 | 基本操作を共通化し、必要になった固有操作をOS別の名前空間へ追加する。 | 採用する。Androidの案Bと案Cの役割分担を維持できる。 |
| 案3 Web標準API | `Notification`、権限要求、Service Worker通知を利用する。 | Webアプリの移植には有用だが、権限とアプリ起動の設計が異なるため後続対象とする。 |

現在のCEFは通常ウインドウで[Alloyモード](../../../core/cef/src/browser/muon_browser_view_delegate.cpp)を使用し、[権限処理](../../../core/cef/src/browser/muon_client.cpp)はローカルネットワーク向けである。Alloyでは未処理の権限要求が既定で`IGNORE`となる。Web標準API案を採用するときは、権限、通知に表示するアプリ名、クリック後の動作を別途検証する。共通通知の導入だけでWeb通知が利用可能になったとは扱わない。[CEFの権限API仕様とコメント](https://cef-builds.spotifycdn.com/docs/147.0/classCefPermissionHandler.html)

## システムバーと安全領域

### バーの表示と配置方針

`configure()`はステータスバーとナビゲーションバーについて、表示・非表示とアイコンの明暗を指定できるようにする。隠したバーはスワイプで一時表示できる。背景を画面端まで描くかどうかは、バーの表示とは独立した`contentLayout`で指定する。

| `contentLayout` | WebViewとアプリの担当 |
| --- | --- |
| `safe-area` | ネイティブ側が、バーとカットアウトを避けるようWebViewの表示範囲を調整する。アプリは同じ余白を重ねて付けない。 |
| `edge-to-edge` | WebViewを画面端まで配置する。アプリは取得したInsetsを使い、操作部品など必要な部分へ余白を付ける。 |

Android 16以降を対象とするアプリは、Android 16上でedge-to-edgeの強制を無効化できない。`safe-area`はコンテンツを安全領域へ配置するmuonの方針として実現する。ウインドウがカットアウト領域へ広がらないことを保証するAPIにはしない。[Android 16の変更](https://developer.android.com/about/versions/16/behavior-changes-16)、[カットアウトの扱い](https://developer.android.com/develop/ui/views/layout/display-cutout)

`getState()`は指定した値と実際のバーの表示状態を分けて返す。スワイプ、マルチウインドウ、OS側の制御による差を観測できるようにする。全画面操作の成功を、バーが常に非表示であり続ける保証とはしない。[immersive mode](https://developer.android.com/develop/ui/views/layout/immersive)、[WindowInsetsControllerCompatの仕様とAPIコメント](https://developer.android.com/reference/androidx/core/view/WindowInsetsControllerCompat)

既存の`enterFullscreen()`は上下のバーの非表示を要求し、`exitFullscreen()`は両方の表示を要求する。`toggleFullscreen()`は共有している要求状態を切り替える。個別APIで片方だけを隠した状態は全画面要求とは扱わず、toggleでは両方を隠す。バーの一時表示だけで要求状態を変更しない。これらの操作では`contentLayout`やアイコンの配色を変更しない。

### InsetsとCSSへの公開

`getInsets()`と`watchInsets()`ではシステムバー、カットアウト、システムジェスチャー、ソフトウェアキーボードの余白を区別する。キーボードの表示で、ノッチを避けるための余白まで変わったように見せない。安全領域の合成では各辺の必要量を求め、バーとカットアウトの値を単純加算しない。[edge-to-edgeとInsets](https://developer.android.com/develop/ui/views/layout/edge-to-edge)

Web側に渡す値はCSSピクセルを基本とし、端末の物理ピクセル、画面密度、WebViewのページズームとの換算方法を定義する。回転、ウインドウサイズの変更、キーボード表示、既存のズームAPIに追従する。換算結果は実際のDOMの位置とネイティブ領域を比較して検証する。

必要な余白は`--muon-safe-area-top`などのCSS変数としても公開する。`safe-area`ではネイティブ側ですでに処理した分を再適用させず、Web側に残る余白を示す。生のInsetsと、コンテンツが追加で避ける量を型と名前で区別する。標準の`env(safe-area-inset-*)`をmuonの値と同一と仮定しない。

`watchInsets()`は購読時点の状態と、その後の変更を届ける。解除可能なハンドルを返し、明示的な解除、ページ遷移、リロード、Activityの破棄で購読を解放する。アプリ側に定期的な取得を要求しない。

### 起動時設定

`android.systemBars`へ初期のバー表示、アイコン配色、`contentLayout`を追加する。未指定時は上下のバーを表示し、`contentLayout`を`safe-area`とする。アイコン配色の未指定値にはAndroidテーマの既定値を使う。JavaScriptが起動する前に適用し、実行時の`configure()`と同じ設定処理を使う。起動時設定を既定値とし、Activityの構成変更による再生成では実行中の要求状態を復元する。プロセスが新規起動した場合は設定ファイルの初期値から開始する。

`browser.initialWindowState`も同じ初期化へ接続し、明示的な`normal`は上下のバーの表示、`fullscreen`は両方の非表示を表す。Android専用の初期バー表示も明示して矛盾する場合は、ビルド時にエラーを返す。`initialWindowState`の省略を明示的な`normal`として扱わず、その場合はAndroid専用の指定を使う。背景色はコンテンツ側の描画として扱い、OSバージョンによって意味が変わるバー背景色の直接設定を共通仕様に含めない。

## 共通通知API

### 共通の投稿・更新・取り消し

`post()`の基本項目はアプリ内で安定した文字列の`id`、`title`、`body`、用途分類の`category`、アプリへ返す`data`とする。`data`はJSONとして保存・復元できる値に限定し、関数やネイティブオブジェクトを受け付けない。通知操作に対応する環境では、タップやクリックで自アプリを開き、このデータをアプリ内の画面選択などに使えるようにする。プロセスが存在しない状態からの起動は、別の対応機能として示す。

同じ`id`を再投稿すると、その通知を更新する。`cancel(id)`は該当通知を取り消し、存在しないIDでも完了する。標準のタップ・クリック操作を受け取ったら通知を取り消す。アプリが識別する`id`をプロセス再起動後も同じ通知へ対応させ、更新や取り消しが別の通知へ作用しないようにする。OSや通知サービスが再起動して対応を確認できない場合は、その状態を診断し、古いOS識別子を別の通知へ誤適用しない。

`post()`の成功は、利用するOS APIへ投稿を依頼する処理が完了したことを表す。OSの受領確認を返さないAPIでは、通知サービスへの到達まで保証しない。ユーザーが見たことや、必ず音・画面表示が発生することも保証しない。既知の権限拒否、通知全体の無効化、指定チャンネルの無効化は呼び出し時に診断する。事前に状態を取得した後でOS設定が変わる場合も扱う。[GApplication.send_notificationの戻り値と表示の扱い](https://docs.gtk.org/gio/method.Application.send_notification.html)

`getStatus()`は、引数を省略した場合にはアプリ全体の投稿可否を返す。`category`を指定した場合には、アプリ全体とその分類の状態を区別して返す。投稿可能、無効、判定できない状態を表し、通知の閲覧状態や一時的な表示の抑制とは分ける。Android固有の詳細は専用APIの権限状態とチャンネル情報で取得する。

### 対応機能と実行環境

`getCapabilities()`は、実装済みの機能と現在のOS・通知サービス・配布登録状態を照らして返す。返却項目は実行中の通知操作、プロセス終了後のクリック起動、通知の保持、追加アクション、アプリ全体と分類別の状態取得を区別する。各項目は対応、非対応、不明を表し、必要な登録がない場合などの理由も取得できるようにする。具体的な型と項目名は段階4で確定する。

対応機能はユーザーの現在の許可状態とは異なる。OS名だけで対応を決めたり、非対応と権限拒否を同じ値にしたりしない。投稿可否の状態取得を提供しない環境では、`getStatus()`が判定できないことを表す`unknown`を返す。Linuxの通知サーバーでは操作や保持が任意の機能なので、存在を仮定しない。[Desktop Notificationsの対応機能と操作イベント](https://specifications.freedesktop.org/notification/latest/protocol.html)

ビルドで無効な機能と、実行環境で利用できない機能も区別する。通知を無効にしたビルドでは関数を公開しない。有効なビルドでは関数を残し、通知サービスの停止など、検出できた失敗を実行時に返す。OS APIが結果を返さず検出できない配送失敗は、成功とも到達確認済みとも断定しない。未対応の追加機能を指定された場合は、成功扱いで無視しない。

### 共通設定と用途分類

通知機能の有効設定を`notifications.enabled`、既定の用途分類を`notifications.defaultCategory`へ置く。`enabled`の未指定値は`false`とする。`post()`で`category`を省略した場合に既定分類を使い、両方とも指定されていない場合は引数不足として診断する。無効なビルドでは共通通知と対象OSの通知専用関数をカタログから除外し、validateのインポートを拒否する。simpleにも公開しない。

Androidの`channels`と`smallIcon`は`android.notifications`へ残す。共通の有効設定と既定分類を同階層に重複定義しない。CEFの通知はAndroid用の設定なしで利用できる。将来Windows・Linux固有の配布設定が必要になった場合も、共通設定との役割を分ける。

Androidでは`category`とチャンネルIDを同じ文字列にする。ここでの`category`はmuonの用途分類であり、Androidのアラームなどを表すシステム分類とは別である。別の対応表や登録ハンドルは導入しない。[Androidのチャンネルとシステム分類](https://developer.android.com/develop/ui/compose/notifications/channels)

CEFでは任意の用途分類キーを受け付け、OSのチャンネル登録を要求しない。Linuxの`email.arrived`などのシステム分類へ文字列をそのまま流用しない。分類別の通知制御がないOSへ、Androidと同等のユーザー設定があるようには公開しない。[GNotificationの分類](https://docs.gtk.org/gio/method.Notification.set_category.html)

## Androidの通知拡張

### 用途分類とチャンネル

`android.notifications.channels`で静的なチャンネルを宣言し、アプリ起動時に登録する。実行時に用途を増やすアプリ向けには`ensureChannel()`を提供する。未登録の`category`への投稿は失敗させ、投稿の副作用として名前や重要度が未指定のチャンネルを作らない。チャンネルを動的に登録する場合は、`ensureChannel()`の完了後に投稿する。

Android 8以降のチャンネルでは、作成後の重要度などをユーザーが管理する。`ensureChannel()`はユーザーの設定を上書きせず、取得結果で実効値を返す。設定変更のためにチャンネルを削除・再作成する動作は含めない。[チャンネルの作成と制約](https://developer.android.com/develop/ui/compose/notifications/channels)、[NotificationChannelのAPIコメント](https://developer.android.com/reference/android/app/NotificationChannel)

API 24・25でも共通のローカル通知は提供する。OSチャンネルが存在しないことを専用APIの利用可否に示し、OSチャンネルの作成要求は非対応として返す。この環境では静的に宣言した用途分類だけに投稿できる。静的設定はmuonが保持し、対応する通知単位の表示属性へ変換できる範囲を明示する。アプリ全体の投稿可否は取得できるが、分類別のOS設定状態は取得できない。チャンネルをユーザーが設定できるようになったとは表示しない。[Android 7.1以前の通知属性](https://developer.android.com/develop/ui/views/notifications/build-notification)

### 通知権限と配布設定

`requestPermission()`は、アプリがユーザーへ通知の用途を説明した後で明示的に呼ぶ。`post()`や起動時のチャンネル登録で、暗黙に権限ダイアログを表示しない。Android 13以降の通常通知は`POST_NOTIFICATIONS`の実行時許可を扱う。[通知権限](https://developer.android.com/develop/ui/compose/notifications/notification-permission)

`getPermissionState()`では、OSの許可状態、ダイアログ要求の実施履歴、設定画面への案内が必要な状態を混同しない。ダイアログを閉じただけの場合も扱い、OSが確定できない状態を恒久拒否と断定しない。API 33未満にはこの権限ダイアログがないため、その事実とアプリ全体の通知可否を分けて返す。

共通設定の`notifications.enabled`が有効な場合に、`android.notifications.channels`と`android.notifications.smallIcon`から必要なManifest宣言と通知用リソースを生成する。既存の`android.permissions`と権限宣言を重複させず、通知を使わないアプリへ新たな実行時権限を要求しない。`notifications.defaultCategory`がある場合は、静的チャンネルの参照先としてビルド時に検証する。動的登録した分類は、`ensureChannel()`が完了した後の`post()`へ明示して使う。

通知用アイコンはランチャーアイコンと分けて指定し、Androidの小アイコンに適したリソースへ変換・検証する。チャンネル名、説明、重要度、既定分類の参照先、アイコンの欠落をビルドで診断する。リソース生成でテキスト処理が必要な場合は既存方針どおり`funcity`を使う。[通知の作成とアイコン](https://developer.android.com/develop/ui/views/notifications/build-notification)

通知アクションボタンを追加する場合も、初期実装では自アプリを開いて操作IDを渡す用途に限定する。プロセスを起動せずに業務処理だけを実行するボタンや直接返信は、後続のバックグラウンド処理の設計で扱う。

## 通知操作とイベントの受け取り

AndroidとCEFで、通知の操作種別、未処理イベント、購読、受領確認の仕様をそろえる。通知の登録情報と未処理イベントはアプリ単位、購読はページの実行コンテキスト単位で管理する。ページのリロードやウインドウの閉鎖では購読を解放し、通知と未処理イベントは破棄しない。最後のウインドウを閉じてプロセスが終了するかどうかは既存のアプリ終了方針に従い、通知を使うだけで常駐させない。

Androidの通知タップでは、アプリが前面にある場合、バックグラウンドにある場合、プロセスが存在しない場合を扱う。Activityの初期Intentと`onNewIntent`を受け取り、同じ形式の通知イベントへ変換する。通知からActivityを開く経路は、公開されているPendingIntentの方式に従う。[通知からActivityを開始する](https://developer.android.com/develop/ui/views/notifications/navigation)

イベントには`eventId`、通知の`id`、操作種別または操作ID、投稿時の`data`を含める。Activityの再生成で同じIntentを再処理する場合は同じイベントとして識別し、同じ通知IDを更新して再び操作した場合は新しいイベントとして扱う。CEFも通常起動、通知による起動、既存プロセスへの通知情報を同じ形式へ変換する。ネイティブ側で未処理イベントを保持し、ページの初期化後に取得できるようにする。JavaScriptのコールバック参照を、アプリ再起動後の受け取り先として保存しない。

`getPendingEvents()`は未受領のイベントを取得し、`watchEvents()`は追加イベントを通知する。アプリは購読を登録してから未処理イベントを取得し、両方に現れたイベントを`eventId`で重複排除する。処理後に`acknowledgeEvent()`を呼ぶ。受領確認前にページやプロセスが終了した場合は再取得できる方式とし、一度しか配送されない保証は設けない。

未処理イベントは永続化し、受領済みイベントの重複配送を防ぐ記録も保持する。保存量と保持期間には上限を定め、上限到達や期限切れの扱いを公開仕様とテストで確定する。OSがアプリを強制停止した場合や通知自体が削除された場合まで、新しい操作イベントの発生を保証しない。[Androidのプロセスライフサイクル](https://developer.android.com/guide/components/activities/process-lifecycle)

購読ハンドルは明示的な解除とページの破棄で解放する。イベントは許可されたメインフレームの購読へだけ渡し、別のActivityや古いページのコールバックを再利用しない。複数のウインドウやWebViewが購読すると同じイベントを受け取る場合があるため、アプリは処理担当のページを一つ決める。受領確認はアプリ全体へ作用し、すでに配送したイベントを取り消す操作にはしない。再配送への対処と、業務処理を重複実行しない方法をドライバーで示す。

## CEFのOS通知実装

### 内蔵プラグインと所有期間

通知の公開関数は既存のC ABIによる内蔵メタデータへ登録し、Windows・Linuxの通知サービスへ委譲する。共通の関数許可とRPCを通し、通知専用のJavaScript直接呼び出し経路を追加しない。現在のPlatform経路はbrowser専用なので、新しい通知名前空間を登録するだけでそこへ配送できるとは扱わない。

現在の[trayサービス](../../../core/cef/src/browser/muon_tray_service.cpp)はBrowser単位で所有され、終了時に削除される。通知サービスはアプリ単位で管理し、trayオブジェクトの有無や寿命に依存させない。通知を使うためだけにトレイアイコンの作成を要求せず、OS通知に失敗した場合もtrayのバルーンやアプリ内表示へ暗黙に置き換えない。

### Windows

Windowsのアプリ通知へ接続し、アプリ名とアイコン、同一IDの更新、取り消し、通知操作を扱う。現在のMicrosoftの推奨APIはWindows App SDKの`AppNotificationManager`である。ただし本プロジェクトはMinGW32/64を使うため、採用前に両方のビルド、APIの利用方法、実行時依存と配布方法をドライバーで検証する。MSVC・ATL専用のサンプルをそのまま導入しない。[通知APIの選択](https://learn.microsoft.com/en-us/windows/apps/develop/notifications/)、[現在のWindowsビルド](../../../core/cef/CMakeLists.txt)

採用するAPIは公式ドキュメントと対象バージョンのヘッダーコメントで確認する。MinGWで利用できない場合は、公開されたWinRT・COM経由の方式を調査し、同じ共通APIの要件を満たせるか確認してから実装手段を確定する。どの方式でも実現できると仮定して先へ進めず、成立しない場合はこの段階で計画を見直す。OS通知を使うためだけにWindowsの配布形式をMSIXへ限定しない。Win32の方式を選ぶ場合も、アプリ識別子など、その方式の登録要件を満たす。[Win32デスクトップ通知の登録要件](https://learn.microsoft.com/en-us/windows/win32/shell/enable-desktop-toast-with-appusermodelid)

既存のruntime appIdはWindowsでアーキテクチャを含む。通知のアプリ識別と起動先もこの単位に合わせ、32bit版と64bit版が互いの通知を更新・取り消ししないようにする。このappIdがあるだけで通知用の登録が済んだとは扱わない。採用方式に必要なアプリ識別、ショートカット属性、COMなどの起動登録をNSISと連携させ、更新とアンインストール時の処理も実装する。[アプリ識別子](../../../ui/cef/targets.ts)、[NSIS生成](../../../ui/cef/pack.ts)、[通知からの起動](https://learn.microsoft.com/en-us/windows/apps/develop/notifications/app-notifications/app-notifications-quickstart)

### Linux

既存のGIO依存を使う`GNotification`を第一候補とする。安定した文字列IDによる更新・取り消しを共通APIへ対応させる。通知の送信完了だけで表示されたとは判断しない。[現在のGIO依存](../../../core/cef/CMakeLists.txt)、[GApplication.send_notificationのAPI仕様とコメント](https://docs.gtk.org/gio/method.Application.send_notification.html)

`GNotification`の利用には、アプリIDと対応する`.desktop`の配置が必要である。終了したアプリを通知から起動する場合は、`GApplication`とD-Busによる起動連携も用意する。通知操作はアプリ単位のアクションへ接続し、すでに存在しない通知オブジェクトやページを配送先にしない。通知の保持はデスクトップ環境の対応に依存する。[GNotificationの要件とAPIコメント](https://docs.gtk.org/gio/class.Notification.html)

既存の`.desktop`生成は通常の`Exec`を扱い、通知からの起動連携は未実装である。アプリID、desktop ID、アイコン、起動実行ファイルをそろえ、D-Busから起動するサービス登録を追加する。AppImageなど起動位置が変わり得る配布物は、既存のdesktop連携と更新時のパス更新を含めて確認する。配置・登録が不足する場合は、必要な条件を診断して対応状況に反映する。[Linux desktop連携](../../../ui/cef/linux-desktop.ts)

GIOのAPIが存在することだけで、どの環境でも保持や操作を利用できるとは扱わない。通知サービスの有無、GNOME・KDEなどの環境差、サービス再起動後の状態を検証する。OSの状態を取得できない場合は`unknown`を返し、ユーザーが通知を許可したと推定しない。

### 通知クリックによる起動

実行中の通知クリックは既存のアプリへ届け、必要なウインドウを開くか前面化する。プロセスが存在しない場合は、配布したアプリのlauncherを通る起動経路へ接続し、共通ランタイムだけを誤って起動しない。ネイティブ側で操作情報を保持してから、ページの初期化と共通イベントの受け取りへ進める。

CEFの既存プロセスへの引数配送には、`root_cache_path`に対応する単一起動機構と`OnAlreadyRunningAppRelaunch`を利用する。Linuxの`GApplication`も既存プロセスへ操作を渡すため、両方の機構が二重のウインドウやイベントを作らないよう接続する。生の起動引数はOSごとに解釈し、アプリ側へは共通イベントを渡す。[CEFの再起動通知とAPIコメント](https://github.com/chromiumembedded/cef/blob/master/include/cef_browser_process_handler.h)

通知の保持と未処理イベントの永続化は別々に検証する。OSから届いた操作を保持することと、OSがまだ届けていない操作の発生を保証することを混同しない。インストール済み配布物でのクリック起動を完成させた後、未登録の展開実行などでは実際に利用できる範囲を返す。

## AndroidのOS設定画面との連携

`settings.open()`の対象は`app-details`、`notifications`、`notification-channel`の識別可能な型で表す。チャンネル設定にだけチャンネルIDを要求する。任意のIntent文字列、他アプリのパッケージ名、任意URLを渡せる汎用起動APIにはしない。

`isAvailable()`は、その端末で対象画面への遷移が可能かを返す。`open()`も実際の起動時に失敗を扱う。専用画面が存在しない場合は非対応を返し、呼び出し側が明示的に許可した場合だけアプリ情報画面へ切り替える。その結果には要求した画面と実際の遷移先を含める。[Settingsの公開アクションとAPIコメント](https://developer.android.com/reference/android/provider/Settings)

`open()`の完了は、画面を開く要求が受理されたことを意味する。ユーザーが設定を変更したかどうかの結果にはしない。Activityが復帰したら権限・通知・バーの実効状態を再取得し、その後の状態取得へ反映する。Web側でも復帰後の取得で更新された表示を確認できるようにする。設定画面を開いている間にプロセスが終了した場合は、新しい起動時に再取得する。

OSの通知設定から自アプリの設定画面を開く逆方向の連携は、後続候補として残す。Androidには通知設定用Activityを示すカテゴリがある。採用時は通知タップの起動情報とは別の起動理由として扱い、対象チャンネルをWeb側の設定画面へ渡す。[通知設定用Activityのカテゴリ](https://developer.android.com/reference/android/app/Notification#INTENT_CATEGORY_NOTIFICATION_PREFERENCES)

## 許可設定と実装上の方針

関数の呼び出し許可、OS側の宣言・アプリ登録、ユーザーが与える権限を別々に検証する。プラグインの`allow`へ関数を追加しても、OSの通知権限が与えられたことにはしない。Android・CEFとも既存のRPCと関数ごとの許可検証を通す。共通APIへの投稿で、Androidの権限ダイアログやWeb通知の許可要求を暗黙に実行しない。

UI操作はActivityとmain Looperに結び付ける。通知の登録情報と未処理イベントはアプリ単位で管理し、Activityの参照を永続的に保持しない。権限ダイアログの同時要求、Activityの再生成中の要求、破棄済みページへの結果配送も扱う。待機のためだけのワーカースレッドや定期ポーリングは追加しない。

Java側はシステムバー、通知、設定の処理を委譲で分離する。CEF側も共通の通知管理とOSごとの処理をインターフェイスと委譲で分け、必要なOSスレッド・イベントループへ接続する。C++のI/Oは既存のcardioを使う方針に従い、待機専用のワーカースレッドやポーリングを追加しない。

TypeScript側の公開定義にはJSDocを付け、状態、単位、購読の解放方法、権限要求の条件を説明する。C++の公開定義にはdoxygen形式のコメントを付ける。新しい外部APIを使う際は、採用バージョンの公式ドキュメントとAPIコメントの両方を確認する。非推奨・内部用のAPIを公開機能の実装手段に選ばない。

## 実装段階と各段階の完了条件

各段階では実際に公開APIを呼ぶテストを先に追加してREDを確認し、その範囲を実装してGREENにする。利用アプリ、Androidテストホスト、CEFのテスト用アプリへ観測用UIを追加し、その段階だけでもビルド・起動できる状態でコミットする。ソースの文字列や設定ファイルの存在だけを合否に使わない。外部APIの成立性を確認する試作も、実際の通知を観測できるドライバーを成果物に含める。

### 段階1 階層名前空間とシステムバーの基本操作

内蔵APIの階層解決、許可フィルター、メタデータ、型定義を拡張し、バー別の表示操作と状態取得を接続する。既存の全画面操作を同じサービスへ委譲し、テストホストから上下別の表示・非表示を切り替えられるようにする。OS非対応の指定と未許可の呼び出しを診断する。

完了条件はsimple・validateの両モードで新しい階層の関数を呼べること、許可していない関数を呼べないこと、既存fullscreenと個別操作の要求状態が食い違わないことである。デスクトップの関数一覧と既存の全画面操作に影響しないことも確認する。

コミットの目安は`feat: add Android system bar plugin APIs`とする。

### 段階2 安全領域と起動時表示

`contentLayout`、Insetsの取得・購読、CSS変数、アイコン配色、初期設定を追加する。サンプルに画面端の操作部品とInsetsの表示を置き、配置方式を切り替えて比較できるようにする。回転、ズーム、キーボード、Activity再生成にも接続する。

完了条件は、操作部品が指定した安全領域へ配置されること、余白が二重にならないこと、回転とズーム後もCSS上の位置が一致すること、初期表示と復元が設定どおりであることとする。バーの一時表示やキーボードの遷移は動画を記録し、フレームと状態変化を対応させてテストでアサートする。

コミットの目安は`feat: expose Android insets and initial display settings`とする。

### 段階3 OS設定画面と通知権限

自アプリの情報画面、通知設定画面、利用可否、復帰時の状態再取得を実装する。通知の有効設定と必要なManifest宣言を生成し、権限状態の取得と明示的な要求を接続する。サンプルから要求・拒否・設定画面への移動・復帰を確認できるようにする。

完了条件はダイアログの許可・拒否・閉じる操作をそれぞれ試験し、観測できる状態と判定できない状態を正しく返すこととする。操作理由の完全な識別は保証しない。OS設定で変更した状態が復帰後に反映されること、画面が存在しない場合とActivityが破棄された場合の結果を返せること、通知を有効にしていないアプリで新しい権限要求が起きないことも確認する。

コミットの目安は`feat: integrate Android settings and notification permission`とする。

### 段階4 共通のローカル通知とチャンネル

共通の`notifications.enabled`と`notifications.defaultCategory`、Androidの静的チャンネル設定、動的登録、通知アイコンの生成を接続する。共通の`post()`、`cancel()`、`getStatus()`、`getCapabilities()`と、チャンネル設定画面への遷移を実装する。サンプルから投稿、同一IDの更新、取り消し、用途分類別の状態と対応機能の確認を実行できるようにする。

完了条件は生成した利用アプリのAPKから通知を投稿でき、同じIDの更新が別通知を増やさず、取り消しが対象だけに作用することとする。チャンネルの無効化、未知の分類、権限拒否、既定分類、API 24・25の動作も検証する。通知はこの段階でもタップでアプリを開ける状態とし、操作データの受け渡しは次段階で完成させる。対応機能は実装済みの範囲を返し、未完成のイベント機能を対応済みとしない。

コミットの目安は`feat: add shared local notifications with Android channels`とする。

### 段階5 Androidの通知操作と共通イベント

通知タップ、必要な起動型アクション、初期Intentと後続Intentを同じイベントへ変換する。未処理イベントの保存、取得、購読、受領確認、保持上限を実装する。サンプルではイベントのデータから対象画面を開き、受領確認前の終了と再配送も観測できるようにする。

完了条件は、前面・バックグラウンド・プロセス終了後のタップで正しい通知データが届くこと、WebViewの初期化より前のイベントを失わないこと、受領確認と重複排除が機能することである。回転後の同じIntentの再処理と、通知を更新した後の新しいタップも区別して試験する。リロード、複数のWebView、購読解除、保存上限、アプリ更新後の既存通知も確認する。JSのコールバックを保存せずに再起動後の操作を扱えることを合否に含める。

コミットの目安は`feat: deliver notification events across Android restarts`とする。

### 段階6 CEFの公開経路とLinuxのローカル通知

内蔵登録、simple・validateの公開、共通設定のビルドへの受け渡し、購読の解放を接続する。Linuxの通知サービスを実装し、必要な`.desktop`とアイコンを配置したドライバーから投稿、更新、取り消し、状態取得、実行中のクリック受信を試せるようにする。共通イベントの保存・受領確認も利用し、ウインドウのリロードと閉鎖を観測する。

完了条件は両モードの公開APIから実際のOS通知を操作できること、Android用設定なしで任意の用途分類を使えること、アプリ全体と分類別の状態が不明な場合を表現できることとする。通知サービス不在、操作非対応、同一IDの更新、別ウインドウからの取り消し、購読解放を検証する。この段階ではプロセス終了後のクリック起動を非対応と示し、通知の保持とは区別する。Windows側の未実装関数は公開しない。

コミットの目安は`feat: add shared notification APIs for CEF Linux`とする。

### 段階7 Windowsのローカル通知

MinGW32/64で利用できるOS通知APIと配布依存を、ビルド・実行可能なドライバーで確定する。その結果に従ってWindowsの通知サービスと必要なアプリ登録を実装し、段階6と同じ共通APIへ接続する。実行中の通知クリックを共通イベントへ変換する。

完了条件は32bit・64bitのビルドとWindows実機で投稿、更新、取り消し、クリック受信を確認し、アプリ名とアイコンが対象アプリのものとして表示されることとする。各アーキテクチャの通知を混同せず、登録不足やOS側の無効化を診断する。CEFの購読破棄、権限フィルター、未処理イベントをLinuxと同じ振る舞いで検証する。プロセス終了後のクリック起動はこの段階では非対応と示す。

コミットの目安は`feat: add native Windows notification support`とする。

### 段階8 CEFの通知起動と配布登録

Windowsの通知起動登録とNSIS、LinuxのD-Bus起動とdesktop連携を完成させる。新規起動と既存プロセスへの配送を共通イベントへ接続し、対象画面を開くドライバーを用意する。アプリ更新で既存通知の起動情報を引き継ぎ、アンインストールでは自アプリの登録を解除する。

完了条件はWindowsのNSIS導入環境とLinuxのD-Bus起動に対応する導入環境で、プロセスを終了した後の通知クリックから配布アプリが起動し、正しいデータを受け取ることとする。実行中のクリックで二重起動せず、ページ初期化前のイベント、受領前の終了、再配送、複数ウインドウを検証する。更新後の古い通知、起動パスの変更、アンインストール後の残存通知、通知サービスの再起動も確認する。

未登録の展開実行と非対応のデスクトップ環境では、理由を含む対応状況を返す。対応する基準環境でのクリック起動は必須とし、すべての環境を非対応や不明にして完了とはしない。GNOME・KDEそれぞれの結果と、Windows各アーキテクチャの配布・登録条件を記録する。

コミットは`feat: activate CEF apps from native notifications`を目安とし、OSや配布形式ごとに動作を確認できる単位へ分ける。

### 段階9 配布物・全体検証・利用文書

公開CLIで利用アプリをビルド・パッケージ化し、事前ビルドしたランタイムから全機能を利用できることを確認する。AndroidのAPK、WindowsのNSISと展開実行、Linuxの既存配布形式で、通知の登録条件と対応状況を検証する。日本語のAndroidガイド、内蔵プラグインリファレンス、CEFの配布ガイドを更新し、それを原本として英語版へ反映する。計画書へのリンクは一般の利用文書に追加しない。

完了条件は、次節の全体テストと端末テストが成功し、必須ケースが未実行のまま残っていないこと、ビルド警告を分析・対処したこと、利用文書がAPIの対応範囲と失敗条件を説明していることである。

コミットは必要に応じて機能の修正と文書を分け、`fix:`、`chore:`、`doc:`を使う。計画外の機能追加が必要になった場合は、その場で範囲を広げず、原因と計画の修正を先に整理する。

## 検証方法

TypeScriptの単体テストは既存のVitest、Web側のドライバーは既存のTypeScript・Viteの構成を使う。AndroidのネイティブUIは既存のinstrumentationとobserverで操作・観測する。ブラウザーを介した操作とWebView・CEF内のE2EにはPlaywright MCPを使う。OS側の通知表示を、JavaScriptのPromise成功だけで判定しない。

| 検証対象 | 確認する動作 |
| --- | --- |
| APIと許可 | Android・CEFの両モード、階層解決、関数ごとの許可、無効ビルドでの非公開、実行時の非対応・不明、購読解放。 |
| バーと配置 | 上下別の表示、既存fullscreenとの併用、3ボタンとジェスチャーナビゲーション、縦横画面、カットアウト、マルチウインドウ。 |
| Insets | DOMの位置、ズームと密度の換算、キーボードの区別、二重余白の防止、変更イベント。 |
| 権限と設定 | 初回要求、許可、拒否、ダイアログを閉じる操作、再要求、設定からの復帰、遷移先の非対応。 |
| 通知 | 各OSの表示内容とアプリ名・アイコン、IDによる更新・取り消し、分類とチャンネル、全体・チャンネル別の無効化。 |
| 対応機能 | クリック受信と終了後の起動の区別、通知の保持、サービス不在、登録不足、状態取得不可、非対応指定の失敗。 |
| イベント | 起動中と停止中のタップ・クリック、初期化前の保持、再配送、受領確認、複数ウインドウ、古い購読への誤配送防止。 |
| 配布 | 通知なし・ありのアプリ、静的・動的チャンネル、Manifestとリソース、APK・NSIS・Linux配布物の生成と実行。 |
| CEFの登録と更新 | appIdと起動先、32bit版と64bit版の分離、アプリ更新、パス変更、アンインストール、通知サービス再起動。 |

OSの差は、チャンネル導入前のAPI 24・25、チャンネルがあるAPI 26以降、通知権限を扱うAPI 33以降、edge-to-edgeの扱いが変わるAPI 35・36以降で確認する。現在のtargetSdkであるAPI 37の環境も対象に含める。境界を再現するエミュレーターと、既存のPixel 6実機を組み合わせる。実際に実行したOS/API、ナビゲーション方式、画面形状を記録する。

Linuxでは既存の[CEFテスト入口](../../../core/cef-test/package.json)が使う[隔離D-Busセッション](../../../scripts/run-dbus-session-command.mjs)に、通常の通知デーモンが自動で存在するとは仮定しない。制御可能なテスト用通知サービスを用意して、対応機能、クリックイベント、サービス再起動を再現する。それとは別にGNOME・KDEで実際の通知表示、クリック、アプリの再起動を確認する。

WindowsではMinGW32/64のビルドに加え、`agent-rover`を使う既存の実機試験へ通知ケースを追加する。Wineでは実OSの通知センターや起動登録の試験を代替しない。Windows接続がない場合に既存のE2Eがskipする結果は、通知機能の検証成功に数えない。[Windows E2E](../../../core/cef-test/plugin-e2e-windows.test.ts)、[NSIS E2E](../../../ui/cef-test/nsis-windows-e2e.test.ts)

画面遷移やバーのアニメーションは、状態通知やUIの条件を待って判定する。一定時間sleepしただけで成功としない。動画を使うケースは時間軸のアサーションを実装し、画像比較用のマスターを追加する場合は事前に目視で確認する。

実装完了時にはリポジトリルートの`npm test`を必ず実行する。この入口だけで端末上の全ケースを実行したとは扱わず、既存の次の入口も使う。

```bash
npm test
npm run test:android --workspace muon-android-tester
npm run test:android:pixel6 --workspace muon-android-tester
npm run test:windows-e2e --workspace muon-core-tester
npm run test:windows-nsis-e2e --workspace muon-ui
```

LinuxのCEFテストは`npm test`に含まれる。通知サービスを制御する試験と実デスクトップの試験は、段階6・8で実行入口を追加して記録する。既存のテストが通っただけで、この追加ケースまで実行したとは扱わない。

Android端末は`ANDROID_SERIAL`で指定し、x86_64・16 KiBページとarm64実機の既存の検証を維持する。追加のOS境界試験はその環境に適したinstrumentationの実行を用意する。利用できない端末・権限・依存ツールによって必須ケースを実行できない場合は、未検証として記録し、完了とはしない。

今回の計画書保存は文書のみの変更であり、ビルドや製品テストの対象ではない。保存時には内容の照合、日本語の推敲、相対リンク、Markdownと差分を確認する。

## 全体の完了条件

1. 共通通知とOS専用機能の役割が分かれ、Android・CEFで投稿・取り消し・イベントの同じAPIを使える。未実装のOS専用APIを公開していない。
2. システムバー、ノッチ、安全領域、起動時表示を制御でき、既存fullscreenと状態を共有している。
3. Androidの通知権限、チャンネル、設定画面への導線と、Android・Windows・Linuxの投稿・更新・取り消しが公開APIから利用できる。
4. AndroidとCEFの対応する導入環境で、アプリの新規起動と実行中の通知操作を受け取り、未処理イベントと受領確認を扱える。
5. simple・validate、関数の許可、Activity再生成、複数ウインドウ、ページ破棄、購読解放の動作を確認している。ページの破棄でアプリ単位の通知と未処理イベントを失わない。
6. Androidの最低APIレベルを維持する。`getCapabilities()`と`getStatus()`で機能の対応と現在の投稿可否を区別し、非対応・不明・登録不足を明示する。
7. 共通の有効設定と既定分類、Android固有のチャンネルとアイコンを分離し、CEFでAndroid用設定を要求しない。
8. Windows・Linuxのアプリ識別、通知起動、既存プロセスへの配送、更新・アンインストールを配布物で確認している。
9. 公開CLIで生成した配布物、全体テスト、必要なOS境界と実機の試験が成功している。Windows E2Eのskipや通知サービスがない環境だけの試験を完了に数えていない。
10. 日本語の利用文書と、その内容に対応した英語版が更新されている。

## 計画書保存時の確認

Androidの案Bと案Cの組み合わせに加え、CEFの案2を反映した。共通APIとOS固有拡張、対応機能と投稿可否、共通設定、Windows・Linuxの実装候補、通知の所有期間、クリック起動と配布登録を記載した。実装段階を9段階へ更新し、各段階で観測できる成果物、検証方法、完了条件を追加した。予約通知・FCM・継続実行・逆方向の設定連携、Web標準APIの互換機能は後続対象として残している。

実装段階と全体の完了条件は未達成である。本書の保存を、機能実装や端末検証の完了とは扱わない。
