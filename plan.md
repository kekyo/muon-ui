## android移植

muonをandroidに対応させることを検討します。
  muonはCEFをバックエンドに使用しているため:
  - CEFのandroid実装を使う/移植する
  - CEFではなくandroid WebViewに接続する
  あるいはその他の方法が考えられます。
  これらについて考察して、いくつか案を示して下さい

---

## 結論

2026年8月時点では、第一候補は「Android WebViewを第2のバックエンドとして追加する」案です。

ただし、単純にCEFをWebViewへ置換するのではなく、

- JavaScript APIとネイティブ実装間のRPC
- プラグイン実行
- Capability判定
- ブラウザ・OS固有操作

をCEFから分離したうえで、デスクトップ用CEFアダプターとAndroid用WebViewアダプターを実装する形が適切です。

一方、「現行と完全に同じネットワーク制御」を必須にする場合、WebViewは不足する可能性があります。この要件をAndroid対応の採否ゲートにすべきです。

## 現在のmuonの構造

現在はCEFが単なる描画部品ではなく、アプリケーション基盤全体に入り込んでいます。

- 起動、サブプロセス、イベントループがCEF依存です。[main.cpp](/home/kouji/Projects/muon-ui/muon-core/src/main.cpp:77)
- アセット配信、プラグイン起動、ブラウザー生成が同じCEF初期化処理内にあります。[muon_app.cpp](/home/kouji/Projects/muon-ui/muon-core/src/app/muon_app.cpp:871)
- プラグインランタイム自体が`CefFrame`や`CefProcessMessage`を公開型に含んでいます。[muon_plugin_runtime.h](/home/kouji/Projects/muon-ui/muon-core/src/plugins/muon_plugin_runtime.h:14)
- ビルドも常にCEFを要求します。[CMakeLists.txt](/home/kouji/Projects/muon-ui/muon-core/CMakeLists.txt:59)
- 配布ターゲットはLinux/WindowsとCEFターゲットが1対1に結び付いています。[targets.ts](/home/kouji/Projects/muon-ui/muon-ui/src/targets.ts:7)

そのため、Android対応は「別のブラウザーコントロールを接続する」よりも「CEF依存部分とmuon本体の境界を作る」作業になります。

一方、Viteが生成する公開APIは最終的に`globalThis.__muon_plugin_call`へ集約されています。[capability.ts](/home/kouji/Projects/muon-ui/muon-ui/src/capability.ts:1339)
ここはAndroid用の別トランスポートを差し込める、かなり有効な既存の境界です。

## 案の比較

| 案 | 初期工数 | Chromium互換性 | エンジン固定 | 現行ネットワーク制御 | 維持費 | 評価 |
|---|---:|---:|---:|---:|---:|---|
| Android WebView | 中 | 高い | できない | 完全再現は困難 | 低〜中 | 第一候補 |
| GeckoView | 中〜大 | 異なる | できる | WebExtension等で設計しやすい | 中 | 有力な代替案 |
| CEF/ChromiumのAndroid移植 | 極大 | 高い | できる | 最大限制御可能 | 極大 | 通常は非推奨 |
| PWA/TWA版muon | 小〜中 | Chrome依存 | できない | ネイティブ機能を大幅制限 | 低 | muon-lite向け |

## 案1: Android WebViewバックエンド

最も現実的です。

構成は次のようになります。

```text
Vite仮想モジュール / muon公開API
                 │
          共通RPCプロトコル
      call / result / event / cancel
          ┌──────┴────────┐
          │               │
   CEF IPCアダプター   WebMessageアダプター
     デスクトップ          Android
          │               │
          └──────┬────────┘
                 │
    Capability・プラグイン共通ランタイム
                 │
       Desktopサービス / Androidサービス
```

Android側では次を使えます。

