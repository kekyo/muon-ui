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
- プラグインランタイムの公開APIからCEF型は分離されていますが、browser builtin種別、filesystem path、dialog cancelなどdesktop固有の境界が残っています。[muon_plugin_runtime.h](/home/kouji/Projects/muon-ui/muon-core/src/plugins/muon_plugin_runtime.h:9)
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

### ステップ5: ネイティブプラグインのNDK対応

#### 事前検証

2026年8月19日に、当時の`cardio 0.2.0`、`tra-ffic 0.3.0`、およびtra-fficが使用する`libffi 3.4.6`を対象としてAndroid試験を行いました。submodule自体は変更せず、Android用ホストを追加したcardioの一時作業コピーと、Android instrumentation testへ一時的に組み込んだtra-fficの既存全回帰テストを使用しました。試験用変更と一時ビルド成果物は製品コードへ残していません。

#### 検証環境と範囲

- 実行環境はPixel 6 AVD、Android 17/API 37、`x86_64`、16 KiBページです。アプリは`targetSdk 37`、`minSdk 24`、NDK `29.0.14206865`でビルドしました。
- `x86_64`ではinstrumentation test全20件を実行し、既存18件と今回の互換性試験2件がすべてPASSしました。
- `arm64-v8a`では同じNDK/API 24設定でlibffi 3.4.6をビルドし、cardio試作、tra-ffic全回帰テスト、libffiを含む`libmuon_android_rpc.so`をAPKへ同梱できるところまで確認しました。生成ELFのLOADセグメントの`p_align`は`0x4000`でした。ただしarm64 VMまたは実機がないため、arm64でのclosure実行は未確認です。
- libffi 3.4.6の公式リリースアーカイブを使用し、確認したSHA-256は`b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e`です。

2026年8月20日にupstreamの正式Android対応版へ更新しました。cardio 1.1.0はAndroid API 24以降を対象として`dispatcher_host_android`と`dispatcher_host_android_auto`を提供し、tra-ffic 1.0.0はAndroid API 24以降の`x86_64`、`arm64-v8a`、4 KiB/16 KiBページをlibffi 3.8.0で正式に試験します。muonのsubmodule参照もこの2版へ更新し、tra-fficが固定するlibffi 3.8.0を再帰submoduleとして使用します。以下の3.4.6による結果は採用版決定前の履歴として残し、実装では正式対応版を基準にします。

同日に正式版のupstream Android全体試験も、NDK 29.0.14206865、SDK Platform 37.0、Build Tools 36.0.0、Android 37.1 `google_apis_ps16k`の`x86_64` VM、16 KiBページで再実行しました。

- cardio 1.1.0は、manual host、Java UI Looperへ接続するauto host、Android設定制約、両ABIのAPK/ELF 16 KiB整列を含む`test-android-runtime`がPASSしました。
- tra-ffic 1.0.0は、libffi 3.8.0を無改変の一時ビルドコピーから両ABI向けに静的ビルドし、全回帰、closure生成・呼び出し、両ABIのAPK/ELF 16 KiB整列を含む`test-android-runtime`がPASSしました。
- 生成されたx86_64とarm64-v8aの`fficonfig.h`はいずれも`FFI_EXEC_STATIC_TRAMP=1`と`FFI_MMAP_EXEC_WRIT=1`で、`FFI_EXEC_TRAMPOLINE_TABLE`は無効でした。従って採用版は、対応ABIでは静的実行トランポリンを優先し、必要時の実行可能マッピングfallbackもコンパイルする構成です。
- `arm64-v8a`はビルド、静的リンク、ELF/APK整列までPASSしました。この時点で利用可能なVMはx86_64だけだったため、arm64実行は後続の実機試験としました。

同日、USB接続したPixel 6実機でも正式版のupstream Android全体試験を再実行しました。実機は`oriole`、Android 17/API 37、`arm64-v8a`、4 KiBページです。

