# Android WebView版ネットワークフィルタの制約

## 結論

Android WebView版では、CEF版の選択的なネットワーク許可規則を再現しない。WebViewを外部ネットワークへ接続しないレンダラーとして扱い、アプリ内アセットと画像用`data:` URLだけを許可する。

この仕様変更を受け入れる場合、`plan.md`のステップ3は完了と判断できる。CEF版と同じ`network.allow`、`network.authorizedOrigin`、`network.localAccess`の動作が必須の場合、WebView案は採用条件を満たさないため、GeckoViewなど別のバックエンドを検討する必要がある。

## 確定したAndroid仕様

許可するURLは次の2種類に固定する。

| URL | 条件 | 用途 |
|---|---|---|
| `https://appassets.androidplatform.net/assets/`配下 | ホスト名とスキームは大文字小文字を区別せず、ポートは省略または443、パスは`/assets/`で始まること | `WebViewAssetLoader`から配信するアプリ内アセット |
| `data:image/`で始まるURL | URL文字列が小文字の`data:image/`で始まること | インライン画像 |

これ以外の`http:`, `https:`, `ws:`, `wss:`, `file:`, `content:`, `blob:`、画像以外の`data:`は、外部通信または遷移の許可対象にしない。Android版では設定ファイルの許可規則によってこの固定リストを拡張できない。

リリースビルドには`INTERNET`と`ACCESS_LOCAL_NETWORK`の権限を宣言しない。`android:usesCleartextTraffic`も`false`のままとする。計装テストはフィルタ自身の効果をOSの権限拒否と区別する必要があるため、デバッグビルドに限って両権限と平文HTTPを有効にする。

実装は[MuonWebViewNetworkFilter.java](muon-android-prototype/android/app/src/main/java/dev/muon/prototype/MuonWebViewNetworkFilter.java)に集約した。

## 遮断に使う層

単独のWebViewコールバックには全通信を捕捉できないため、次の層を同時に適用する。

| 層 | 設定または動作 |
|---|---|
| リリースManifest | `INTERNET`と`ACCESS_LOCAL_NETWORK`を付与しない。平文通信を禁止する |
| `WebSettings` | `blockNetworkLoads=true`、ファイルアクセスと`content:`アクセスを無効化し、mixed contentを禁止する |
| ナビゲーション | 固定許可リスト外を`shouldOverrideUrlLoading`で中止する |
| リソース要求 | アプリ内アセットを`WebViewAssetLoader`から返し、それ以外をHTTP 403相当の`WebResourceResponse`で拒否する |
| Service Worker | ファイル、`content:`、ネットワークアクセスを無効化し、専用コールバックでも全要求をHTTP 403相当で拒否する |
| CSP | `default-src 'none'`を基準に、`connect-src`, `frame-src`, `worker-src`, `object-src`, `form-action`などを`'none'`にする。スクリプトとスタイルは`'self'`、画像は`'self' data:`だけを許可する |

必要な`androidx.webkit.WebViewFeature`が1つでも利用できないWebViewプロバイダーでは、安全性を弱めて起動せず、`IllegalStateException`でfail-closedとする。このため、古いWebViewプロバイダーではAndroid版が起動できない可能性がある。

## 実測結果

2026年8月18日に次の環境で確認した。

- Pixel 6 AVD
- Android 17、API level 37、x86_64
- `com.google.android.webview` 149.0.7827.5
- アプリのtarget SDK 37

テストではアプリへ`INTERNET`と`ACCESS_LOCAL_NETWORK`を付与し、WebViewと同じアプリプロセス内で実TCPサーバーを起動した。JavaScript側の成功・失敗だけではなく、サーバーが要求を受信したかどうかを最終判定に使用した。

リリースAPKも別途生成し、Manifestの実体に`INTERNET`と`ACCESS_LOCAL_NETWORK`が含まれず、`android:usesCleartextTraffic=false`であり、デバッグ専用の観測Activityが含まれないことを確認した。

| 経路 | deny-by-default時の結果 | 追加の実測事項 |
|---|---|---|
| メインフレーム | 外部URLはサーバーへ到達しない | アプリから直接`loadUrl()`した場合も遮断できた |
| iframe | 外部URLはサーバーへ到達しない | 遮断時にもiframeの`load`イベントが発生し得るため、DOMイベントは許可判定の根拠にできない |
| `fetch` | Promiseが拒否され、サーバーへ到達しない | CSPとネットワーク遮断設定が有効だった |
| XHR | `error`となり、サーバーへ到達しない | CSPとネットワーク遮断設定が有効だった |
| WebSocket | 接続エラーとなり、ハンドシェイクはサーバーへ到達しない | 通常の`shouldInterceptRequest`には要求自体が現れない |
| リダイレクト | 初回URLもリダイレクト先もサーバーへ到達しない | フィルタを外した観測では初回URLだけが通常コールバックに現れ、リダイレクト先は現れなかった |
| Service Worker | 登録をCSPで拒否し、Workerのネットワークも無効化する | フィルタを外した観測ではWorkerからの`fetch`はService Worker専用コールバックにだけ現れた |
| `blob:` | iframe内の`blob:`文書を利用できず、その内部リソースもサーバーへ到達しない | フィルタを外すと`blob:`文書は読み込めるが、通常の`shouldInterceptRequest`には`blob:` URLが現れなかった |
| ローカルネットワーク | エミュレーターのプライベートIPv4アドレスへの`fetch`はサーバーへ到達しない | テストでは`ACCESS_LOCAL_NETWORK`を明示的に許可しており、OS権限ではなくフィルタが遮断した |
| `data:image/` | 1ピクセルGIFを正常に読み込める | 固定許可リストの例外として維持する |

