# Androidの再起動後のサービス起動とビルド時の権限設定

## 目的と現在の動作

案Aでは通常のアプリ起動時とAndroid端末の再起動後に同じ自動起動設定を使う。端末を再起動して初めてロックを解除すると、アプリの画面を開かなくても、設定がONでAndroidの起動条件を満たすサービスが動くようにする。Androidビルドガイドには、muonがビルド時に追加する権限とアプリ開発者が指定する権限をまとめる。

計画開始時点（`cb06900c`）では、アプリの画面を開くとAndroidランタイムがサービス専用プロセス（`:muon_services`）に接続する。このプロセスはサービスごとの自動起動設定を読み、ONのものをフォアグラウンドサービスとして起動する。フォアグラウンドサービスは処理の実行中にAndroidへ通知を提示する。現在のマニフェストには画面とサービスの登録があり、端末の起動通知を受けるレシーバーは未実装である。[画面からの接続](../../../core/android/runtime/src/main/java/dev/muon/runtime/MuonAndroidPlatformService.java)・[サービスプロセス](../../../core/android/runtime/src/main/java/dev/muon/runtime/MuonServiceProcess.java)・[マニフェストのテンプレート](../../../ui/android/templates/AndroidManifest.xml.fc)

## 案Aの起動条件

Android向けのnativeまたはQuickJSサービスを一つでも含むAPKには、muonがビルド時に `android.permission.RECEIVE_BOOT_COMPLETED` を宣言し、システムの起動通知 `android.intent.action.BOOT_COMPLETED` を受けるレシーバーを一つ登録する。開発者は既存のサービス設定だけでこの機能を使える。全サービスの初期値がOFFでも、インストール後に利用者がONにできるため登録対象とする。サービスがないAPKにはmuonはこのレシーバーや受信用権限を自動追加しない。受信用権限はインストール時に付与される。[Androidの受信用権限](https://developer.android.com/reference/android/Manifest.permission#RECEIVE_BOOT_COMPLETED)

レシーバーは `android:exported="false"` とし、サービス専用プロセスで起動通知を受け取る。配信時にはロック解除後のユーザー領域にアクセスできるので、既存の設定ファイルをそのまま読める。`ACTION_USER_UNLOCKED` の配信先は実行中に登録したレシーバーに限られる。[起動通知の仕様](https://developer.android.com/reference/android/content/Intent#ACTION_BOOT_COMPLETED)・[ロック解除の通知](https://developer.android.com/privacy-and-security/direct-boot#notification)・[レシーバーの公開範囲](https://developer.android.com/guide/topics/manifest/receiver-element#exported)

端末起動時に通知を受けるには、インストール後に利用者が一度アプリを開いておく必要がある。Android 15以降では、強制停止を解除して利用者が再びアプリを開いたときにも `BOOT_COMPLETED` が届くことがある。この配信は画面からの起動と重なり得る。[初回起動の条件](https://developer.android.com/develop/background-work/services/alarms)・[停止状態を解除したときの配信](https://developer.android.com/reference/android/content/Intent#ACTION_BOOT_COMPLETED)

| サービスの設定・状態 | 起動通知を受けたときの動作 |
| --- | --- |
| 利用者の保存値がない | `muon.json` の `services.<id>.autostart` を使う。省略時はOFF。 |
| `muon.services.setAutostart(id, true)` または `false` の保存値がある | 保存値を優先する。初期値がOFFでも保存値がONなら起動対象とし、初期値がONでも保存値がOFFなら起動しない。 |
| ONで、Androidが起動を許可する | 画面を開かずにサービスを開始する。起動中・実行中に通知が再び届いたり画面が接続したりしても、同じサービスを二重に開始しない。 |
| ONで、サービスの種類や権限が起動条件を満たさない | ONを保持し、理由をサービスごとに記録する。他の対象サービスは起動する。次に画面を開いて条件が整えば、保留したサービスを開始する。 |

設定の読み込みと起動の判断を通常起動とブート受信で共有する。現行のライフサイクルは初期化時にONの全サービスの起動を要求するため、ブート受信時にはサービスごとの起動条件を適用できるようにする。レシーバーはページの参照を取得せず、既存のフォアグラウンド起動処理に引き渡す。設定の非同期読み込み中は `goAsync()` で受信処理を維持し、開始要求を渡した時点、または対象なし・失敗が判明した時点で `finish()` を呼ぶ。サービス本体の処理はその後も専用プロセスで続けられる。[設定の保存と読み込み](../../../core/android/runtime/src/main/java/dev/muon/runtime/MuonServicePreferences.java)・[ライフサイクル](../../../core/common/src/services/muon_service_lifecycle.cpp)・[Androidのブロードキャスト受信](https://developer.android.com/develop/background-work/background-tasks/broadcasts)

配布APKの `minSdk` は24、`targetSdk` は37である。このAPKでは、Android 14以降で `microphone`、Android 15以降で `camera`、`dataSync`、`mediaPlayback`、`mediaProjection`、`phoneCall` にブート受信からの起動制限がある。複数の種類を指定したサービスでは、指定したすべての種類について条件を確認する。実行時権限が必要な場合は利用者が画面で許可する機会を設け、ブート受信中は許可済みの範囲で起動する。[対象SDK](../../../core/android/toolchain.json)・[種類別のブート起動制限](https://developer.android.com/about/versions/15/changes/foreground-service-types)・[バックグラウンドからの起動条件](https://developer.android.com/develop/background-work/services/fgs/restrictions-bg-start)

## Androidビルドガイドに集約する内容

日本語版の「設定とMuon API」に「ビルド時に追加される権限と実行時の許可」を設け、設定表の `permissions` 行から参照できるようにする。各項目には設定例、自動追加の条件、開発者が指定する項目、端末上で利用者の許可が必要かを記す。通信・通知・サービスの各説明からも同じ節を参照できるようにし、日本語版を基に英語版へ反映する。[日本語版のAndroidビルドガイド](../android.md)・[英語版](../../en/android.md)・[サービスガイド](../services.md)

| アプリの構成 | muonが生成する宣言と開発者の設定 |
| --- | --- |
| アプリが明示した権限 | `android.permissions` に完全名を配列で指定する。既定は空。Viteで `build.android.permissions` を明示するとその配列が優先され、muonは機能に必要な自動追加分と合わせて重複を除く。 |
| Androidサービスを含む | 現行ビルダーは `FOREGROUND_SERVICE`、指定した種類に対応する `FOREGROUND_SERVICE_*`、`POST_NOTIFICATIONS` を追加する。`shortService` には種類固有の権限がない。開発者はサービスの `android.foregroundServiceType` と `android.notification` を指定し、`specialUse` には用途を示す `specialUseSubtype` も指定する。案Aの実装後は、この行にブート受信に必要な権限とレシーバーの自動登録を加える。 |
| ローカル通知を使う | `notifications.enabled: true` の場合も `POST_NOTIFICATIONS` を追加する。Android 13以降で通常の通知を表示するための許可は、通知機能を有効にしたアプリの画面から `muon.android.notifications.requestPermission()` で求める。フォアグラウンドサービスを開始するだけなら、この実行時許可は不要。サービスは開始時に通知を提示する。[Androidの通知権限](https://developer.android.com/develop/ui/compose/notifications/notification-permission) |
| ネットワークを使う | いずれかのサービスの `io.http.origins` に接続先が1件以上あるか、`io.server.listen` が指定されていれば `INTERNET` を自動追加する。WebViewだけで外部と通信する場合の例として `{"android":{"permissions":["android.permission.INTERNET"]}}` を載せる。`INTERNET` はインストール時に付与される。平文HTTPについては、サービスの接続先に明示した `http://` のホストに限ってネットワークセキュリティ設定で許可することも説明する。[Androidの通信権限](https://developer.android.com/develop/connectivity/network-ops/connecting)・[ネットワークセキュリティ設定](https://developer.android.com/privacy-and-security/security-config) |
| 機能固有の実行時権限が必要 | 例えば `location` では `FOREGROUND_SERVICE_LOCATION` をmuonが追加する。開発者は `ACCESS_COARSE_LOCATION` または `ACCESS_FINE_LOCATION` を宣言し、利用者の許可を得る。バックグラウンドから位置情報サービスを開始する場合には、端末のOSと用途に応じて `ACCESS_BACKGROUND_LOCATION` の許可も必要になる。現行のmuonには任意の権限の許可を求める汎用JS APIがなく、`android.permissions` に指定しても許可ダイアログは開かない。[位置情報サービスの要件](https://developer.android.com/develop/background-work/services/fgs/service-types#location) |

ガイドのアプリID `com.example.notes`、`versionCode` が1の場合は、次のコマンドでビルドし、最終APKに含まれる権限とレシーバーを確認できる。`INTERNET` の例では、権限一覧に `android.permission.INTERNET` があることを確かめる。設定が反映されない場合は読み込み順も確認する。設定ファイルの既定の探索順は `muon.json5`、`muon.jsonc`、`muon.json` で、最初に見つかったファイルを使う。`.muon/android/app/src/main/AndroidManifest.xml` はビルドのたびに生成されるため、権限を変更するときは読み込まれる設定ファイルかViteのビルド設定を編集する。[設定ファイルの選択](../../../ui/common/build-config.ts)・[権限の読み込み](../../../ui/android/target.ts)・[サービスの権限生成](../../../ui/android/services.ts)・[マニフェスト生成](../../../ui/android/build.ts)・[Android SDKのAPK解析ツール](https://developer.android.com/tools/apkanalyzer)

```sh
npx muon build --target android
apkanalyzer manifest permissions dist-muon/android/com.example.notes-1-debug.apk
apkanalyzer manifest print dist-muon/android/com.example.notes-1-debug.apk
```

## 実装段階と完了条件

各段階の成果を個別にコミットする。起動処理を変更する段階では、利用アプリで観測できる動作を先にテストにし、修正前の失敗と修正後の成功を同じテストで確認する。

### 段階1　現在のビルドで確認できる権限をガイドにまとめる

- [x] 日本語版と英語版に権限の節を追加した。ブート受信の実装前に3種類のdebug APKを生成し、`apkanalyzer manifest permissions` で照合した。WebView用に明示した場合は `INTERNET`、通知だけを有効にした場合は `POST_NOTIFICATIONS` を確認した。HTTPS接続先を指定した `dataSync` サービスには、この2権限に加えて `FOREGROUND_SERVICE` と `FOREGROUND_SERVICE_DATA_SYNC` が含まれた。

### 段階2　配布APKのサービスを画面なしで起動する

- [x] Android向け配布ビルドに受信権限とレシーバーを追加した。PIN付きのAndroid API 37エミュレーターで、修正前のAPKでは開始記録がなく、修正後は初回ロック解除でQuickJSサービスが開始することを確認した。UIプロセスは起動せず、あとから開いた画面から同じサービスが応答した。[配布APKの再起動試験](../../../ui/android-test/service-boot-e2e.test.ts)
- [x] 初期値がすべてOFFのAPKで、保存値をONにしたサービスだけが起動した。全件をOFFにし、一つの初期値をONに変えたAPKに更新して再起動すると、保存値のOFFが優先され、サービスは起動しなかった。サービスなしのAPKを2種類生成し、受信権限は明示した場合だけ含まれ、どちらにもレシーバーがないことを確認した。

### 段階3　起動制限と画面からの再接続を検証する

- [x] 同じAPKでnativeとQuickJSのサービスを起動し、`specialUse|dataSync` を指定したサービスは画面を開くまで保留した。位置情報サービスは許可がない間も保存値がONで、`getState()` に権限不足の理由が残った。位置情報を許可して画面に戻るとサービスが開始した。
- [x] 再起動後に起動通知を再送して画面から接続し、nativeとQuickJSの開始記録がそれぞれ1回であること、世代が変わらずQuickJSのメソッドが応答することを確認した。共通ライフサイクルでは、古い世代からの再試行と保存値OFFからの再起動を防ぐ試験を追加し、25件が通った。[配布APKの再起動試験](../../../ui/android-test/service-boot-e2e.test.ts)・[共通ライフサイクルの試験](../../../core/common-test/muon_service_lifecycle_test.cpp)

### 段階4　再起動と初回ロック解除を通して確認する

- [ ] ロックを設定した試験用エミュレーターに配布形式のAPKをインストールし、一度アプリを開いてサービスごとのONとOFFを保存する。端末を実際に再起動し、ロック中にサービスが起動していないことをOS側で確認する。最初のロック解除後、対象アプリの画面を開かずに、今回の起動を示すサービスの記録とAndroidの稼働状態を確認する。その後で画面を開き、起動済みのサービスには二度目の開始記録がないことを確かめる。
- [ ] 検証したブート受信の条件と制限を、両言語のAndroidビルドガイドとサービスガイドに反映する。リポジトリ直下で `npm test` を実行し、全体テストと再起動試験の結果を記録して各段階の完了条件と照合する。
