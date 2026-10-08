通知の構造を検討している理由が:
  - muonアプリは基本的にはページ(WebView)上のJSでコードを実装している
  - androidの場合、muonアプリはインアクティブになり得る
  - もし、Firebaseのような通知システムをページ上で使用している場合、muonアプリがインアクティブになっていると、通知を受信できない
  のではないか？

という懸念から来たものです

---

はい、その懸念は正しく、設計課題の本質は「ページJSをbackground execution unitと見なせない」ことです。

前の考察でprovider抽象化まで広げましたが、より直接的には次の問題です。

## ページ上のJSだけでは受信を保証できない

AndroidでMuonが非アクティブになると、WebViewには次が起こり得ます。

  - page JSの実行が抑制・停止される。
  - WebView rendererがkillされる。
  - Activityごと破棄される。
  - app process自体がkillされる。

したがって、ページ上のFirebase listener、WebSocket、Realtime Database listenerなどをpush受信の唯一の入口にはできません。process がまだ残っているbackground状態でも、継続実行を保証すべきではありません。

Firebase Cloud MessagingのWeb版は、通常のブラウザではpageとは別のService Workerを使ってbackground messageを処理します。FCM Web受信仕様 (https://firebase.google.com/docs/cloud-messaging/web/receive-messages)

しかしFCM WebはPush APIを持つ対応ブラウザを前提としており、Firebaseの対応環境にもAndroid WebViewは含まれていません。FCM Web setup (https://firebase.google.com/docs/cloud-messaging/web/get-started)、Firebase JavaScript SDK対応環境 (https://firebase.google.com/docs/web/environments-js-sdk)

Android WebViewがService Workerの一部を実装していることと、ブラウザと同じWeb Push transportによってapp processを起こせることは別です。現在Muonが設定しているService Workerもrequest interception用であり、外部pushによるprocess起動を提供するものではありません。

## Muonに必要なのはService Worker相当のbackground entry

望ましい構造は次です。

  Android native push entry
            |
            v
      Muon event inbox
            |
            +-- WebViewがactive ----> page JSへ即時配送
            |
            +-- WebViewがinactive --> QuickJS background entry
            |                               |
            |                               +-- 処理結果をinboxへ保存
            |
            +-- 次回起動/tap -------> page-ready後にpage JSへ配送

つまりMuon applicationには、概念上二つのJS entryを持たせます。

  - UI entry
      - WebView上で動く通常のapplication code
      - UIがactiveな間だけ実行可能

  - background entry
      - QuickJS上で動く固定module
      - pushなどの外部eventに応じてheadless実行
      - UIを直接操作せず、必要なactionをinboxへ残す

これはWebにおけるpageとService Workerの関係に近いものです。ただし、同じglobal stateやmodule instanceを共有するものではありません。

## 現行QuickJS実装はまだこの役割を果たさない

現在のQuickJS Serviceは、Activity生成時にWebView bridgeからbindされます。core/android-test/android/app/src/main/java/dev/muon/runtime/MuonActivity.java:107

Activity破棄時にはruntimeをすべてshutdownしてunbindします。core/android-poc/android/app/src/main/java/dev/muon/runtime/MuonJavaScriptRuntimeBridge.java:310

したがって現在は、

  WebView -> QuickJS runtime

という構造であり、必要なのは、

  WebView -> shared QuickJS client
  Android background Worker -> shared QuickJS client

への変更です。

push受信componentはまずeventを永続保存し、WorkManagerがActivityとは独立してQuickJS Serviceへbindし、background moduleを実行する形が適しています。

## QuickJSを採用しない場合との差

QuickJSなしでもnative push自体は受信できます。その場合は、

  - native codeだけでsystem notificationを表示する。
  - eventを保存する。
  - ユーザーのnotification tapまたは次回app起動時にpage JSへ渡す。

ところまでは実現できます。

一方、

  - WebViewが存在しない状態でも開発者のJSを実行したい。
  - JSでnetwork requestやlocal data更新を行いたい。
  - 結果を次回WebViewへ渡したい。

という要件を満たすには、QuickJSのようなActivity非依存runtimeが必要です。

したがってQuickJS採否に関する本当の選択は次のようになります。

   QuickJS              WebView非アクティブ時
  ━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   採用しない           native処理とevent保存のみ。JS処理は次回foregroundまで延期
  ───────────────────  ───────────────────────────────────────────────────────────
   採用する             専用background moduleを起動してJS処理可能
  ───────────────────  ───────────────────────────────────────────────────────────
   WebViewを強制起動    background Activity start制限があるため一般解にはしない

## Firebaseをページから直接使えるか

Firebase Web MessagingをMuonページへ入れただけでは、Android background受信を保証できません。既存Web applicationを移植する場合も、FCM Web registrationをそのままAndroid native registrationへ置き換えられるわけではありません。

Muonとしては、

  - page codeはmuon.pushのようなprovider非依存APIを使う。
  - Android backendがnative FCM SDKなどへ接続する。
  - active時はpage listenerへ配送する。
  - inactive時はbackground QuickJSまたはinboxへ配送する。

というfacadeが必要です。

FCMは最初のnative adapterで構いません。ここで重要なのはprovider一般化よりも、FCM callbackとWebViewの間にActivity非依存のevent inboxを置くことです。

## plan4で先に決めるべきこと

notification機能全体を今すぐ実装する必要はありませんが、QuickJS正式採用前に次だけはgateとして検証すべきです。

  1. Activity/WebViewが存在しない状態でnative eventを受信できる。
  2. eventをprocess killに耐えるinboxへ保存できる。
  3. WorkManagerからQuickJS background moduleを起動できる。
  4. QuickJSの結果を次回WebViewへ配送できる。
  5. 同じeventの再実行を許容する冪等契約がある。
  6. QuickJSを除外した場合は、JS処理が次回foregroundまで延期されることを明示する。

このgateは実際のFCMをまだ組み込まず、fake native event sourceで検証できます。その後FCM、HMSなどをadapterとして接続すれば、QuickJSのlifecycle設計とprovider選択を分離できます。

---

どちらかといえば、QuickJSで通知を受信する機能は「望ましい」レベル感で、必ず達成したい要件ではありません。おそらく多くのmuonアプリ開発者は、CEF muon同様にページ主体でJSコードを記述するので、WebView上のJSでネットワーク通知を取りこぼすことなく（但しプロバイダのベストエフォートは問題ない）受信したい、と考えると思われます。
これは、androidネイティブなアプリケーションをkotlinやflutterで書いているのと同じような振る舞いに近づけたい、という意図に近いで
す。
なので、通知プロバイダからの受信をJSで（インアクティブ時も）到達出来るようにすることが目標になります（繰り返しますが、ベストエフォートであるのは理解した上で、かつ、通知は一旦通知領域に表示され、ユーザーがタップを必要とすることも問題ありません）

---

理解しました。その要件なら、QuickJSによるbackground処理は必須ではありません。目標は「通知到着時にページJSを実行する」ことではな
  く、次のように定義するのが適切です。

> providerから端末へ配送された通知をnative層で保持し、WebViewが次に実行可能になった最初の時点で、ページJSへ取りこぼさず配送する。

これはKotlinやFlutterの一般的な挙動にも近いです。UIのActivityやFlutter isolateが常時動いているわけではなく、native componentが受信し、通知tapなどでUI runtimeが再開された後にpayloadを渡します。

## 想定する配送モデル

  T0: providerから通知到着
      |
      +-- WebView active
      |      native受信 -> 永続化 -> page JSへ即時配送
      |
      +-- WebView inactive
             native受信 -> 永続化 -> 通知領域へ表示
                                        |
                                        +-- ユーザーtap
                                        |      Activity/WebView起動
                                        |      -> page-ready
                                        |      -> page JSへ配送
                                        |
                                        +-- tapしない
                                               次回通常起動時に配送

この場合、「インアクティブ時にもJSへ到達する」は、厳密には「インアクティブ時にnative層まで到達して保存され、JSが再び実行可能になった時点で配送される」という意味になります。

WebViewが存在しない瞬間にJS callbackを実行したい場合だけQuickJSが必要です。

## 必要なnative層

Muon Android backendに、ActivityやWebViewから独立した次の機能を持たせます。

  1. provider adapterが通知を受信する。
  2. payloadを正規化し、app-privateな永続inboxへ保存する。
  3. notificationを表示し、PendingIntentにはpayloadではなくevent IDを格納する。
  4. notification tapをonCreate()またはonNewIntent()で受け取る。
  5. WebViewのMuon bridgeが準備できるまで待つ。
  6. ページJSがlistenerを登録した後、未配送eventを渡す。
  7. JS側の処理完了後にackし、inboxから削除する。

重要なのは、native側から一方的に早いタイミングでJS eventを発火しないことです。Activity起動直後はまだページbundleやlistenerが準備できていないため、ページ側からの「購読準備完了」handshakeまたはpull APIが必要です。

## ページ開発者から見える形

API名は別途確定するとして、概念的には次の利用形になります。

```ts
  const registration = await muon.push.register();
  await applicationServer.registerPushEndpoint(registration);

  muon.push.onMessage(async (event) => {
    await updateApplicationState(event.data);
  });
```

同じlistenerが、

  - foregroundでの即時受信
  - notification tapによるcold start
  - backgroundからのwarm resume
  - renderer reload後の未ack event再配送
  - notificationをtapしなかった場合の次回通常起動

を区別せず受け取れるようにします。

必要ならeventに次の情報を付けられます。

  - receivedAt
  - provider
  - messageId
  - deliveryReason: foreground、notificationTap、resume
  - notificationAction
  - attempt
  - expiresAt

providerのtokenやregistrationはopaqueな値としてページへ返し、ページ側が通常のnetwork APIでapplication serverへ登録します。これにより、ページ主体の開発モデルを維持できます。

## FCMではdata messageを推奨する

FCMのbackground notification messageは、Firebase SDKがnotificationを直接表示し、アプリcodeにはユーザーtap後のIntent
extrasとして渡されます。FCM Android受信仕様 (https://firebase.google.com/docs/cloud-messaging/android/receive-messages)

この方式でも「tap後にページへ渡す」は可能ですが、Muon native層はtap前にpayloadを永続化できません。ユーザーがnotificationをdismissした場合、ページへ配送されない可能性があります。

Muonがprovider delivery後のローカルな取りこぼしを防ぐなら、推奨経路は次です。

FCM data message
  -> Muon FirebaseMessagingService
  -> durable inboxへ保存
  -> Muonがsystem notificationを表示

これならnotificationをdismissしても、次回foreground時にpending messageをページへ渡せます。

一方、provider-managed notificationも互換モードとして扱えますが、その場合の契約は「notification tap時だけpayloadを取得できる」と明記する必要があります。

## 配送保証

Muonが保証できる境界は次のようになります。

- providerからnative adapterまで:
    - providerのbest effort

- native adapterがinboxへcommitした後:
    - at-least-onceでページJSへ配送

- JSがackした後:
    - 通常は再配送しない

- JS実行中のrenderer/process kill:
    - 未ackなので次回再配送

exactly-onceは保証できないため、event IDを提供し、handlerは冪等に実装できるようにします。queueにはTTL、最大件数、retry上限も必要です。

## QuickJSの位置付け

この設計でQuickJSの役割は明確にoptionalになります。

   状態                       QuickJSなし                      QuickJSあり
  ━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━  ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
   WebView active             ページJSへ即時配送               同じ
  ─────────────────────────  ───────────────────────────────  ─────────────────────────────
   WebView inactive、tap前    native保存とnotification表示     必要ならbackground JSも実行
  ─────────────────────────  ───────────────────────────────  ─────────────────────────────
   notification tap後         WebView起動後にページJSへ配送    同じ
  ─────────────────────────  ───────────────────────────────  ─────────────────────────────
   tapなし                    次回foregroundまで保留           background JSだけ先行可能

QuickJSは「通知をページへ確実に届けるため」には不要です。必要になるのは、「ユーザーがtapする前にも開発者のJS処理を行いたい」場合だけです。

したがって、この通知要件をQuickJS正式採用のblocking gateにする必要はありません。plan4.mdでは次のように分離するのが適切です。

- 必須候補: native push ingress、durable inbox、notification tap、WebView page-ready配送
- optional: QuickJS background handler
- provider別: FCMを最初のadapterとし、公開ページAPIはprovider非依存

これなら、CEF版と同じページ主体の開発体験を維持しつつ、Android nativeアプリに近いnotification lifecycleを提供できます。

---
 
 - QuickJSへの対応は保留します
 - muon.push.onMessage() には競合条件があるかもしれません。WebViewがインアクティブ状態になるばあい、ロードして動作中のJSのインスタンスはどのように扱われますか？もし、インスタンスが破棄され、次回アクティブ化のときにはページが再度ロードされたのとおなじような条件となる場合、コールバックフックは存在しないので再びonMessage()でフックされるまで、通知を受信すべきハンドラは存在しないことになります。これはmuonアプリ開発者側も認識すべき制約かも知れませんが、少なくともこの間に受信したメッセージがロストしないようにする必要があるかも知れません

---

その認識で正しいです。QuickJSを保留するなら、ページ上のcallback自体は永続化できないため、「callbackが存在しない期間もmessageだけはnative側に保持し、次のJS instanceが購読した時に再配送する」必要があります。

`muon.push.onMessage()`を通常の一過性event listenerとして実装すると、指摘どおり競合条件が生じます。

## WebView上のJS instanceの扱い

Androidで「インアクティブ」は一つの状態ではありません。

| 状態 | JS instance |
|---|---|
| Activityが`onPause`/`onStop`しただけ | 残っている可能性が高いが、実行やtimerは保証できない |
| Activity再生成 | WebViewを作り直す実装なら消失する |
| WebView renderer kill | 消失する |
| app process kill | 完全に消失する |
| 次回cold start | 新しいpage loadと新しいJS instanceになる |

Androidはbackground processを必要に応じてkillでき、`onDestroy()`が必ず呼ばれる保証もありません。[Android process lifecycle](https://developer.android.com/guide/components/activities/process-lifecycle)

現在のMuon試作では、Activityの`onDestroy()`でWebViewを明示的に`destroy()`しています。[MuonActivity.java](../../../core/android-test/android/app/src/main/java/dev/muon/runtime/MuonActivity.java)

次のActivity生成では新しいWebViewを作り、ページを最初から`loadUrl()`します。[MuonActivity.java](../../../core/android-test/android/app/src/main/java/dev/muon/runtime/MuonActivity.java)

したがって製品契約としては、JS instanceとcallbackは一時的なものとして扱うべきです。background移行後も同じinstanceが残ることは最適化として利用できますが、配送保証の根拠にはできません。

## native inboxを配送の主体にする

必要なのは、次の状態分離です。

```text
Message:
  pending
     |
     v
  leased to document/session
     |
     +-- JS handler成功 -> acknowledged -> 削除
     |
     +-- reload/renderer kill/process kill
             -> lease失効 -> pendingへ戻す

Page:
  absent -> loading -> bridge-ready -> consumer-ready
                                      |
                         pause/stop -> suspended
                                      |
                         reload/destroy -> invalid
```

messageはJS callbackに直接所有させず、nativeの永続inboxをsource of truthにします。

配送手順は次のようになります。

1. provider adapterがmessageを受信する。
2. native inboxへtransactionalに保存する。
3. WebViewがinactiveならnotificationを表示する。
4. page JSが新しいdocument上でhandlerを登録する。
5. JS側からnativeへ`subscribe(documentGeneration)`を送る。
6. nativeがpending messageを現在のsessionへleaseして配送する。
7. handlerの処理完了後にACKする。
8. ACK後にだけinboxから削除する。

message到着が手順4の前でも途中でも、最初にinboxへ入るためロストしません。

## background時にJS instanceが残っている場合

Activityが単に`onStop`しただけなら、既存のcallbackが残っている可能性があります。ただしMuonは、Activityが`RESUMED`でない間は配送を停止した方が安全です。

- background中にmessage到着
  - inboxへ保存
  - notification表示
  - JSへは送らない
- 同じWebView instanceのままresume
  - 既存subscriptionを再activate
  - pending messageを配送
- Activityやrendererが再生成された
  - 古いsubscriptionを破棄
  - 新しいpageが再度subscribeした後に配送

これにより、「callbackは存在するがJS executionがsuspendされている」という曖昧な状態を避けられます。

## `onMessage()`という名前の問題

一般的な`onMessage()`は「登録後に発生したlive eventだけを受け取る」と解釈されやすいため、durable deliveryには少し不向きです。

選択肢は二つあります。

### 1. `onMessage()`をdurable subscriptionとして定義する

- 登録前のpending messageも配送する。
- handlerが返すPromiseの完了をACKとする。
- handlerがthrow/rejectした場合は未処理として残す。
- document消失時の未ACK messageは次のsessionへ再配送する。

簡単ですが、通常のevent listenerより強い意味を持つため、文書化が重要です。

### 2. durable性を名前に表す

概念的には、`subscribeMessages()`や`consumeMessages()`のようなAPIにします。

```javascript
const subscription = await muon.push.consumeMessages(async (message) => {
  await processMessage(message);
});
```

この形なら、次が伝わりやすくなります。

- startup時に毎回consumerを登録する。
- 過去のpending messageも受け取る。
- handler完了までmessageは処理済みにならない。
- 同じmessageが再配送される可能性がある。

現段階では、こちらの方が誤解が少ないと考えます。

## 購読開始時の競合を防ぐ方法

JS側の処理順序も重要です。

1. document-start bootstrapがnative transportを準備する。
2. application codeがcallbackをローカルに登録する。
3. その後nativeへsubscribe要求を送る。
4. nativeはsubscribe成立後にだけmessageを送る。

nativeへsubscribeを送ってからcallbackを保存すると、その間にmessageが返ってくる競合が発生します。したがって「ローカル登録を先、native subscribeを後」に固定します。

native側ではmessageへ単調増加するsequenceを付け、subscribe開始時のpending drainと新規到着を同じdispatcherで直列化すれば、snapshotとlive deliveryの間にも隙間が生じません。

## ACKの意味

ロストを防ぐと、代わりに重複配送の可能性が発生します。

例えば、

1. JS handlerがserverへ更新を送る。
2. 更新は成功する。
3. ACK前にrendererがkillされる。
4. 同じmessageが次回再配送される。

という状態は避けられません。

そのため契約はat-least-onceとし、次を提供します。

- 安定したmessage ID
- delivery attempt
- provider受信時刻
- 明示的なACKまたはasync handler完了ACK
- handler timeout
- retry上限
- TTL
- 必要ならdead-letter/resync-required event

開発者にはmessage IDを使った冪等処理を求めます。

## handlerが登録されない場合

Muonが保証できるのはmessageの保持までです。新しいpageが`consumeMessages()`を呼ばなければ、application固有のJS処理は実行できません。

これは開発者が認識すべき制約です。ただし次の状態にできます。

- messageはTTLまでinboxに残る。
- debug buildでは「pending messageがあるがconsumer未登録」と診断する。
- pending件数を開発者向けdiagnosticで確認できる。
- 次のpage reloadでconsumerが登録されれば再び配送する。
- queue上限到達時は黙って削除せず、collapseまたはresync-requiredへ変換する。

## QuickJS保留後の契約

QuickJSを保留する場合、notification lifecycleは次で確定できます。

- inactive中には開発者JSを実行しない。
- native adapterが受信と永続化を行う。
- 必要ならsystem notificationを表示する。
- notification tapまたは通常resumeでWebViewを利用可能にする。
- pageがconsumerを再登録した後、pending messageをat-least-once配送する。
- JS callbackやJS object自体の生存は保証しない。

つまり、永続化するのはhandlerではなくmessageです。この分離を行えば、WebViewの破棄・再ロードと`onMessage()`登録タイミングの競合があっても、messageのロストは防げます。