- cardio 1.1.0の`test-android-runtime`は、manual host、Java UI Looper auto host、Android設定制約、両ABI成果物検査を含めてPASSしました。
- tra-ffic 1.0.0の`test-android-runtime`は、libffi 3.8.0のclosure生成・呼び出しを含む全回帰と、両ABI成果物検査を含めてPASSしました。
- 実機に残っていた旧`com.example.traffic`テストpackageは別のdebug署名だったため更新を拒否されました。製品アプリではないことを確認してこのテストpackageだけを削除し、1.0.0のテストAPKをclean installして全体試験を再実行しました。
- arm64実行自体は確認できましたが、このPixel 6の実ページサイズは4 KiBです。arm64の16 KiBページ実行は引き続き完了条件として残します。

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

cardio 0.2.0の公開APIだけで同等ホストをmuon側に実装することはできませんでした。wait snapshotの作成・回収、キュー取得、timer deadlineなどがprivateで、当時は既存のauto hostだけがfriendだったためです。この課題はcardio 1.1.0の正式な`dispatcher_host_android_auto`でupstream解決済みです。muonはこの公開hostを使用し、private実装の複製や独自ポーリングを持ちません。

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

tra-ffic自身にはAndroid固有の機能不良を再現できませんでした。`TRA_FFIC_IN_POSIX`、pthread、libffi ABIはいずれもx86_64で正常でした。当時残っていたAndroid公式テストとarm64確認の不足は、tra-ffic 1.0.0のAndroid artifact/runtime CIとlibffi 3.8.0への更新でupstream対応済みです。muon側ではupstreamと同じ版・ABI・16 KiB整列条件を再現し、最終APK内でのclosure実行を接続テストします。

libffi 3.8.0は対応するLinux x86_64/AArch64で静的実行トランポリンを既定有効にでき、トランポリンコードと書き込み可能なパラメーターを別マッピングにします。従って、最初の実装ではlibffiのソースを変更せず、tra-fficが固定するリビジョンをmuonのビルドディレクトリへ無改変でビルドします。生成された設定で静的トランポリンが有効なことと、実行時マッピングがW^Xを満たすことは両ABIの接続テストで検証します。

将来libffiへAndroid固有の変更が必要になった場合も、libffi submoduleの作業ツリーは変更しません。候補は優先順に次のとおりです。

1. コンパイル定義、configure cache、CFLAGS/LDFLAGSだけで解決できる場合は、muon-builderの外部ビルドレシピで指定する。upstreamソースを変更せず、更新追従が最も容易なため第一候補とする。
2. ソース修正が必要な場合は、muon内にbase libffi commitへ紐付くpatch queueを置き、submoduleからビルド用一時ディレクトリへエクスポートしたコピーに`git apply --check`後に適用する。元submoduleをcleanに保ち、パッチ不一致を更新時の明示的エラーにできるため、一般的なソース修正の推奨案とする。
3. `ffi_closure_alloc()`/`ffi_closure_free()`だけを差し替えればよい場合は、muon所有のallocator shimを最終リンク時の`--wrap`で注入する。差分を小さくできる一方、NDK linker依存となり、libffi内部からの参照や全ABIでの動作を別途検証する必要がある。
4. allocator実装全体を置換する必要がある場合は、muon所有のビルドmanifestでlibffiの対象source listを管理し、upstreamの該当translation unitをmuon側実装へ置換する。内部実装への結合が強く更新コストも高いため、patch queueでも対処できない場合だけ採用する。

どの方式でも、対象libffi commit、適用した設定またはpatchのSHA-256、生成物のABI・ELF整列、closure回帰テストをビルド記録へ含めます。libffiのsubmodule参照そのものをmuon独自commitへ差し替える方法は採用しません。

#### upstreamとmuonの分担