この結果を固定する計装テストは[MuonNetworkFilterTest.java](muon-android-prototype/android/app/src/androidTest/java/dev/muon/prototype/MuonNetworkFilterTest.java)にある。コールバック範囲の観測には、リリースAPKへ入らないデバッグ専用の[MuonNetworkCapabilityProbeActivity.java](muon-android-prototype/android/app/src/debug/java/dev/muon/prototype/MuonNetworkCapabilityProbeActivity.java)を使用する。

## CEF版との相違

| 項目 | CEF版 | Android WebView版 |
|---|---|---|
| URL許可 | `network.allow`のURL全体globで追加可能 | 固定したアプリ内アセットと`data:image/`だけ。外部URLの追加は不可 |
| 許可オリジン | `network.authorizedOrigin`により、トップレベル遷移の宛先またはサブリソースのrequest initiatorを判定 | 未対応。外部オリジンは常に拒否 |
| request initiator | CEFコールバックから独立した値を取得して判定 | `WebResourceRequest`には相当するプロパティがない。`Referer`や`Origin`ヘッダーは全経路で得られる保証がなく、信頼できる代替にはしない |
| メインフレーム判定 | ナビゲーションとフレーム情報から判定 | `isForMainFrame()`は利用できるが、他の不足を補えない |
| リダイレクト | 各要求をネットワークポリシーで評価可能 | `shouldInterceptRequest`は最初のURLだけを通知し、リダイレクト先を再評価できない |
| WebSocketと`blob:` | 通常のネットワーク要求は`CefResourceRequestHandler`へ集約してポリシーを適用する | WebSocket要求と`blob:` URLは通常の`shouldInterceptRequest`に現れないため、このコールバックによる選択的許可には使えない |
| Service Worker | ブラウザー単位のCEF要求処理と同じポリシーへ接続できる | 別のService Worker用コールバックが必要で、その設定はプロセス全体に作用する |
| ローカルネットワーク | `network.localAccess.loopbackOrigins`と`localNetworkOrigins`を要求元オリジン単位で判定 | Android 17の`ACCESS_LOCAL_NETWORK`はホストアプリの実行時権限であり、WebViewもその状態を継承する。オリジン単位の許可にはできない |
| 拒否時の見え方 | HTTP 403応答とmuonの診断メッセージを返す | 経路によりHTTP 403、ナビゲーション中止、CSP違反、一般的なネットワークエラーのいずれかになる。CEF版と同じエラー文字列は保証しない |
| エンジンの固定 | 配布物に含めたCEFバージョンで固定 | 端末で選択されたWebViewプロバイダーと更新状態に依存する |

Androidの`WebViewClient`仕様でも、`shouldInterceptRequest`はリダイレクト後のURL、`javascript:`、`blob:`を通知せず、`shouldOverrideUrlLoading`はアプリ自身の`loadUrl()`とPOSTを含む全遷移を通知しないと明記されている。[WebViewClient API](https://developer.android.com/reference/android/webkit/WebViewClient)

Service Workerには別のコントローラーと設定が必要である。[ServiceWorkerControllerCompat API](https://developer.android.com/reference/androidx/webkit/ServiceWorkerControllerCompat)、[ServiceWorkerWebSettingsCompat API](https://developer.android.com/reference/androidx/webkit/ServiceWorkerWebSettingsCompat)

Android 17では`ACCESS_LOCAL_NETWORK`がtarget SDK 37以上のアプリに対する実行時権限となり、WebViewのローカル通信もホストアプリの権限状態を継承する。[Local network permission](https://developer.android.com/privacy-and-security/local-network-permission)

## 運用上の制約

- Android WebView内から外部HTTP、WebSocket、Service Worker通信を利用するアプリは動作しない。
- Android版では`network.allow`、`network.authorizedOrigin`、`network.localAccess`を設定しても外部通信を有効化しない。
- 外部通信が必要になった場合は、URL、リダイレクト、DNS解決後のアドレス、ローカルネットワーク、応答サイズをネイティブ側で一元検証できるHTTPプラグインを別ステップで設計する。WebView設定だけを部分的に緩和しない。
- デバッグManifestのネットワーク権限と平文通信許可は計装テスト専用である。リリースManifestへ移してはならない。
- WebView更新でコールバック範囲やCSP動作が変わり得るため、サポートするAndroidバージョンとWebViewプロバイダーごとに計装テストを再実行する。
- CEF版とのネットワーク互換が製品要件へ戻った場合は、このフィルタを拡張して近似せず、WebView採用判断そのものを再度行う。

## 完了条件との比較

`plan.md`のステップ3が挙げたメインフレーム、iframe、`fetch`、XHR、WebSocket、リダイレクト、Service Worker、`blob:`、ローカルネットワークはすべて実WebViewと実TCPサーバーで検証した。deny-by-default時は全経路でサーバー到達がなく、固定許可対象のアプリ内アセット、RPC、`data:image/`は引き続き動作した。

一方、CEF版の選択的ネットワーク契約は満たしていない。したがって、ステップ3の最終判定は「Android仕様を外部ネットワーク禁止へ変更する条件で完了」である。
