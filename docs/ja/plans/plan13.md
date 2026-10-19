# Android内蔵プラグインと共通通知API

## 目的と採用方針

Androidのシステムバー制御、通知、OS設定画面との連携を、muonの内蔵プラグインとして提供する。Android専用の機能を名前空間ごとに分ける案Bと、共通操作を共通APIへ置く案Cを組み合わせる。

共通の`muon.notifications`は通知の投稿、更新、取り消し、投稿可否の確認、通知操作の受け取りを担当する。Android専用の`muon.android.*`は、システムバーと安全領域、通知チャンネルと権限、設定画面への遷移を担当する。既存の`muon.browser`の全画面操作は、Android専用のシステムバー制御と実装・状態を共有する。

本計画の初期実装はAndroidを対象とする。共通APIという名称は、複数のOSで同じ操作を表現できる仕様を意味する。デスクトップ通知の実装済みを意味しない。デスクトップへの実装を追加するまでは、その対象の関数一覧へ新しい通知APIを登録しない。

作成日は2026年10月19日。調査の基準コミットは`7194a156ce13ebaeee1317fbe3a19b7a5afc70d0`である。本書は実装前の計画であり、以下のAPI名、設定項目、動作は追加する仕様を示す。現在の作業は計画書の保存までとし、実装と検証の結果は実施後に記録する。

## 用語と対象範囲

システムバーは、上端のステータスバーと下端のナビゲーションバーを指す。ノッチなど、センサーのために表示できない部分をディスプレイカットアウトと呼ぶ。InsetsはシステムUIやカットアウトと重ならないために必要な各辺の余白である。画面端まで背景などを描く配置をedge-to-edgeと呼び、システムバーを隠す全画面表示とは区別する。

通知チャンネルは、Androidでユーザーが通知音や重要度を設定する単位である。共通通知APIでは、アプリが通知を用途別に分けるキーを`category`と呼ぶ。初期実装ではこのキーをAndroidのチャンネルIDに直接対応させる。

本計画に含めるのは、バーの表示と配色、安全領域の取得と変更通知、即時のローカル通知、通知タップによる起動とデータの受け渡し、自アプリのOS設定画面への遷移である。ローカル通知は、アプリが端末内で発行する通知を意味する。

次の機能は後続の検討対象として残す。

- デスクトップ向けの通知バックエンド。
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

外部プラグインを読み込む形式へ変更する必要はない。登録名は`internal`のままとし、公開APIとJava側の処理を機能別に分ける。外部プラグイン用のABIや、サブモジュール・ベンダーコードは変更しない。

[現在のツールチェーン](../../../core/android/toolchain.json)は`minSdk: 24`、`compileSdk: 37`、`targetSdk: 37`である。本機能のためだけに最低APIレベルを引き上げず、OSに存在しない機能は利用可否として示す。未対応の指定を成功扱いで無視しない。

## 公開APIの役割分担

関数は既存APIと同様にPromiseを返す。以下の表は名前空間と責務を決めるもので、TypeScriptの型定義は各実装段階で追加する。

| 名前空間 | 主な関数案 | 担当する操作 |
| --- | --- | --- |
| `muon.browser` | `enterFullscreen()`、`exitFullscreen()`、`toggleFullscreen()` | 既存の共通全画面操作。Androidでは同じシステムバーサービスを使う。 |
| `muon.notifications` | `post(options)`、`cancel(id)`、`getStatus(options?)` | 共通形式での投稿・更新・取り消し、アプリ全体と用途分類別の投稿可否。 |
| `muon.notifications` | `getPendingEvents()`、`watchEvents(handler)`、`acknowledgeEvent(eventId)` | 起動時と実行中の通知操作を、同じイベント形式で受け取る。 |
| `muon.android.systemBars` | `configure(options)`、`getState()`、`getInsets()`、`watchInsets(handler)` | バー別の表示と配色、配置方針、実際の表示状態、安全領域。 |
| `muon.android.notifications` | `getPermissionState()`、`requestPermission()`、`ensureChannel(options)`、`getChannels()` | Androidの通知権限とチャンネルの作成・状態取得。 |
| `muon.android.settings` | `isAvailable(target)`、`open(target, options?)` | 自アプリの情報、通知全体、指定チャンネルの設定画面への遷移。 |

Android専用側へ`post()`、`cancel()`、通知イベントの別APIを追加しない。設定画面を開く操作も`muon.android.settings`へ集約する。これにより、同じ操作の状態管理や許可設定が複数箇所へ分かれることを避ける。