| 対象 | 判定 | 対応先 |
|---|---|---|
| Androidの既存Looperへ接続するauto host | cardio 1.1.0の`dispatcher_host_android_auto`で正式対応済み | muonは公開hostを使用し、upstreamのAndroid回帰テストに加えてmuon接続テストを行う |
| Looper eventからcardio内部snapshotへの`revents`転記 | 試作で再現した必須処理をcardio 1.1.0が内部実装する | cardio upstream実装を利用し、muon側へ複製しない |
| tra-fficのPOSIX/libffi動作 | tra-ffic 1.0.0がAndroid両ABIと4 KiB/16 KiBページを正式対応する | tra-ffic本体のmuon独自修正は不要 |
| Android NDKの継続試験とarm64 hardening | tra-ffic upstreamにartifact/runtime CIが追加済み | muonでも最終APKを対象とした両ABI接続テストを行う |
| libffiの版、取得、ハッシュ、ABI別静的ビルド | tra-ffic 1.0.0がlibffi 3.8.0を固定し、muonの配布物と再現可能ビルドに属する | muonの外部ビルドレシピで管理し、必要な変更は一時コピーへのpatch queueなどsubmodule外で適用する |
| APK/AABへのプラグイン同梱、JNI境界、Activityライフサイクル | muon固有 | muon側で実装する |

#### 実装開始条件の判定

ステップ5の実装開始条件は満たしています。

- cardio 1.1.0、tra-ffic 1.0.0、libffi 3.8.0の採用commitが固定され、すべてのsubmodule作業ツリーがcleanです。
- upstreamの正式なAndroid全体試験は、16 KiBページの`x86_64` VMと4 KiBページの`arm64-v8a` Pixel 6実機でPASSしています。
- cardioはJava UI Looperへ自動接続する公開hostを持ち、muon側でprivate実装を複製する必要がありません。
- tra-fficとlibffiは両ABIでclosureを実行でき、現時点ではlibffi sourceへのdownstream変更を必要としません。

`arm64-v8a`の16 KiBページ実行は最終完了条件として残しますが、両ABIの16 KiB整列成果物と`x86_64`の16 KiB実行を確認済みなので、実装開始を妨げる条件とはしません。

#### 現在の実装との差分

現在のAndroid試作は、CEF非依存の`muon_rpc_core`だけをJNI libraryへリンクしています。[Android CMake](/home/kouji/Projects/muon-ui/muon-android-prototype/android/app/src/main/cpp/CMakeLists.txt:5)
JNIの`invoke_plugin`は常に「plugin routes are unavailable」を返し、plugin metadata、tra-ffic、libffi、cardio hostをまだ接続していません。[Android JNI host](/home/kouji/Projects/muon-ui/muon-android-prototype/android/app/src/main/cpp/muon_android_rpc_jni.cpp:584)
GradleのABIも現在は`x86_64`だけです。[Android Gradle設定](/home/kouji/Projects/muon-ui/muon-android-prototype/android/app/build.gradle.kts:20)

一方、`MuonPluginRuntime`の公開境界はCEF型を含みませんが、実装sourceはdesktopのbuiltin filesystem、executor、browser、ログ、実行ファイル相対path解決を直接参照しています。[plugin runtime](/home/kouji/Projects/muon-ui/muon-core/src/plugins/muon_plugin_runtime.cpp:14)
従って、Android用に別のplugin runtimeを複製するのではなく、共通runtime coreとdesktop builtin adapterを分離してからAndroidへリンクします。

#### 実装方針