- アプリ内アセットは`WebViewAssetLoader`で`https://appassets.androidplatform.net/`などから配信する。`file://`を避け、Same-Origin Policyを維持できます。[WebViewAssetLoader公式資料](https://developer.android.com/reference/androidx/webkit/WebViewAssetLoader)
- JavaScriptブリッジは`WebViewCompat.addWebMessageListener`を使用する。
- 許可オリジンを厳密に指定し、`sourceOrigin`と`isMainFrame`も確認する。
- `addJavascriptInterface`は全フレームに公開され、呼び出し元オリジンを安全に特定できないため使用しない。[AndroidのJavaScriptブリッジ公式資料](https://developer.android.com/develop/ui/views/layout/webapps/native-api-access-jsbridge)
- C/C++側の共通ランタイムとはJNIで接続する。
- ネイティブプラグインはABI別にビルドし、APK/AABへビルド時に同梱する。[Android NDKのABI公式資料](https://developer.android.com/ndk/guides/abis)

注意点として、WebViewの実装・バージョンは端末上のプロバイダーに依存します。Android 7以降は複数パッケージから選択される場合もあります。[WebViewバージョン管理](https://developer.android.com/develop/ui/views/layout/webapps/managing-webview)

### 最大の問題: ネットワーク制御

現在のmuonは、

- URL全体に対するglob判定
- トップレベル遷移かどうか
- リクエスト発生元オリジン

を使って許可判定しています。[muon_network_policy.cpp](/home/kouji/Projects/muon-ui/muon-core/src/network/muon_network_policy.cpp:48)

WebViewの`shouldInterceptRequest`だけでは同等になりません。公式仕様上、

- リダイレクト後のURLでは呼び出されない
- `javascript:`と`blob:`は対象外
- `shouldOverrideUrlLoading`はPOSTなど全遷移を捕捉しない

という制約があります。[WebViewClient公式資料](https://developer.android.com/reference/android/webkit/WebViewClient)

WebView案では、次のいずれかを仕様として選ぶ必要があります。

1. Androidではオリジン単位のallowlistへ簡略化する。
2. WebViewからの外部ネットワークを禁止し、許可されたネイティブHTTPプラグイン経由に限定する。
3. `WebViewClient`、Service Worker用ハンドラー、ローカルプロキシを組み合わせて近似する。ただし現行との完全同等は保証しない。
4. 完全同等が必須ならWebView案を採用しない。

私は2が最も安全で明瞭、1が最も使いやすいと考えます。

## 案2: GeckoViewバックエンド

GeckoViewはアプリにエンジンを同梱できるため、テストしたバージョンと実際に動くバージョンを一致させられます。[GeckoView公式サイト](https://mozilla.github.io/geckoview/)

組み込みWebExtensionとNative MessagingによってJavaScriptブリッジを構築でき、WebExtension側でネットワーク要求を扱う設計も可能です。[GeckoViewのWebコンテンツ連携](https://firefox-source-docs.mozilla.org/mobile/android/geckoview/consumer/web-extensions.html)

利点は次の通りです。

- Webエンジンのバージョンを固定できる。
- ブラウザー用途を意識したAPIがWebViewより豊富。
- 組み込み拡張によるブリッジ、要求制御、スクリプト注入を設計しやすい。

欠点は次の通りです。

- CEFと異なるGeckoエンジンなので、Webアプリの挙動差が生じる。
- APKが大きくなる。
- CEFコードはほとんど再利用できず、別のバックエンド実装が必要。
- Chromium固有機能との互換性は失われる。

「Chromium互換性よりも、実行エンジンの再現性と制御性を重視する」場合の有力候補です。

## 案3: CEFまたはChromiumをAndroidへ移植する

現在のCEF公式バイナリはWindows、macOS、Linux向けで、Android向け配布はありません。[CEF公式ドキュメント](https://chromiumembedded.github.io/cef/general_usage.html)
現在の公式ビルド設定もLinux、Windows、macOSのみをサポートし、それ以外を`Unsupported platform`としています。[CEFのgn_args.py](https://github.com/chromiumembedded/cef/blob/master/tools/gn_args.py)

したがって、「CEFのAndroid実装を使う」は現状選べず、実質的には新規移植になります。

必要になるのはCEFの薄いAndroid対応だけではありません。

- AndroidのActivity/Viewライフサイクル
- Chromiumマルチプロセス起動
- サンドボックス
- GPU描画
- タッチ、IME、アクセシビリティ
- 権限要求
- APK/AABパッケージング
- Chromium更新への継続的追従

直接Chromiumの`android_webview`をアプリへ組み込む案もありますが、Chromium自身が「個別APKで利用するためのものではない」と明記しています。[Chromiumソース構造の公式説明](https://www.chromium.org/developers/how-tos/getting-around-the-chrome-source-code/)

これはmuonのAndroid対応というより、独自ブラウザーエンジン配布プロジェクトです。専任チームを置き、継続的にChromiumへ追従すること自体を目的にできる場合に限る案です。

## 案4: PWA/TWAによるmuon-lite

Android版ではネイティブプラグイン互換を求めず、

- Web UI
- Service Worker
- Custom TabsまたはTWA
- 最小限のAndroid連携

だけを提供する案です。

開発・保守コストは低い一方、ファイルシステム、任意ネイティブプラグイン、ウィンドウ操作など、muonの主要機能は提供できません。「既存muonアプリをそのままAndroidへ」という目的には合いませんが、モバイル向け閲覧クライアントとしては成立します。

## 推奨する進め方

最初からAndroid全機能を実装せず、以下を順に検証するのがよいです。

1. CEF非依存RPCを抽出する
   完了条件は、デスクトップCEF版の挙動と全テストが変更前と同じであることです。

2. WebViewによる最小試作を作る
   アセット表示、`getConfigValues`相当のPromise往復、エラー、キャンセル、バイナリ転送、Activity再生成を通すことを完了条件とします。

3. ネットワーク制御を検証する
   メインフレーム、iframe、fetch、XHR、WebSocket、リダイレクト、Service Worker、`blob:`、ローカルネットワークをdeny-by-defaultで検証します。現行契約を満たせなければ、Android仕様を変更するかGeckoViewへ進みます。

4. Android API対応表を定義する
   reload、fullscreen、zoomなどは実装候補です。一方、minimize、maximize、title bar、window bounds、system tray、launcher updater、Node sidecar、executorはAndroidでは非対応または別APIにすべきです。[ブラウザー組み込み機能一覧](/home/kouji/Projects/muon-ui/muon-core/src/browser/muon_builtin_browser.h:17)

5. ネイティブプラグインをNDK対応する
   まず`arm64-v8a`とエミュレーター用`x86_64`を対象にし、プラグインはビルド時同梱とします。cardioはAndroidメインスレッドの既存Looperへ自動接続し、tra-fficはAndroid向けlibffiと静的リンクします。詳細な事前検証、upstreamとの分担、完了条件は次節のとおりです。

総合すると、「WebViewを採用し、RPCとプラグインランタイムをCEFから分離する」が推奨案です。ただし現行ネットワークポリシーの完全互換を必須とするなら、先にセキュリティ試作を行い、不成立ならGeckoViewを選ぶのが妥当です。CEFのAndroid移植は初手には勧められません。

### ステップ5: ネイティブプラグインの事前検証

2026年8月19日に、現在の`cardio 0.2.0`、`tra-ffic 0.3.0`、およびtra-fficが使用する`libffi 3.4.6`を対象としてAndroid試験を行いました。submodule自体は変更せず、Android用ホストを追加したcardioの一時作業コピーと、Android instrumentation testへ一時的に組み込んだtra-fficの既存全回帰テストを使用しました。試験用変更と一時ビルド成果物は製品コードへ残していません。

#### 検証環境と範囲

- 実行環境はPixel 6 AVD、Android 17/API 37、`x86_64`、16 KiBページです。アプリは`targetSdk 37`、`minSdk 24`、NDK `29.0.14206865`でビルドしました。
- `x86_64`ではinstrumentation test全20件を実行し、既存18件と今回の互換性試験2件がすべてPASSしました。
- `arm64-v8a`では同じNDK/API 24設定でlibffi 3.4.6をビルドし、cardio試作、tra-ffic全回帰テスト、libffiを含む`libmuon_android_rpc.so`をAPKへ同梱できるところまで確認しました。生成ELFのLOADセグメントの`p_align`は`0x4000`でした。ただしarm64 VMまたは実機がないため、arm64でのclosure実行は未確認です。
- libffi 3.4.6の公式リリースアーカイブを使用し、確認したSHA-256は`b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e`です。

#### cardioのAndroidメインLooper統合

実現可能です。Androidのメインスレッドで`ALooper_forThread()`を取得し、cardioを動かすためのfdを`ALooper_addFd()`のコールバックとして登録すれば、`dispatcher_host_glib_auto`や`dispatcher_host_win32_auto`と同じく、cardio側でネストした`park()`ループを所有せずにシステムのメッセージループへ参加できます。[Android NDK Looper API](https://developer.android.com/ndk/reference/group/looper)

AOSPではJava `MessageQueue`もスレッドローカルなネイティブ`Looper::getForThread()`を使用し、NDKの`ALooper_forThread()`も同じ関数を返しています。このため、Activityのメインスレッド上で取得した`ALooper`はJava UIメッセージを処理するLooperと同じです。[MessageQueue JNI実装](https://android.googlesource.com/platform/frameworks/base/+/HEAD/core/jni/android_os_MessageQueue.cpp) [NDK ALooperラッパー実装](https://android.googlesource.com/platform/frameworks/base/+/4fe6c3e51be77e35f40872cdbca6c80f8f8b7ecb/native/android/looper.cpp)

試作ホストは次の構成で動作しました。

- cardioのキュー投入と別スレッドからの投稿は`eventfd`でLooperを起こす。
- cardioの次回期限は`timerfd`へ設定する。
- `from_fd()`が待機するfdを`ALooper_addFd()`へ動的に登録し、待機終了後に解除する。
- Looperコールバック1回につきcardioのwork itemを1件実行し、Java UIメッセージを長時間飢餓状態にしない。
- Looperが返した`revents`をcardioの集約wait snapshotだけでなく、内部のPOSIX fd snapshotへ転記してからready continuationを回収する。この転記を欠いた最初の試作では、即時処理、別スレッド投稿、タイマーまでは動いたものの`from_fd()`が再開せず、転記追加後にGREENになりました。
- ホストの生成と破棄は同じLooperスレッドで行い、破棄時はfdをLooperから外してからcloseする。`ALooper_removeFd()`のAPIコメントにある、既に配送中のコールバックとの競合もライフサイクル設計で排除する。

VMでは、Javaの`Handler`メッセージがcardio処理中にも配送され、即時continuation、50 msタイマー、pipe fd readiness、別スレッドからのpostがすべてAndroidメインスレッドと同じスレッドで実行されました。したがって、muon-coreがメインスレッドdispatcherを前提とする構成を維持できます。

現在のcardio公開APIだけで同等ホストをmuon側に実装することはできません。wait snapshotの作成・回収、キュー取得、timer deadlineなどがprivateで、既存のauto hostだけがfriendになっているためです。upstreamのcardioに`dispatcher_host_android_auto`を追加するか、同等機能を安全に実装できる公開host adapter境界を追加する必要があります。muon側でprivate実装を複製する案は採用しません。

#### tra-fficとlibffi closure

検証したAndroidアプリプロセスでは動的トランポリンの生成と実行が許可されました。

- `ffi_closure_alloc()`が成功した。
- `ffi_prep_closure_loc()`が`FFI_OK`を返した。
- 生成コードを`int32_t(int32_t)`として呼び出し、入力41から結果42を得た。
- 実行アドレスを`/proc/self/maps`で確認すると、パスを持たない匿名`rwxp`マッピングだった。
- tra-fficの既存回帰テストをAndroidアプリ内でそのまま実行し、終了コード0を得た。inline/threadの両drain modeについて、primitive、structured scalar、retval ABI、buffer view、async completion、pointer-list、function marshalling/identity、3段function signature、lifetime、closure allocation/free balanceを含む全ケースがPASSした。

libffiの公式文書どおり、closureは実行時に小さな関数を組み立て、`ffi_closure_alloc()`が書き込み用アドレスと対応する実行用アドレスを返します。[libffi 3.4.6 Closure API](https://github.com/libffi/libffi/blob/v3.4.6/doc/libffi.texi) Android向けconfigureでは`FFI_MMAP_EXEC_WRIT`が有効になり、3.4.6の実装は可能なら匿名の実行可能マッピングを使い、拒否された場合は書き込み用と実行用の二重マッピングを持つ一時ファイル方式へフォールバックします。[libffi 3.4.6 configure](https://github.com/libffi/libffi/blob/v3.4.6/configure.ac) [libffi 3.4.6 closure allocator](https://github.com/libffi/libffi/blob/v3.4.6/src/closures.c)

AndroidはアプリdomainのJIT用途に`execmem`を許可しており、NDKの共有メモリAPIも`PROT_EXEC`マッピングを仕様に含めています。このため、アプリ内の動的コード生成が一律禁止されているわけではありません。[AOSP appdomain policy](https://android.googlesource.com/platform/system/sepolicy/+/main/private/app.te) [Android NDK Memory API](https://developer.android.com/ndk/reference/group/memory)

一方、Android 10以降は書き込み可能なアプリhome内のファイルを実行することをW^X違反として制限します。[Android 10 behavior changes](https://developer.android.com/about/versions/10/behavior-changes-10) 今回は匿名マッピングが成功したためこの制限に触れませんでしたが、端末ポリシーが匿名RWXを拒否した場合、libffi 3.4.6の一時実行ファイルfallbackがアプリsandbox内で成功するとは限りません。また、`minSdk 24`向けconfigureでは`memfd_create`を利用できませんでした。従って、1台のAVDでの成功を全OEM・全セキュリティ構成の保証とはしません。

tra-ffic自身にはAndroid固有の機能不良を再現できませんでした。`TRA_FFIC_IN_POSIX`、pthread、libffi ABIはいずれもx86_64で正常でした。ただし、Android用の公式ビルド・実行テストがupstreamにないことと、arm64実行を未確認であることは課題です。またlibffi 3.4.7にはARM64のBTI修正とpointer authentication対応が含まれるため、3.4.6を製品版の固定値とする前に新しいlibffiでもtra-ffic全テストを実行して採用版を決めます。[libffi 3.4.7 release notes](https://github.com/libffi/libffi/releases/tag/v3.4.7)

#### upstreamとmuonの分担

| 対象 | 判定 | 対応先 |
|---|---|---|
| Androidの既存Looperへ接続するauto host | 現在のcardioに存在せず、private状態へアクセスする必要がある | cardio upstreamへ`dispatcher_host_android_auto`とAndroid回帰テストを追加する |
| Looper eventからcardio内部snapshotへの`revents`転記 | 試作で実際に欠落を再現したAndroid host固有の必須処理 | cardio upstreamのAndroid host実装とテストで保証する |
| tra-fficのPOSIX/libffi動作 | x86_64 Android VMの既存全回帰テストで問題なし | tra-ffic本体の修正は現時点で不要 |
| Android NDKの継続試験とarm64 hardening | upstreamにAndroid test targetがなく、arm64実行は未確認 | tra-ffic upstreamでAndroid build/test targetとlibffi対応版の範囲を追加する候補 |
| libffiの版、取得、ハッシュ、ABI別静的ビルド | muonの配布物と再現可能ビルドに属する | muonのAndroidビルドで管理する。ただしlibffi自体のallocator修正が必要ならlibffi upstreamで扱う |
| APK/AABへのプラグイン同梱、JNI境界、Activityライフサイクル | muon固有 | muon側で実装する |

#### ステップ5の完了条件

1. cardio upstream版のAndroid auto hostを使用し、muon側にcardio private実装のコピーや独自ポーリングがない。
2. Androidメインスレッド上で、Javaメッセージとcardioの即時、timer、fd、別スレッドpostが共存し、Activityの生成・破棄を繰り返してもcallbackやfdが残らない。
3. tra-fficの全回帰テストと直接closure呼び出しが、`x86_64` AVDと16 KiBページ対応の`arm64-v8a`実機またはVMの両方でPASSする。
4. 採用するlibffi版、公式取得元、SHA-256、NDK API level、コンパイルフラグを固定し、ABI別に再現可能な静的ビルドを行う。匿名RWXを許容するか、W^Xを保つallocatorを要求するかもセキュリティ方針として決定する。
5. `arm64-v8a`と`x86_64`のプラグインをAPK/AABへビルド時同梱し、非対応ABI、欠落プラグイン、初期化失敗をJavaScript側へ決定的なエラーとして返す。
6. Android接続テストを含む全プロジェクトテストがPASSし、Android用処理によってデスクトップCEF版の挙動が変わらない。
