# サービスの起動・終了通知とQuickJSのnpm開発環境・共通I/O

## 目的と出発点

サービスの作者が起動理由に応じて初期化し、ユーザーセッションの終了を知ったときに状態を保存できるようにする。ファイルの読み書き、外部へのHTTP通信、ローカルのHTTP・WebSocketサーバーをサービスから使えるようにする。QuickJSを選んだ作者も、npmで依存関係を管理し、エディターでTypeScriptの型を参照しながら開発できることを目標とする。

2026年11月19日、plan18完了時の `871b58376` を起点とする。plan17でnativeとQuickJSのサービス、ページからの呼び出し、参照数に基づく起動・停止を実装した。将来目標だったPCでのNode.jsサービスはplan18で実現し、CEFの開発起動と配布ビルドでも3方式を選べる。plan19の実装対象はLinux・WindowsのCEF版ではnative・QuickJS・Node.js、Androidではnative・QuickJSとする。[サービスガイド](../services.md)

plan18は全5段階を完了し、全体テストも成功した。plan18でLinux armhf・arm64とAndroid arm64について確認した範囲は、ビルドと配布物の検査までである。plan17に挙がったサービス間の参照・依存関係の管理や、自動起動の設定を保ったままサービスを一時休止する `requestShutdown()` は、引き続き別の課題として残る。[plan18の完了判定](plan18.md#2026年11月19日-全体試験と完了判定)・[plan17の参照管理](plan17.md#参照による寿命の管理)・[一時休止の検討](plan17.md#検討した別案)

| 対象 | 計画開始時の実装と今回の変更点 |
| --- | --- |
| 起動と終了 | TSの `start(context)` は非同期の `stop(reason)` を持つインスタンスを返す。QuickJSとNode.jsのホストが渡す停止理由は固定文言で、nativeのC ABI v2の `stop` に理由の引数はない。サービスホストが把握した契機を3方式の作者に渡す。[TSランタイム](../../../sdk/src/service-runtime.ts)・[QuickJSホスト](../../../core/common/src/services/muon_quickjs_service_host.cpp)・[Node.jsホスト](../../../core/service-host/muon_node_service_host.cpp)・[C ABI](../../../sdk/include/muon_service_api.h) |
| QuickJSの開発 | 公開SDKにサービスとタイマーなどの型宣言がある。ビルダーはTSと依存モジュールをViteで単一のJSにまとめる。現行のQuickJS設定は `entry` を受け付け、`project` を指定できるのはNode.jsに限られる。QuickJSにもnpmプロジェクトを指定できるようにする。[SDKの公開先](../../../sdk/package.json)・[設定の読み込み](../../../ui/common/service-config.ts)・[QuickJSビルダー](../../../ui/common/service-build.ts) |
| ファイルと通信 | Node.jsのサンプルは `node:fs/promises` で同梱ファイルを読み、終了時に専用ディレクトリへ書き込む。nativeではOSやライブラリを直接呼べる。QuickJSで同様の処理をするには許可済みのnativeプラグインが必要になる。これらの用途をサービスSDKから利用できるようにする。[Node.jsのサンプル](../../../sdk/examples/services/node-files.ts)・[QuickJSの実行環境](../services.md#quickjsの実行環境) |
| ローカルサーバー | 現在の `muon.server` ではCEFのページがHTTP要求やWebSocket接続を処理し、ページを破棄すると待受も終了する。サービス自身が待ち受け、画面を開かずに応答できるAPIを追加する。[ローカルサーバーガイド](../local-server.md)・[ページとサービスが使うサーバー実装](../../../core/common/src/server/muon_server_service.cpp) |

## 起動・終了ハンドラー

PCのユーザーセッションは、ユーザーがデスクトップにログインしてからログアウト、またはOSの終了に至るまでを指す。その間に個々のサービスは何度でも起動・停止できる。サービスを実行するホストは、同じIDのサービスでも起動するたびに新しい実行世代を割り当てる。

ページが `acquire()` で取得した参照と、自動起動設定が有効な間ホストが保持する常駐参照のどちらかがあれば、その世代は通常の動作を続ける。起動理由はその世代が始まった時点で決める。実行中のサービスに画面が再接続しても `start` は呼び直さない。

既存の `start` に `context.startReason` を追加し、`stop` には理由と終了処理用のキャンセル通知をまとめた `info` を渡す。次の名前は公開APIの案である。作者は型で理由を判別できる。OSから詳細を得られた場合は補足情報も渡す。

| ホストが観測した契機 | ハンドラーの動作 |
| --- | --- |
| PCのログイン登録による起動 | `--muon-autostart` で起動し、保存済み設定により開始したサービスに `startReason = "login-autostart"` を渡す。UIの自動表示設定や、自動起動を登録する操作とは別に扱う。 |
| セッション中の起動 | ページの `acquire()`、通常のアプリ起動時に有効だった常駐設定、利用中に常駐設定を有効にした操作、開発中のサービス更新を区別する。Androidでアプリを開いて開始したときも、その実際の契機を渡す。 |
| セッション終了の確定 | 稼働中の各サービスの `stop(info)` に `info.reason = "session-ending"` を渡す。終了が中止されたときはサービスを継続する。 |
| アプリやサービスへの停止要求 | アプリ全体の明示終了、最後の参照の解放、開発中のサービス更新、開発コマンドの終了、OSが通知したサービスの実行期限、詳細が分からないホストへの終了要求を区別する。画面を閉じて最後の参照がなくなれば、その理由は参照の解放となる。 |

`stop` は起動に成功した世代ごとに1回だけ呼ぶ。起動中に終了要求が届いたら開始処理へキャンセルを伝え、インスタンスを返した場合は終了処理を行う。複数の終了通知が届いても、最初に受理した理由を保つ。Viteが画面のモジュールを更新するHMR、ページの再読み込み、ロック、サスペンドをセッション終了として扱わない。

停止を始めたら新しい呼び出しの受付を閉じ、実行中の呼び出しにキャンセルを通知する。開始時の `context.signal` はこの時点でキャンセルされるため、`stop` 内で状態を書き出すI/Oには `info.signal` を使えるようにする。ホストはOSとランタイムが許す期限までその完了を待つ。期限が分かる場合は残り時間も渡す。期限に達したら保留中のI/Oを打ち切り、ホストが所有する資源を回収する。[現行の停止処理](../../../sdk/src/service-runtime.ts)・[サービスの生存管理](../../../core/common/src/services/muon_service_lifecycle.h)

意図しないクラッシュ、`SIGKILL`、電源断など、サービス自身が検出できない終了はハンドラーの対象外とする。次回起動時に前回分の終了通知を作って届ける処理や、自動再起動も対象外とする。Androidの `lastHostExit` は引き続き過去の終了を照会するための情報である。[Androidでの動作条件](../services.md#androidでの動作条件)

### OSごとの通知

| 環境 | ホストが利用する通知と理由 |
| --- | --- |
| Windows | 既存のメッセージ受信を利用する。`WM_QUERYENDSESSION` は終了を許可し、`WM_ENDSESSION` の `wParam` が真のときに終了処理へ進む。`ENDSESSION_LOGOFF` はログオフ、フラグが0ならシャットダウンまたは再起動として伝える。`ENDSESSION_CLOSEAPP` のみの場合は保守などによるアプリの終了として扱う。[Windowsホスト](../../../core/service-host/muon_service_host_windows.cpp)・[Microsoftの終了通知仕様](https://learn.microsoft.com/en-us/windows/win32/shutdown/wm-endsession) |
| Linux | デスクトップが対応する場合はXDG Desktop Portalの `Inhibit.CreateMonitor` で自分のセッションを監視する。バージョン3以降の `session-state` が `Query End` なら1秒以内に `QueryEndResponse` で応答し、`Ending` で初めて終了を通知する。ログアウトを抑止しない。ポータルを利用できない環境でもホストの通常の停止処理は使える。`SIGTERM`・`SIGINT`・`SIGHUP` だけを受けた場合は、一般のホスト終了要求として扱う。[Linuxホスト](../../../core/service-host/muon_service_host_linux.cpp)・[XDG Desktop Portalの通知仕様](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Inhibit.html) |
| Android | 動作中のサービスホストが動的に登録した受信先で `ACTION_SHUTDOWN` を受け取った場合に終了を通知する。通知付きで実行するforeground serviceの `onTimeout` は、そのサービスの実行期限として伝える。Activityの終了は既存の参照管理に従う。OSがコールバックを送らない停止は対象外とする。[Androidサービス](../../../core/android/runtime/src/main/java/dev/muon/runtime/MuonNativeService.java)・[終了ブロードキャスト](https://developer.android.com/reference/android/content/Intent#ACTION_SHUTDOWN)・[実行期限](https://developer.android.com/develop/background-work/services/fgs/timeout)・[ユーザー操作による停止](https://developer.android.com/develop/background-work/services/fgs/handle-user-stopping) |

ログイン時の開始はLinux・Windowsの既存の登録経路を使う。Androidはアプリの正規の起動時に保存済み設定を評価する。端末のブート時にサービスを起動する登録は本計画の範囲外とする。[muonのランチャー](../../../core/cef/src/main.cpp)・[XDG自動起動仕様](https://specifications.freedesktop.org/autostart/latest/)・[Windowsのログイン時起動](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)

## QuickJSサービスをnpmプロジェクトとして開発する

アプリの `muon.json` から、サービスの `package.json` があるディレクトリを `project` で指定する。`project` は `muon.json` のあるディレクトリ、`entry` は `project` を基準に解決する。これで作者はNode.jsサービスと同じ構成でソース、依存関係、型検査をサービスごとに管理できる。小さなQuickJSサービスでは現行の `entry` 単独指定も使え、その場合の基準は `muon.json` のあるディレクトリとする。

```json
{
  "services": {
    "worker": {
      "runtime": "quickjs",
      "project": "./services/worker",
      "entry": "src/service.ts"
    }
  }
}
```

- `services/worker` に `package.json`、`package-lock.json`、`tsconfig.json`、`src/service.ts` を置く。作者はこのディレクトリで `npm install`、`npm run typecheck`（`tsc --noEmit`）、`npm test`（Vitest）を実行できる。アプリ側では同じ設定で `npx muon run`、`npx vite dev`、配布用の `npx muon build --target linux-amd64` などを利用する。Androidは生成したAPKで確認する。
- `muon-plugin-sdk/services` から `defineService` と公開型を取得する。型宣言には起動・終了理由、キャンセル通知、ファイル操作、HTTPの要求と応答、サーバーのハンドラー、バイナリー値、エラーの識別方法を含める。SDKに同梱するサンプルの `tsconfig.json` では `strict` を有効にし、`lib` に `ES2022`、`types` に `muon-plugin-sdk/service-globals` を指定する。作者はDOMや `@types/node` を追加せずにコード補完と型検査を利用できる。[現在のサービス型](../../../sdk/src/services.ts)・[QuickJSのグローバル型](../../../sdk/src/service-globals.ts)
- QuickJSで動くnpm依存はプロジェクトを基準に解決し、Viteがビルド時にサービスのJSへまとめる。配布先で実行するのはQuickJSで、サービスのためにNode.jsや `node_modules` を配置する必要はない。`node:` 組み込みやNode.js用ネイティブアドオンを要求する依存には、サービスと依存先を示すビルドエラーを返す。宣言済みの `muon:` プラグインは引き続きホストが提供する。[現在のバンドル処理](../../../ui/common/service-build.ts)
- 配布用のデータファイルはプロジェクトで明示し、サービスが論理名で読めるようにする。開発中はサービスのソース、取り込んだ依存、パッケージ定義・ロックファイル、宣言済みのデータの変更をViteが検出する。更新用のビルドが成功してからサービスホストを入れ替える。UIだけのHMRでは同じサービス世代を保つ。[サービス入力の収集](../../../ui/common/service-inputs.ts)・[開発時の監視](../../../ui/cef/development-service-watch.ts)

## サービスが使うI/O

下表の `context` は `start` に渡す引数である。QuickJSとNode.jsには同じTS型と非同期API、nativeには同じ操作を開始し完了通知を受け取れるC99 ABIを提供する。C++20では `cardio` の非同期処理から利用できるようにする。Node.js固有のコードでは引き続き `node:fs/promises` などを直接使える。[Node.jsのファイルAPI](https://nodejs.org/api/fs.html#promises-api)

| API案 | 用途と動作 |
| --- | --- |
| `context.fs` | サービス専用の永続領域（`dataDirectory`）を基準に、UTF-8テキストと `Uint8Array` の読み書き、ディレクトリの作成・一覧、名前の変更を提供する。状態を保存するときは同じ領域内でファイルを原子的に置き換え、書き込みが失敗しても以前の内容を読めるようにする。配布アセットは読み取り専用とする。追加のディレクトリはサービス設定で読み取り・書き込みを指定する。 |
| `context.http.request()` | 外向きのHTTP・HTTPS要求を送る。メソッド、URL、ヘッダー、本文を渡し、ステータス、ヘッダー、テキストまたはバイナリーの本文を受け取る。HTTPのエラーステータスと通信失敗を区別する。宛先の許可をリダイレクトごとに確認する。タイムアウト、キャンセル、同時要求数と本文サイズの上限を設ける。HTTPSはOSで信頼された証明書とホスト名を検証する。 |
| `context.server` | サービス自身が `127.0.0.1` でHTTP・WebSocketを待ち受け、画面が閉じていても要求に応答する。現行の `muon.server` と同様にHostとOriginを検証し、必要に応じてBearerトークンを指定できる。サービスが停止したら待受と接続を閉じ、同じポートを次の世代で再利用できるようにする。[現行サーバーの公開仕様](../local-server.md) |

`muon.json` のサービスIDごとにファイル領域、外向きの通信先、ローカルでの待受を個別に許可する。共通APIは指定した領域の外へ出るパスやリンク、その領域に許可していない書き込みを拒否する。画面側の権限は継承しない。この設定が制御するのはmuonが提供するAPIである。nativeコードやNode.jsの標準ライブラリを直接呼ぶ処理には、それぞれのプロセスのOS権限が適用される。

I/O、待受、接続はサービスIDと実行世代に属する。起動時に使うI/O、個別の要求のI/O、終了処理のI/Oにそれぞれ対応するキャンセル通知を渡せるようにする。キャンセルと通常完了が重なっても結果は一度だけ返し、古い世代の完了通知は新しい世代へ渡さない。I/Oや待受自体がサービスの参照数を増やすことはない。

ホスト側のファイル・ネットワーク操作には `cardio` を使い、あるサービスがI/Oを待つ間も同じホストで別のサービスを実行できるようにする。C ABIには構造体のバージョンとサイズ、バッファーの所有者と有効期間、完了通知を届けるスレッドを明記する。QuickJSではホストから完了通知を受け取り、サービスのイベントループでPromiseを完了させる。Node.jsではファイル操作などを標準ライブラリで実装する。共通APIを通じて行う操作には他の方式と同じ権限、キャンセル、資源の解放条件を適用する。

サーバーのHTTP・WebSocket処理と待受をCEFへの依存から分け、ページ用とサービス用の両方から使う。サービスのハンドラーにはHTTP要求やWebSocketのメッセージをSDKの型で渡す。ページが作ったサーバーはページを閉じた時点で終了し、常駐サービスが作ったサーバーはそのサービスの停止時に終了する。Android版でもサービスホストから待ち受けられるようにする。通信に必要な `INTERNET` 権限とAndroidの平文通信・証明書の設定を適用する。[ページとサービスが使うサーバー実装](../../../core/common/src/server/muon_server_service.cpp)・[Androidの通信権限](https://developer.android.com/develop/connectivity/network-ops/connecting)・[Androidのネットワークセキュリティ設定](https://developer.android.com/privacy-and-security/security-config)

Androidの試作版にはQuickJS用のファイル、HTTP、ソケットの実装例がある。入出力や試験例を参考にし、同期ファイル操作をJSの `async` 関数で包む実装は製品の非同期I/Oへ置き換える。独自プロトコル用のTCP接続は将来の拡張候補とし、plan19のネットワークI/OはHTTP・HTTPSクライアントとローカルのHTTP・WebSocketサーバーまでとする。[試作版の対応範囲](../../../core/android-poc/README.md)・[試作版のJS](../../../core/android-poc/android/app/src/main/assets/muon-javascript/runtime.js)・[試作版のファイル処理](../../../core/android-poc/android/app/src/main/cpp/muon_javascript_runtime_jni.cpp)

## 実装段階と完了条件

各段階で利用側のテストを先に追加して未対応の動作を確認し、実装後に同じテストと実行可能なサンプルで判定する。チェック欄は各段階の成果物が動作した時点で更新する。

### 段階1　型検査して起動できるQuickJSのnpmプロジェクト

`project` と `entry` をビルダーに接続し、型宣言を含むSDKと、QuickJSで動くnpm依存を一つ使ったechoサービスを用意する。新しいホストAPIの実装に先立ち、既存の `defineService` と呼び出しでプロジェクト構成を検証する。

- [x] リポジトリ外の利用アプリに配布形式のSDKとビルダーをインストールし、型検査、Vitest、開発起動、配布ビルドを実行できる。誤った引数は型検査が検出し、Node.js専用の依存はビルドが理由を示す。QuickJSで依存を使った呼び出しの結果が返り、Vite開発中に依存を変更すると結果も変わる。

Linux amd64で配布形式のSDKとビルダーをリポジトリ外にインストールし、型検査とVitestを実行した。型検査はバイナリー引数に文字列を渡した箇所を検出した。

`muon run`、`vite dev`、配布ビルドを実行し、配布アプリを別の場所に移してソースを削除した後も、QuickJSがnpm依存の `base64-js` を使って入力を変換できた。Viteの更新試験では、サービスに取り込んだnpmパッケージの変更が応答に反映された。UIだけを変更したときは元のサービスが応答した。[サンプル](../../../sdk/examples/services/quickjs-project)・[配布アプリの試験](../../../core/cef-test/service-consumer.test.ts)・[Viteの更新試験](../../../ui/cef-test/dev-service-updates.test.ts)

### 段階2　起動・終了の契機をサービスで観測する

C ABIのバージョンを更新し、ホストが把握した理由をサービスの起動・停止を管理する既存のコードから3方式へ伝える。起動理由・停止理由・世代を記録する小さなサービスを動かす。実行期限のある場合は期限内に終了処理を待ち、終了と取り消しをOSごとの入力で検証する。

- [x] PCでログイン登録からの起動と手動起動を識別できる。Windowsの終了照会と中止、Linuxの `Query End` と通常シグナル、Androidの `ACTION_SHUTDOWN` と `onTimeout` がそれぞれ表のとおりになる。通知の重複、起動と停止の競合、画面を閉じて再接続する場合を含め、サービスが受け取った理由と回数を確認できる。

Linuxの実ホストでは、通常起動とログイン登録の起動引数を使い分け、常駐サービスが受け取った起動理由を確認した。ページからの再接続では世代が変わらず、最後の参照を解放したサービスには参照解放の理由が届いた。実プロセスに送ったSIGTERMはホスト終了として届いた。テスト用のXDGポータルでは、終了照会への応答、取り消し後の継続、終了確定を順に確認した。[Linuxホスト試験](../../../core/cef-test/service-host.test.ts)・[ポータル通知試験](../../../core/service-host/session_monitor_test.cpp)

Windows 11 VMではamd64版とi686版のホストに終了メッセージを送り、照会、取り消し、ログオフ確定を確認した。Android API 37のx86_64エミュレーターでは、OSが実際に発行したforeground serviceの期限通知と、対象アプリだけに送った終了ブロードキャストを区別して記録した。PCのログインとログオフ、Android端末の電源断は試験のために実行していない。[Windows試験](../../../core/service-host/windows-driver.ts)・[Android試験](../../../core/android-test/observer/src/androidTest/java/dev/muon/e2e/observer/ServiceConstraintsTest.java)

C ABI v3とTS SDKの試験では、起動中のキャンセルと再取得、重複する停止要求、後処理に渡す独立したシグナルを確認した。QuickJSとNode.jsでも、サービスが世代と理由を記録して終了した。[SDK試験](../../../sdk/test/service-runtime.test.ts)・[QuickJS試験](../../../core/common-test/muon_quickjs_service_sdk_test.cpp)・[Node.js試験](../../../core/service-host/node-driver.ts)

### 段階3　終了時に保存したファイルを次回起動で読む

`context.fs` と対応するC APIを追加する。サンプルは同梱の初期値を読み、サービス停止時に最後の値を専用領域へ保存する。共通APIだけを使うTSソースをQuickJSとNode.jsで動かし、native版も同じ入出力で試す。

- [x] 配布アプリで値を変更して正常終了し、次回起動時に同じ値を読める。テキストとバイナリーの往復、宣言済みアセットの読み取り、領域外へのアクセス拒否を確認する。ファイルの置き換え前にキャンセルやエラーが起きれば以前の値が残る。起動時の `signal` がキャンセルされた後でも `stop` は期限内に保存できる。

Linux amd64では、利用アプリを別のディレクトリへ移してソースを削除した後、native・QuickJS・Node.jsの3方式で同梱の初期値を読み込んだ。各サービスで値を変更してアプリを終了し、次の起動で保存したバイナリーが戻ることを確認した。Android API 37のx86_64エミュレーターでは、生成APKに同梱したnativeとQuickJSの両方で、参照を解放してから再取得し、次の世代で同じ値を読めた。[配布アプリの試験](../../../core/cef-test/service-consumer.test.ts)・[Androidの試験](../../../core/android-test/service-rpc.test.ts)

Linuxのファイル試験では、UTF-8テキストとバイナリーの読み書き、許可したディレクトリの操作、領域外へのアクセスの拒否を確認した。原子的に置き換える前に書き込みをキャンセルした場合は以前の値が残った。[C ABI側のファイル試験](../../../core/common-test/muon_service_files_test.cpp)・[Node.jsのファイル試験](../../../node/test/service-files.test.ts)

### 段階4　サービスからHTTP・HTTPSで通信する

要求を受け取ったあと、テスト側からの指示で応答を返すサーバーを用意する。サービスからそのサーバーへ接続し、送受信、キャンセル、停止の順序を検証する。

- [x] 対応する各方式でステータス、ヘッダー、本文を取得できる。証明書の検証失敗、許可外へのリダイレクト、サイズ超過、応答待ちのキャンセルを識別できる。通信待ちの間も別サービスを呼び出せ、停止後に届いた応答が新しい世代に渡らない。

Linux amd64とWindows 11 VM（amd64・i686）ではnative・QuickJS・Node.jsを、Android API 37のx86_64エミュレーターではnative・QuickJSを使って試験した。バイナリーを送受信し、HTTP 418のステータスとヘッダー、UTF-8の本文を読めた。未信頼の証明書、許可外へのリダイレクト、1 MiBを超える応答、時間切れは、それぞれ異なるエラーになった。[デスクトップの通信試験](../../../core/service-host/http-driver.ts)・[Androidの通信試験](../../../core/android-test/service-rpc.test.ts)

テストサーバーが応答を保留している間も別サービスを呼び出せた。キャンセルするとサーバー側でも切断を観測した。PCでは保留中のサービスを停止して再取得し、古い接続に応答を書いた後も次の世代が自分の要求に応答した。

### 段階5　画面を閉じても応答するローカルサーバー

`context.server` と対応するC APIを追加し、HTTP要求とWebSocketメッセージに応答する常駐サービスをPCとAndroidで動かす。ホスト外のクライアントからサービスへ接続して検証する。

- [x] UIを閉じた後も常駐サービスが同じ世代で応答する。最後の参照を解放すると待受と既存接続が閉じ、次の世代が同じポートを取得できる。その間も別サービスが所有する待受は応答を続ける。開発中のホスト更新でも古い応答が混入しない。Host、Origin、Bearerの検証と、ページ所有の `muon.server` がページ破棄で終了する動作も確認する。

Linux amd64とWindows 11 VMのamd64・i686では、native・QuickJS・Node.jsの各サービスが、画面との接続を閉じた後も同じ世代でHTTPとWebSocketに応答した。最後の参照を解放すると外部クライアントの接続が閉じ、同じポートを次の世代で再利用できた。別のサービスの待受はその間も応答した。Host、Origin、Bearerの検査と、テキストおよびバイナリーのエコーも確認した。[外部クライアントによる試験](../../../core/service-host/server-driver.ts)

Android API 37のx86_64エミュレーターでは、生成APKで唯一のActivityを終了した後、テスト用のPCからnative・QuickJSの常駐サービスへ接続した。画面を再度開き、サービスの世代と待受の解放を確認した。LinuxのCEFではページ所有のサーバーも試し、ページを破棄すると接続も待受も終了した。[Androidの試験](../../../core/android-test/service-rpc.test.ts)・[ページ所有のサーバー試験](../../../core/cef-test/server.test.ts)

LinuxのVite開発起動では、画面の外からQuickJSサービスにWebSocketで接続し、応答を受けたままサービスの入力を更新した。旧ホストとの接続が切れた後に、3方式の新しい待受でバイナリーと実行世代を確認した。[開発中の入れ替え試験](../../../core/cef-test/service-consumer-development.ts)

### 段階6　公開パッケージを使った開発と配布の確認

利用者向けのサービスガイドに、QuickJSプロジェクトの作成、型の読み込み、各APIの許可、起動・終了理由の扱いを記載する。公開する構成のパッケージをリポジトリ外にインストールし、ソースツリーへの参照を持たない利用アプリで試す。SDKの型宣言だけで全APIを使うQuickJSサンプルが `tsc --noEmit` を通り、誤った停止理由の値、ファイルの引数、HTTP・WebSocketハンドラーの戻り値を型検査が検出することを確かめる。

- [ ] Linux・WindowsのCEF版でnative・QuickJS・Node.jsを開発起動と配布アプリから実行し、Androidではnative・QuickJSを生成APKで実行する。配布先を移してもアセットを読み、保存・通信・待受が動作する。OSから実際に受け取った通知、テストで入力した通知、ビルドだけを確認できた環境を記録する。各段階の条件と照合し、リポジトリ全体の `npm test` が成功した時点でplan19を完了とする。

SDKをnpmパッケージにしてリポジトリ外の利用アプリに入れ、QuickJS用のecho・状態保存・HTTPクライアント・ローカルサーバーを型検査した。DOMとNode.jsの型を読み込まずに4つとも通り、Vitestも成功した。停止理由、ファイルに書くバイナリー、HTTPとWebSocketのハンドラーの戻り値、HTTP応答の形式をそれぞれ誤った値にすると、型検査は5か所すべてを検出した。[日本語ガイド](../services.md)・[英語ガイド](../../en/services.md)