- 対象はAPK/AABへbuild時に同梱した正式なMuon pluginだけとします。downloadしたcode、外部storage、`plugin.path`のruntime探索、`muon.executor.loadLibrary()`は実装しません。AndroidもAPK外codeの動的loadを避けるよう推奨しています。[Android Dynamic Code Loading](https://developer.android.com/privacy-and-security/risks/dynamic-code-loading)
- pluginの論理名とpackage内のELF sonameをbuild-time registryで明示的に対応付けます。runtimeはdirectory scanやファイル名推測を行わず、registryにあるsonameだけを`dlopen()`します。Android packageのnative library配置は`lib/<abi>/lib<name>.so`に固定します。[Android ABI管理](https://developer.android.com/ndk/guides/abis)
- Androidの組み込みbrowser、environment、filesystem関数は、現在どおり`MuonRpcRouteKind::Platform`としてJava serviceへ送ります。desktop専用builtin pluginをAndroid native runtimeへリンクしません。
- Android processにつき`dispatcher_host_android_auto`と`MuonPluginRuntime`を一組だけ作り、Activity/WebViewごとに独立したRPC owner/sessionを登録します。Activity再生成時にcardio dispatcherを重複生成しません。
- `libcardio.so`はhostとcardio利用pluginが共有します。JNI library、`libcardio.so`、C++ pluginのすべてでNDK 29.0.14206865の`libc++_shared.so`を一つだけ使用します。複数のshared libraryへ`libc++_static`を重複リンクすると、allocation、exception、C++ global stateが未定義動作になるためです。[Android NDK C++ runtime](https://developer.android.com/ndk/guides/cpp-support)
- tra-fficはheader実装を共通runtime coreへ組み込み、libffi 3.8.0はABI別の`libffi.a`として静的リンクします。libffi submodule sourceは変更しません。
- すべてのAndroid ELFとAPK/AABを16 KiBページ対応として生成します。NDK r28以降は16 KiB整列が既定ですが、最終成果物の全`LOAD` segmentとpackage alignmentを試験で直接確認します。[Android 16 KiBページ対応](https://developer.android.com/guide/practices/page-sizes)
- 現在の`muon-android-prototype`をステップ5の結合・配布形式試験hostとして使用します。公開`muon-ui` target、Play Store署名、installerはこのステップへ含めませんが、再利用できるmuon-core target、registry generator、Gradle/CMake recipeとして実装し、debug/release APKとrelease AABの両方を成果物にします。
- 各実装項目は、期待する機能を検証する試験を先に追加して全体実行でREDを確認し、実装後に同じ全体試験をGREENにします。各GREENごとに以下の粒度でコミットし、submodule内には変更を作りません。
- 実装中にcardioまたはtra-fficの公開APIだけで再現するAndroid固有課題を新たに検出した場合は、muonへprivate実装のcopyや恒久的workaroundを入れません。最小再現試験、ABI、ページサイズ、lifecycle条件、実行logをupstream課題として切り出し、upstreamで修正された正式commitへsubmodule参照を更新してからmuon接続試験をGREENにします。libffi課題だけは前節どおりmuonのbuild設定または一時copyへのpatch queueで管理します。ステップ5の最終まとめには、新たに摘出した課題、再現条件、対応先、解決状況を一覧で残します。

#### 実施計画

##### 1. plugin runtime coreをplatform非依存にする

`MuonPluginRuntime`のFFI marshalling、plugin metadata検証、function wrapper lifetime、tra-ffic queue、外部plugin登録を`muon_plugin_runtime_core`として独立したCMake targetにします。desktopのbuiltin plugin初期化、browser function種別、filesystem dialog cancel、実行ファイル相対path、CEF用log sinkはdesktop adapterへ移します。Androidはcoreだけをリンクし、platform functionは既存Java serviceへ残します。

runtime serviceにはowner-thread判定、owner-threadへのpost、buffer確保、owner生存判定、transport送信に加え、platform logとpackage library loaderに必要な境界を明示します。desktop adapterは現行のpath、signature、salt、builtin挙動を維持し、Android adapterはbuild-time registryのsonameだけを受理します。logical plugin nameとlibrary locatorを分離し、Androidでdesktopの`name + ".so"`規則を流用しません。

先に、desktop builtinをリンクしないcoreだけでpluginをload、invoke、stopできるhost試験と、NDK CMakeでcoreをlinkする試験を追加します。完了条件は、Android targetがCEF、GTK、GIO、desktop executorへ依存せずlinkでき、既存desktop全体試験の結果が変わらないことです。

コミット境界は`refactor: extract portable native plugin runtime`とします。

##### 2. Android native dependencyを再現可能にbuildする

muon所有のAndroid build recipeを追加し、tra-fficが固定するlibffi sourceをbuild directoryへ無改変でcopyしてから、`x86_64-linux-android`と`aarch64-linux-android`の各hostで`--disable-shared --enable-static --disable-docs --with-pic`を指定してconfigureします。入力commit、NDK、API 24、configure引数、CFLAGS、LDFLAGS、patch一覧とSHA-256をmanifestへ記録します。初期状態のpatch一覧は空です。

同じCMake graphでupstreamの`libcardio.cpp`からABI別`libcardio.so`をbuildし、`CARDIO_SHARED_LIB=1`、`CARDIO_BUILD_SHARED_LIB=1`、`CARDIO_HAS_POSIX_FD=1`、`CARDIO_WITH_LINUX_IO_URING=0`を固定します。JNI libraryとcardioを利用するpluginは`CARDIO_SHARED_LIB=1`でこの一つのlibraryへlinkします。Android CMakeには`ANDROID_STL=c++_shared`を指定し、Gradleに`libc++_shared.so`を一つだけpackageさせます。

先に、両ABIのdependency manifest、`fficonfig.h`、static trampoline設定、library依存関係、16 KiB ELF整列を実成果物から検証するartifact試験を追加します。完了条件は、clean buildから同じ入力で両ABIを再生成でき、libffi submoduleがcleanなままであることです。

コミット境界は`chore: build Android native plugin dependencies`とします。

##### 3. build-time plugin registryとpackage処理を作る

Android build入力から、logical plugin name、package soname、ABI別artifact、`allow`、string `config`を持つmuon所有manifestを生成します。`plugin.path`、runtime `signature`、`salt`はAndroid入力として拒否します。重複logical name、重複soname、`lib<name>.so`形式でないlibrary、片方のABI欠落、ELF machine不一致、plugin API entry point欠落はpackage前にbuild errorにします。

registry generatorはC++用の固定tableとGradle/CMake用staging一覧を同じ正規化済み入力から生成します。runtimeはtableを列挙順にloadし、filesystem存在確認を前提にせずpackage sonameで開きます。`dlopen()`または`dlsym("muon_init_plugin")`の失敗、pluginがloadを辞退した場合、metadata不正、allow対象関数が0件の場合は、logical plugin nameを含む決定的なstartup errorにします。

simple modeの公開metadataは実際にloadしたAndroid pluginからpage load前に取得します。validate modeのexact importは既存のfunction pathを使用し、wildcard importがある場合だけ現行と同様にhost用plugin artifactをinspectorへ渡してcatalogを生成します。手書きのAndroid function catalogを真実源にはしません。

先にmanifest validationの全ケースと、意図的にmissing/invalid pluginを持つAndroid build variantの失敗試験を追加します。完了条件は、registry外libraryをruntimeが探索せず、両ABIのAPK/AABへregistryどおりのpluginだけが入ることです。

コミット境界は`feat: package Android native plugins`とします。

##### 4. process単位のcardio hostとplugin runtimeを接続する

Java main Looper上でnative process runtimeを作り、最初に`dispatcher_host_android_auto`、次に`MuonPluginRuntime`を構築します。runtime servicesはmain-thread判定、cardioによるowner-thread post、process内buffer、Activity session registryによるowner生存判定、WebView transport送信、logcat sinkを提供します。

現在のJNI stateはJNI呼び出し中だけ有効な`JNIEnv*`を一時保持するため、cardio callbackから完了するpluginには使用できません。process runtimeは`JavaVM*`と必要最小限のglobal referenceを保持し、callback時に現在のmain threadの`JNIEnv*`を取得します。released sessionへは送信せず、global/local referenceを所有規則どおり解放します。

各Activity/WebViewは単調増加するowner idを持つRPC sessionだけを作ります。`pagehide`と`onDestroy`でcall、renderer function、plugin proxy、binary attachmentをreleaseします。configuration changeではprocess runtimeを維持し、最後の通常session終了ではpluginの非同期`Stop()`を開始します。停止中に新しいActivityが来た場合はpollingやblockを行わず、Stop完了、plugin unload、cardio host破棄を同じLooper上で終えてから新runtimeを作り、待機sessionをattachします。OSによるprocess強制終了ではstop callbackが保証されないことはAndroid lifecycleの制約として扱います。

先に、Muon test pluginからcardio dispatcherがinit時にもcall時にも存在すること、即時、timer、fd、別thread postがJava `Handler`と同じmain threadで完了すること、session release後のcallbackを配送しないことをinstrumentation testへ追加します。完了条件は、独自loop、worker thread、pollingを追加せず、Activity再生成と終了・再起動でdispatcher、callback、fd数が増え続けないことです。

コミット境界は`feat: host Android plugins on the main looper`とします。

##### 5. plugin metadataとfull-duplex RPCをWebViewへ接続する

JNIの固定`ResolveFunctionId()`を、platform routeと`MuonPluginRuntime::GetFunctions()`から作るtableへ置き換えます。ID空間の重複を検証し、plugin routeは`MuonRpcRouteKind::Plugin`として`GetCallArgumentTypes()`、`Invoke()`、`ReleasePluginFunctionProxy()`、`ReleaseFunctionContext()`へ接続します。platform routeは現在のJava serviceへ残します。

WebView codecは、現在のstring、boolean、`u32`、binaryだけでなく、Muon plugin ABIの全scalar、null string/pointer、64 bit integer表現、nested function signature、renderer-owned function、plugin proxy、buffer viewをCEF側と同じ意味で扱います。hostからJavaScript functionを呼ぶmessage、JavaScriptから結果を返すmessage、proxy releaseをprotocolへ追加します。cancel後のnative完了は破棄し、context releaseを最終的なlifetime cleanupにします。binaryはWebView境界でcopyし、pluginが保持できる期間を既存`muon_plugin_api.h`契約から変更しません。

simple modeでは、load済みmetadataから許可されたnamespace/functionだけをdocument開始時に構築し、pluginのsetup scriptも許可済み関数だけを対象に実行します。validate modeでは既存のVite virtual moduleと`globalThis.__muon_plugin_call`を使い、capability idとfunction pathをnative側でも検証します。どちらも構成済みasset originのmain frame以外へplugin bridgeを公開しません。

先に、既存のtypes、recursive functions、function lifetime、cardio test pluginをAndroid用にbuildし、primitive、64 bit、binary、async completion、JavaScript callback、function return/proxy identity、release、allow、configをpublic JavaScript APIから検証する試験を追加します。完了条件は、同じtest pluginの意味上の結果がdesktop CEFとAndroid WebViewで一致することです。

コミット境界はscalar/binary経路を`feat: invoke Android native plugins`、function/lifetime経路を`feat: bridge Android plugin functions`に分けます。

##### 6. failure、W^X、package、lifecycleをhardeningする

非対応ABI、plugin欠落、entry point欠落、init失敗、metadata不正、duplicate path、allow不一致をそれぞれ再現し、build時に判定できるものはbuild error、install後にしか判定できないものはWebViewをloadする前のstartup errorとして返します。失敗時も既にloadしたpluginを逆順にStop/unloadし、cardio hostとJNI referenceを解放します。

libffi closureについては、最終`libmuon_android_rpc.so`内でfunction marshallingを実行し、closure allocation/freeが釣り合うこと、実行addressが実行可能であること、対応する書き込みdataと実行codeが同じ`rwx` mappingになっていないことを`/proc/self/maps`の実測で確認します。libffi変更が必要になった場合だけ、前節の優先順位に従ってmuon build設定、次に一時copyへのpatch queueを使用します。

artifact試験はdebug/release APKとrelease AABについて、`arm64-v8a`、`x86_64`、`libmuon_android_rpc.so`、`libcardio.so`、`libc++_shared.so`、全pluginの存在、ELF machine、`DT_NEEDED`、`LOAD` segmentの`0x4000`整列、APK zip alignmentを確認します。AABから生成したinstallable split APKも検査し、実際に端末へinstallして試験します。

先に各failureとlifecycle leakの試験を追加します。完了条件は、成功・失敗・cancel・Activity再生成・runtime停止のすべてでpending call、function lease、closure、plugin task、Looper fdが回収されることです。

コミット境界は`fix: harden Android native plugin lifecycle`とします。

##### 7. 全体試験と完了判定を行う

各GREEN commit前に個別testだけを実行せず、`npm test`で全workspace、`npm run test:android --workspace muon-android-prototype`で全instrumentation testを実行します。最終判定では次のmatrixをすべて実行します。

| 対象 | ABI・ページ | 実行内容 |
|---|---|---|
| Android WebViewローカル完了gate | `x86_64`・16 KiB VM | debug instrumentation全件、release APK、AAB由来split APK |
| Android WebView実機確認 | `arm64-v8a`・4 KiB Pixel 6 | ローカル完了後に実機を手動接続し、debug instrumentation全件、release APK、AAB由来split APKを実行する |
| Android WebView追加互換性gate | `arm64-v8a`・16 KiB実機またはVM | 実行環境を利用できる場合にplugin全回帰、closure/W^X、lifecycle全件を実行する |
| cardio upstream | 実行可能な各Android環境 | `test-android-runtime`全体 |
| tra-ffic upstream | 実行可能な各Android環境 | `test-android-runtime`全体 |
| desktop回帰 | Linux、Windows i686/amd64 | rootの全workspace test。Android分岐による挙動差がないこと |

時間待ちで成否を推測せず、Java/native双方のcompletion、latch、resource countで終了を判定します。端末またはVMのABI、API、実ページサイズをtest開始時にassertし、想定と違う環境でPASSにしません。

今回の実装作業は、両ABIのbuild・artifact試験、rootの全workspace test、およびローカル`x86_64`・16 KiB VMの全instrumentation testがGREENになった時点をローカル完了とします。接続済みの実機があってもこの段階では使用しません。Pixel 6の`arm64-v8a`・4 KiB実行は、ローカル完了後に利用者が実機を手動接続したことを確認してから独立して行います。利用可能な`arm64-v8a`・16 KiB実行環境がない場合は、16 KiB整列済みarm64成果物の検査までを記録し、runtime確認を未実施と明記します。

最終GREEN後に利用者向けplugin build/package手順、対応ABI、build-time同梱制約、Android process kill時のstop制約を文書化し、`doc:`コミットを作ります。

#### ステップ5の完了条件

1. cardio 1.1.0の`dispatcher_host_android_auto`をprocessのmain Looper上で使用し、muon側にcardio private実装のcopy、独自loop、polling、安易なworker threadがない。
2. 共通`MuonPluginRuntime` coreをdesktopとAndroidで使用し、Android native targetがCEF、GTK、GIO、desktop builtin executorへ依存しない。
3. Androidではbuild-time registryにあるpackage sonameだけをloadし、`plugin.path`探索、runtime download、外部plugin signature検査を行わない。`allow`、`config`、simple/validate capabilityは既存契約を維持する。
4. Java main message、cardioの即時、timer、fd、別thread post、plugin completionが共存し、Activity再生成、終了・再起動、cancel後にもcallback、pending call、function lease、closure、plugin task、Looper fdが残らない。
5. primitive、64 bit、binary、renderer function、plugin proxy、async completionを含むMuon plugin結合試験が`x86_64`と`arm64-v8a`でPASSし、desktop CEF版と意味上の結果が一致する。
6. tra-fficの全回帰と最終muon library内の直接closure呼び出しが16 KiBページの`x86_64` VMでPASSする。`arm64-v8a`は4 KiB Pixel 6で実機確認し、16 KiB環境を利用できる場合は同じ試験を追加実行する。利用できない場合もarm64 ELF/APK/AABの16 KiB整列検査を必須とする。
7. libffi 3.8.0のcommit、公式取得元、NDK、API level、configure引数、コンパイルフラグ、patch hashを固定し、ABI別に再現可能な`libffi.a`をbuildする。静的トランポリンとW^Xを実行時に検証し、libffi submoduleを変更しない。
8. debug/release APKとrelease AABが`arm64-v8a`、`x86_64`、共有cardio、共有libc++、registry内pluginを含み、全ELFとpackageが16 KiBページ対応である。非対応ABI、plugin欠落、entry point欠落、init失敗をbuild時またはpage load前に決定的なerrorとして返す。
9. Android実機・VM接続試験を含む全project testとupstream Android全体試験がPASSし、Android分岐によってdesktop CEF版の挙動が変わらない。