validateモードでは`muon:notifications`、`muon:android.systemBars`、`muon:android.notifications`、`muon:android.settings`からインポートする。simpleモードでは対応する`window.muon.*`の階層を公開する。関数が提供されていない対象では、validateのビルドでそのインポートを拒否し、simpleのオブジェクトにも関数を作らない。

新しい共通APIの初期実装をAndroidに限定しても、既存のデスクトップ全画面操作の意味は変えない。デスクトップ通知を追加するときは、OS固有のチャンネルや権限を共通APIの必須引数にしない方針を保つ。

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

`getInsets()`と`watchInsets()`では、システムバー、カットアウト、システムジェスチャー、ソフトウェアキーボードの余白を区別する。キーボードの表示で、ノッチを避けるための余白まで変わったように見せない。安全領域の合成では各辺の必要量を求め、バーとカットアウトの値を単純加算しない。[edge-to-edgeとInsets](https://developer.android.com/develop/ui/views/layout/edge-to-edge)

Web側に渡す値はCSSピクセルを基本とし、端末の物理ピクセル、画面密度、WebViewのページズームとの換算方法を定義する。回転、ウインドウサイズの変更、キーボード表示、既存のズームAPIに追従する。換算結果は、実際のDOMの位置とネイティブ領域を比較して検証する。

必要な余白は`--muon-safe-area-top`などのCSS変数としても公開する。`safe-area`ではネイティブ側ですでに処理した分を再適用させず、Web側に残る余白を示す。生のInsetsと、コンテンツが追加で避ける量を型と名前で区別する。標準の`env(safe-area-inset-*)`をmuonの値と同一と仮定しない。

`watchInsets()`は購読時点の状態と、その後の変更を届ける。解除可能なハンドルを返し、明示的な解除、ページ遷移、リロード、Activityの破棄で購読を解放する。アプリ側に定期的な取得を要求しない。

### 起動時設定

`android.systemBars`へ初期のバー表示、アイコン配色、`contentLayout`を追加する。未指定時は上下のバーを表示し、`contentLayout`を`safe-area`とする。アイコン配色の未指定値にはAndroidテーマの既定値を使う。JavaScriptが起動する前に適用し、実行時の`configure()`と同じ設定処理を使う。起動時設定を既定値とし、Activityの構成変更による再生成では実行中の要求状態を復元する。プロセスが新規起動した場合は設定ファイルの初期値から開始する。

`browser.initialWindowState`も同じ初期化へ接続し、明示的な`normal`は上下のバーの表示、`fullscreen`は両方の非表示を表す。Android専用の初期バー表示も明示して矛盾する場合は、ビルド時にエラーを返す。`initialWindowState`の省略を明示的な`normal`として扱わず、その場合はAndroid専用の指定を使う。背景色はコンテンツ側の描画として扱い、OSバージョンによって意味が変わるバー背景色の直接設定を共通仕様に含めない。

## 共通通知APIとAndroid拡張

### 共通の投稿・更新・取り消し

`post()`の基本項目は、アプリ内で安定した文字列の`id`、`title`、`body`、用途分類の`category`、アプリへ返す`data`とする。`data`はJSONとして保存・復元できる値に限定し、関数やネイティブオブジェクトを受け付けない。通知をタップすると自アプリを開き、このデータをアプリ内の画面選択などに使えるようにする。

同じ`id`を再投稿すると、その通知を更新する。`cancel(id)`は該当通知を取り消し、存在しないIDでも完了する。標準のタップ操作では通知を自動的に取り消す。アプリが識別する`id`をプロセス再起動後も同じ通知へ対応させ、更新や取り消しが別の通知へ作用しないようにする。

`post()`の成功は、OSへの投稿処理が完了したことを表す。ユーザーが見たことや、必ず音・画面表示が発生することまでは保証しない。既知の権限拒否、通知全体の無効化、指定チャンネルの無効化は呼び出し時に診断する。事前に状態を取得した後でOS設定が変わる場合も扱う。

`getStatus()`は、引数を省略した場合にはアプリ全体の投稿可否を返す。`category`を指定した場合には、アプリ全体とその分類の状態を区別して返す。投稿可能、無効、判定できない状態を表し、通知の閲覧状態や一時的な表示の抑制とは分ける。Android固有の詳細は、専用APIの権限状態とチャンネル情報で取得する。

### 用途分類とチャンネル

初期実装では`category`とAndroidのチャンネルIDを同じ文字列にする。ここでの`category`はmuonの用途分類であり、Androidのアラームなどを表す通知のシステム分類とは別である。別の対応表や登録ハンドルは導入しない。`post()`で`category`を省略した場合は、`android.notifications.defaultCategory`を使う。[Androidのチャンネルとシステム分類](https://developer.android.com/develop/ui/compose/notifications/channels)

`android.notifications.channels`で静的なチャンネルを宣言し、アプリ起動時に登録する。実行時に用途を増やすアプリ向けには`ensureChannel()`を提供する。未登録の`category`への投稿は失敗させ、投稿の副作用として名前や重要度が未指定のチャンネルを作らない。チャンネルを動的に登録する場合は、`ensureChannel()`の完了後に投稿する。

Android 8以降のチャンネルでは、作成後の重要度などをユーザーが管理する。`ensureChannel()`はユーザーの設定を上書きせず、取得結果で実効値を返す。設定変更のためにチャンネルを削除・再作成する動作は含めない。[チャンネルの作成と制約](https://developer.android.com/develop/ui/compose/notifications/channels)、[NotificationChannelのAPIコメント](https://developer.android.com/reference/android/app/NotificationChannel)

API 24・25でも共通のローカル通知は提供する。OSチャンネルが存在しないことを専用APIの利用可否に示し、OSチャンネルの作成要求は非対応として返す。この環境では静的に宣言した用途分類だけに投稿できる。静的設定はmuonが保持し、対応する通知単位の表示属性へ変換できる範囲を明示する。アプリ全体の投稿可否は取得できるが、分類別のOS設定状態は取得できない。チャンネルをユーザーが設定できるようになったとは表示しない。[Android 7.1以前の通知属性](https://developer.android.com/develop/ui/views/notifications/build-notification)

### 通知権限と配布設定

`requestPermission()`は、アプリがユーザーへ通知の用途を説明した後で明示的に呼ぶ。`post()`や起動時のチャンネル登録で、暗黙に権限ダイアログを表示しない。Android 13以降の通常通知は`POST_NOTIFICATIONS`の実行時許可を扱う。[通知権限](https://developer.android.com/develop/ui/compose/notifications/notification-permission)

`getPermissionState()`では、OSの許可状態、ダイアログ要求の実施履歴、設定画面への案内が必要な状態を混同しない。ダイアログを閉じただけの場合も扱い、OSが確定できない状態を恒久拒否と断定しない。API 33未満にはこの権限ダイアログがないため、その事実とアプリ全体の通知可否を分けて返す。

通知機能を有効にするビルド設定として、`android.notifications.enabled`、`defaultCategory`、`channels`、通知用の`smallIcon`を用意する。`enabled`の未指定値は`false`とする。無効なビルドでは共通通知とAndroid専用通知の関数をカタログから除外し、validateのインポートをビルド時に拒否する。simpleでも公開しない。有効時は必要なManifest宣言と通知用リソースを生成し、既存の`android.permissions`と重複しないようにする。通知を使わないアプリへ新たな実行時権限を要求しない。

通知用アイコンはランチャーアイコンと分けて指定し、Androidの小アイコンに適したリソースへ変換・検証する。チャンネル名、説明、重要度、既定分類の参照先、アイコンの欠落をビルドで診断する。リソース生成でテキスト処理が必要な場合は既存方針どおり`funcity`を使う。[通知の作成とアイコン](https://developer.android.com/develop/ui/views/notifications/build-notification)

通知アクションボタンを追加する場合も、初期実装では自アプリを開いて操作IDを渡す用途に限定する。プロセスを起動せずに業務処理だけを実行するボタンや直接返信は、後続のバックグラウンド処理の設計で扱う。

### 通知タップとイベントの受け取り

通知タップでは、アプリが前面にある場合、バックグラウンドにある場合、プロセスが存在しない場合を扱う。Activityの初期Intentと`onNewIntent`を受け取り、同じ形式の通知イベントへ変換する。通知からActivityを開く経路は、公開されているPendingIntentの方式に従う。[通知からActivityを開始する](https://developer.android.com/develop/ui/views/notifications/navigation)

イベントには`eventId`、通知の`id`、操作種別または操作ID、投稿時の`data`を含める。Activityの再生成で同じIntentを再処理する場合は同じイベントとして識別し、同じ通知IDを更新して再びタップした場合は新しいイベントとして扱う。ネイティブ側で未処理イベントを保持し、WebViewの初期化後に取得できるようにする。JavaScriptのコールバック参照を、アプリ再起動後の受け取り先として保存しない。

`getPendingEvents()`は未受領のイベントを取得し、`watchEvents()`は追加イベントを通知する。アプリは購読を登録してから未処理イベントを取得し、両方に現れたイベントを`eventId`で重複排除する。処理後に`acknowledgeEvent()`を呼ぶ。受領確認前にページやプロセスが終了した場合は再取得できる方式とし、一度しか配送されない保証は設けない。

未処理イベントは永続化し、受領済みイベントの重複配送を防ぐ記録も保持する。保存量と保持期間には上限を定め、上限到達や期限切れの扱いを公開仕様とテストで確定する。OSがアプリを強制停止した場合や通知自体が削除された場合まで、新しい操作イベントの発生を保証しない。[Androidのプロセスライフサイクル](https://developer.android.com/guide/components/activities/process-lifecycle)

購読ハンドルは明示的な解除とページの破棄で解放する。イベントは許可されたメインフレームの購読へだけ渡し、別のActivityや古いページのコールバックを再利用しない。同じアプリの複数のWebViewが購読する場合の配送先は段階5で確定し、単一Activityの試験だけで完了扱いにしない。

## OS設定画面との連携

`settings.open()`の対象は、`app-details`、`notifications`、`notification-channel`の識別可能な型で表す。チャンネル設定にだけチャンネルIDを要求する。任意のIntent文字列、他アプリのパッケージ名、任意URLを渡せる汎用起動APIにはしない。

`isAvailable()`は、その端末で対象画面への遷移が可能かを返す。`open()`も実際の起動時に失敗を扱う。専用画面が存在しない場合は非対応を返し、呼び出し側が明示的に許可した場合だけアプリ情報画面へ切り替える。その結果には要求した画面と実際の遷移先を含める。[Settingsの公開アクションとAPIコメント](https://developer.android.com/reference/android/provider/Settings)

`open()`の完了は、画面を開く要求が受理されたことを意味する。ユーザーが設定を変更したかどうかの結果にはしない。Activityが復帰したら権限・通知・バーの実効状態を再取得し、その後の状態取得へ反映する。Web側でも復帰後の取得で更新された表示を確認できるようにする。設定画面を開いている間にプロセスが終了した場合は、新しい起動時に再取得する。

OSの通知設定から自アプリの設定画面を開く逆方向の連携は、後続候補として残す。Androidには通知設定用Activityを示すカテゴリがある。採用時は通知タップの起動情報とは別の起動理由として扱い、対象チャンネルをWeb側の設定画面へ渡す。[通知設定用Activityのカテゴリ](https://developer.android.com/reference/android/app/Notification#INTENT_CATEGORY_NOTIFICATION_PREFERENCES)

## 許可設定と実装上の方針

関数の呼び出し許可、Manifestの権限宣言、ユーザーが与える実行時権限を別々に検証する。プラグインの`allow`へ関数を追加しても、OSの通知権限が与えられたことにはしない。Javaへの新しい直接呼び出し経路を作らず、既存のRPCと関数ごとの許可検証を通す。

UI操作はActivityとmain Looperに結び付ける。通知の登録情報と未処理イベントはアプリ単位で管理し、Activityの参照を永続的に保持しない。権限ダイアログの同時要求、Activityの再生成中の要求、破棄済みページへの結果配送も扱う。待機のためだけのワーカースレッドや定期ポーリングは追加しない。

Java側はシステムバー、通知、設定の処理を委譲で分離する。TypeScript側の公開定義にはJSDocを付け、状態、単位、購読の解放方法、権限要求の条件を説明する。新しい外部APIを使う際は、採用バージョンの公式ドキュメントとAPIコメントの両方を確認する。非推奨・内部用のAPIを公開機能の実装手段に選ばない。

## 実装段階と各段階の完了条件

各段階では実際に公開APIを呼ぶテストを先に追加してREDを確認し、その範囲を実装してGREENにする。利用アプリまたは既存のAndroidテストホストへ観測用UIを追加し、その段階だけでもビルド・起動できる状態でコミットする。ソースの文字列や設定ファイルの存在だけを合否に使わない。

### 段階1 階層名前空間とシステムバーの基本操作

内蔵APIの階層解決、許可フィルター、メタデータ、型定義を拡張し、バー別の表示操作と状態取得を接続する。既存の全画面操作を同じサービスへ委譲し、テストホストから上下別の表示・非表示を切り替えられるようにする。OS非対応の指定と未許可の呼び出しを診断する。

完了条件は、simple・validateの両モードで新しい階層の関数を呼べること、許可していない関数を呼べないこと、既存fullscreenと個別操作の要求状態が食い違わないことである。デスクトップの関数一覧と既存の全画面操作に影響しないことも確認する。

コミットの目安は`feat: add Android system bar plugin APIs`とする。

### 段階2 安全領域と起動時表示

`contentLayout`、Insetsの取得・購読、CSS変数、アイコン配色、初期設定を追加する。サンプルに画面端の操作部品とInsetsの表示を置き、配置方式を切り替えて比較できるようにする。回転、ズーム、キーボード、Activity再生成にも接続する。

完了条件は、操作部品が指定した安全領域へ配置されること、余白が二重にならないこと、回転とズーム後もCSS上の位置が一致すること、初期表示と復元が設定どおりであることとする。バーの一時表示やキーボードの遷移は動画を記録し、フレームと状態変化を対応させてテストでアサートする。

コミットの目安は`feat: expose Android insets and initial display settings`とする。

### 段階3 OS設定画面と通知権限

自アプリの情報画面、通知設定画面、利用可否、復帰時の状態再取得を実装する。通知の有効設定と必要なManifest宣言を生成し、権限状態の取得と明示的な要求を接続する。サンプルから要求・拒否・設定画面への移動・復帰を確認できるようにする。

完了条件は、ダイアログの許可・拒否・閉じる操作をそれぞれ試験し、観測できる状態と判定できない状態を正しく返すこととする。操作理由の完全な識別は保証しない。OS設定で変更した状態が復帰後に反映されること、画面が存在しない場合とActivityが破棄された場合の結果を返せること、通知を有効にしていないアプリで新しい権限要求が起きないことも確認する。

コミットの目安は`feat: integrate Android settings and notification permission`とする。

### 段階4 共通のローカル通知とチャンネル

静的チャンネル設定、動的登録、既定分類、通知アイコンの生成を追加する。共通の`post()`、`cancel()`、`getStatus()`と、チャンネル設定画面への遷移を実装する。サンプルから投稿、同一IDの更新、取り消し、用途分類別の状態確認を実行できるようにする。

完了条件は、生成した利用アプリのAPKから通知を投稿でき、同じIDの更新が別通知を増やさず、取り消しが対象だけに作用することとする。チャンネルの無効化、未知の分類、権限拒否、既定分類、API 24・25の動作も検証する。通知はこの段階でもタップでアプリを開ける状態とし、操作データの受け渡しは次段階で完成させる。

コミットの目安は`feat: add shared local notifications with Android channels`とする。

### 段階5 通知操作とアプリの再起動

通知タップ、必要な起動型アクション、初期Intentと後続Intentを同じイベントへ変換する。未処理イベントの保存、取得、購読、受領確認、保持上限を実装する。サンプルではイベントのデータから対象画面を開き、受領確認前の終了と再配送も観測できるようにする。

完了条件は、前面・バックグラウンド・プロセス終了後のタップで正しい通知データが届くこと、WebViewの初期化より前のイベントを失わないこと、受領確認と重複排除が機能することである。回転後の同じIntentの再処理と、通知を更新した後の新しいタップも区別して試験する。リロード、複数のWebView、購読解除、保存上限、アプリ更新後の既存通知も確認する。JSのコールバックを保存せずに再起動後の操作を扱えることを合否に含める。

コミットの目安は`feat: deliver notification events across Android restarts`とする。

### 段階6 配布物・全体検証・利用文書

公開CLIで利用アプリをビルド・パッケージ化し、事前ビルドしたランタイムから全機能を利用できることを確認する。日本語のAndroidガイド、内蔵プラグインリファレンス、制約の説明を更新し、それを原本として英語版へ反映する。計画書へのリンクは一般の利用文書に追加しない。

完了条件は、次節の全体テストと端末テストが成功し、必須ケースが未実行のまま残っていないこと、ビルド警告を分析・対処したこと、利用文書がAPIの対応範囲と失敗条件を説明していることである。

コミットは必要に応じて機能の修正と文書を分け、`fix:`、`chore:`、`doc:`を使う。計画外の機能追加が必要になった場合は、その場で範囲を広げず、原因と計画の修正を先に整理する。

## 検証方法

TypeScriptの単体テストは既存のVitest、Web側のドライバーは既存のTypeScript・Viteの構成を使う。AndroidのネイティブUIは既存のinstrumentationとobserverで操作・観測する。ブラウザーを介した操作やWebView内のE2EにはPlaywright MCPを使う。OS側の通知表示を、JavaScriptのPromise成功だけで判定しない。

| 検証対象 | 確認する動作 |
| --- | --- |
| APIと許可 | 両モードの階層解決、関数ごとの許可、未対応対象でのビルド診断、購読解放。 |
| バーと配置 | 上下別の表示、既存fullscreenとの併用、3ボタンとジェスチャーナビゲーション、縦横画面、カットアウト、マルチウインドウ。 |
| Insets | DOMの位置、ズームと密度の換算、キーボードの区別、二重余白の防止、変更イベント。 |
| 権限と設定 | 初回要求、許可、拒否、ダイアログを閉じる操作、再要求、設定からの復帰、遷移先の非対応。 |
| 通知 | 表示内容とアイコン、IDによる更新・取り消し、分類とチャンネル、全体・チャンネル別の無効化。 |
| イベント | 起動中と停止中のタップ、初期化前の保持、再配送、受領確認、古い購読への誤配送防止。 |
| 配布 | 通知なしのアプリ、通知ありのアプリ、静的・動的チャンネル、Manifestとリソース、公開CLIによるAPK生成。 |

OSの差は、チャンネル導入前のAPI 24・25、チャンネルがあるAPI 26以降、通知権限を扱うAPI 33以降、edge-to-edgeの扱いが変わるAPI 35・36以降で確認する。現在のtargetSdkであるAPI 37の環境も対象に含める。境界を再現するエミュレーターと、既存のPixel 6実機を組み合わせる。実際に実行したOS/API、ナビゲーション方式、画面形状を記録する。

画面遷移やバーのアニメーションは、状態通知やUIの条件を待って判定する。一定時間sleepしただけで成功としない。動画を使うケースは時間軸のアサーションを実装し、画像比較用のマスターを追加する場合は事前に目視で確認する。

実装完了時にはリポジトリルートの`npm test`を必ず実行する。この入口だけで端末上の全ケースを実行したとは扱わず、既存の次の入口も使う。

```bash
npm test
npm run test:android --workspace muon-android-tester
npm run test:android:pixel6 --workspace muon-android-tester
```

端末は`ANDROID_SERIAL`で指定し、x86_64・16 KiBページとarm64実機の既存の検証を維持する。追加のOS境界試験はその環境に適したinstrumentationの実行を用意する。利用できない端末・権限・依存ツールによって必須ケースを実行できない場合は、未検証として記録し、完了とはしない。

今回の計画書保存は文書のみの変更であり、ビルドや製品テストの対象ではない。保存時には内容の照合、日本語の推敲、相対リンク、Markdownと差分を確認する。

## 全体の完了条件

1. 共通通知とAndroid専用機能の役割が分かれ、投稿・取り消し・イベント・設定起動の重複APIがない。
2. システムバー、ノッチ、安全領域、起動時表示を制御でき、既存fullscreenと状態を共有している。
3. 通知の権限、チャンネル、投稿・更新・取り消し、設定画面への導線が公開APIから利用できる。
4. 通知タップを、アプリの新規起動と実行中の両方で受け取り、未処理イベントと受領確認を扱える。
5. simple・validate、関数の許可、Activity再生成、ページ破棄、購読解放の動作を確認している。
6. 最低APIレベルを維持し、非対応機能を明示する。共通通知を未実装のデスクトップへ公開しない。
7. 公開CLIで生成した配布物、全体テスト、必要なOS境界と実機の試験が成功している。
8. 日本語の利用文書と、その内容に対応した英語版が更新されている。

## 計画書保存時の確認

案Bと案Cの組み合わせ、APIの役割、初期設定、通知の範囲、設定画面との連携、実装段階、検証方法と完了条件を記載した。先の検討で発展案とした予約通知・FCM・継続実行・逆方向の設定連携も、後続対象として残している。

実装段階と全体の完了条件は未達成である。本書の保存を、機能実装や端末検証の完了とは扱わない。
