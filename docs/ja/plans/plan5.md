# Android FCM push通知 実装計画

本計画は後続作業として保持する。先行するAndroid対応の完成とdevelopへのマージには、[plan6.md](./plan6.md)の範囲と完了条件を適用し、FCMの実装完了を前提にしない。

## 1. 目的と位置付け

この文書は、[plan3-2.md](./plan3-2.md)で合意したAndroid通知配送モデルを、現行codebaseへ実装するための実行計画である。[plan4.md](./plan4.md)の全体計画を置き換えるものではなく、同文書の「ステップ4: push通知のdurable deliveryを実装する」を、公開契約、component、永続schema、TDD、device gate、commit単位まで具体化する。

実装の目的は、providerからAndroid端末へ到達したmessageをActivityやWebViewが存在しない状態でもnative層で永続化し、ページJSが次に安全に実行可能になった時点で取りこぼさず配送することである。providerから端末までの配送はproviderのbest effortとし、native inboxへのcommit後はat-least-onceでページへ配送する。

初回scopeでは、WebViewがinactiveな間に開発者JSを実行しない。QuickJS、Node.js、WorkManager上のbackground JSは通知配送の依存関係へ入れない。

## 2. 現状と着手条件

### 2.1 現状

2026年8月28日時点では、次の状態である。

- Android実装は`muon-android-prototype`内の固定試作hostであり、公開`android` targetではない。
- `MuonActivity`がWebView、RPC bridge、JavaScript runtime bridgeを生成し、Activity破棄時にWebViewとbridgeを破棄している。
- rendererからnativeへのRPCは存在するが、Activity非依存のpush ingress、永続inbox、nativeからrendererへのdurable dispatcherは存在しない。
- Firebase SDK、`FirebaseMessagingService`、`POST_NOTIFICATIONS`、notification channel、`muon.push`は未実装である。
- `muon-android` production backend packageはまだ分離されていない。
- root worktreeにはWindows Settings E2Eの未commit変更が存在する。この計画の実装では、その変更を上書きまたは混在させない。
- `adb`とAndroid SDK Platform 24/37は存在するが、計画作成時点ではVMとPixel 6のどちらも接続されていない。

### 2.2 通知実装へ着手する前のgate

[plan4.md](./plan4.md)の実施順序を維持し、通知のproduction codeを試作hostへ先行追加して後から移植することはしない。次を満たしてから本計画のステップ1へ入る。

1. 現在のWindows Settings E2E変更を、通知作業とは別のGREEN commitとして完了している。
2. root `npm test`が終了code 0である。
3. `plan4.md`のステップ1から3を完了し、`muon-android` production backendとAndroid専用fixtureが分離されている。
4. クリーンな利用者fixtureから公開CLIでdebug APKを生成できる。
5. production artifactに試作page、test bridge、fault injectionが含まれない。
6. x86_64 16 KiB VMとarm64 Pixel 6の既存Android gateがGREENである。
7. 通知実装中の各Android code commitで両device gateを全件実行できる状態である。

着手条件を満たすまでに配置が変わった場合は、以下に示す予定pathを新しいproduction backendの同じ責務へ読み替える。責務と境界を変更して計画と実装が矛盾する場合は、実装を開始せず確認する。

## 3. 初回releaseで固定するscope

### 3.1 含めるもの

- provider非依存の公開namespace `muon.push`
- 最初のprovider adapterとしてFirebase Cloud Messaging
- FCM data messageだけをdurable deliveryの標準経路とする契約
- Firebase Installation IDを使うregistration
- Activity/WebView非依存のapp-private SQLite inbox
- foreground、warm resume、cold start、通常起動、notification tap後のページ配送
- Promise正常完了によるACK、失敗、timeout、reload、kill時の再配送
- Android system notification、channel、small icon、tap用`PendingIntent`
- Android 13以降の`POST_NOTIFICATIONS`状態取得と明示request
- TTL、retry、deduplication、queue上限、overflow、`resync-required`
- fake native event sourceによるprovider非依存の全lifecycle test
- FCM registration、受信、削除通知をnative contractへ変換する薄いadapter test
- push有効時だけFirebase SDK、Service、permission、metadata、resource、inboxを生成するbuild integration
- 日本語・英語の利用者向け文書とAPI reference

### 3.2 初回scopeに含めないもの

- FCM notification messageまたはnotification + data messageのdurable互換mode
- Firebase Web MessagingをWebView内で直接使用する経路
- inactive時のQuickJSまたはNode.js handler
- WorkManagerによる開発者JS実行
- topic購読API、condition配信API、upstream messaging
- notification action button、inline reply、image download、custom layout
- direct boot中の受信とdevice-protected storage
- HMS、APNsなどFCM以外のadapter
- application server、Firebase Admin credential、service accountのclient artifactへの同梱
- exactly-once配送
- Muonによるend-to-end encryption

## 4. FirebaseとAndroid APIの選択

### 4.1 固定version

実装開始時に公式release noteとSDK source commentを再確認したうえで、初回実装では次を固定する。

| 項目 | 固定値 |
|---|---:|
| Firebase Android BoM | `34.18.0` |
| `firebase-messaging` | BoMが解決する`25.1.2` |
| Google services Gradle plugin | `4.5.0` |
| Android Gradle Plugin | 現行固定値`9.2.1`を維持 |
| compile/target SDK | 現行固定値`37`を維持 |
| min SDK | 現行固定値`24`を維持 |

KTX moduleは使用せず、`com.google.firebase:firebase-messaging`のmain moduleを使用する。versionを`latest`や範囲指定にはしない。

参照資料:

- [Firebase Android SDK release notes](https://firebase.google.com/support/release-notes/android)
- [AndroidでFCMを開始する](https://firebase.google.com/docs/cloud-messaging/android/get-started)
- [Androidでmessageを受信する](https://firebase.google.com/docs/cloud-messaging/android/receive-messages)
- [FirebaseMessaging API reference](https://firebase.google.com/docs/reference/android/com/google/firebase/messaging/FirebaseMessaging)
- [FirebaseMessagingService API reference](https://firebase.google.com/docs/reference/android/com/google/firebase/messaging/FirebaseMessagingService)
- [RemoteMessage API reference](https://firebase.google.com/docs/reference/android/com/google/firebase/messaging/RemoteMessage)
- [Android notification runtime permission](https://developer.android.com/develop/ui/views/notifications/notification-permission)
- [Android PendingIntent security](https://developer.android.com/guide/components/intents-filters#DeclareMutabilityPendingIntent)

### 4.2 registrationはFIDを使用する

`plan3-2.md`ではprovider tokenをopaque registrationとして表現していたが、現在のFirebase Android SDKでは`FirebaseMessaging.getToken()`と`onNewToken()`がdeprecatedになり、Firebase Installation IDを使う`FirebaseMessaging.register()`と`FirebaseMessagingService.onRegistered()`が正式経路になっている。

したがって初回実装は次を使用する。

- build時に`firebase_messaging_installation_id_enabled=true`をManifestへ生成する。
- app起動ごとに`FirebaseMessaging.getInstance().register()`を呼ぶ。
- `onRegistered(installationId)`で最新FIDをapp-private storageへ保存する。
- `muon.push.register()`はregistration完了後のFIDをprovider固有形式として解釈せず、opaqueな`value`としてページへ返す。
- 利用者applicationは起動ごとにregistrationをapplication serverへ冪等upsertする。
- FIDの変更をnative側で保存し、次の`register()`で必ず最新値を返す。
- deprecatedなtoken API、Instance ID API、internal APIは使用しない。

初回scopeでは公開`unregister()`を設けない。runtime opt-out、server側registration削除、Firebase auto-initとの関係を別契約として決めてから追加する。

### 4.3 FCM callback内で行う処理

`FirebaseMessagingService.onMessageReceived()`はbackground threadで呼ばれ、公式API commentでは20秒以内の完了が要求される。callback内では、payload検証、短いSQLite transaction、必要なnotification表示、dispatcherへの通知だけを行う。network request、ページJS、QuickJS、長時間処理は行わない。

`onDeletedMessages()`はmessage欠落を黙って無視せず、native inboxへ`resync-required`をcoalesceする。

## 5. 公開契約

### 5.1 Android build設定

公開Android設定では、`push`を省略した状態を無効とする。無効時に空のstubを公開しない。

```ts
interface MuonAndroidPushConfig {
  readonly provider: "fcm";
  readonly googleServicesFile: string;
  readonly notification: {
    readonly channelId: string;
    readonly channelName: string;
    readonly channelDescription?: string;
    readonly importance: "low" | "default" | "high";
    readonly smallIconPath: string;
    readonly defaultTitle: string;
    readonly defaultBody?: string;
  };
  readonly inbox?: {
    readonly ttlSeconds?: number;
    readonly maximumMessages?: number;
    readonly maximumBytes?: number;
    readonly handlerTimeoutMilliseconds?: number;
    readonly maximumAttempts?: number;
  };
}
```

公開API境界なのでoptional fieldを許すが、resolver後のinternal descriptorでは全fieldを必須値に正規化する。

既定値は次に固定する。

| 項目 | 既定値 | validation |
|---|---:|---:|
| inbox TTL | 2,419,200秒（28日） | 60から2,419,200秒 |
| 最大message数 | 1,000 | 1から10,000 |
| 最大payload合計 | 4 MiB | 64 KiBから64 MiB |
| handler timeout | 30,000 ms | 1,000から300,000 ms |
| 最大delivery attempt | 5 | 1から20 |
| retry delay | 1秒、10秒、60秒、300秒 | 初回実装では固定 |

`googleServicesFile`はFirebase Android client設定だけを受理する。次をbuild時に検証する。

- JSONとしてparseできる。
- 対象clientのAndroid package nameが生成application IDと一致する。
- Firebase API key、project ID、application ID、project numberが存在する。
- service accountの`private_key`、`private_key_id`、`client_email`を含むserver credential形式ではない。
- diagnosticとlogへ設定内容、API key、host上の不要な絶対pathを出力しない。

push無効buildでは、Google services plugin、Firebase dependency、FCM Service、push bridge、SQLite inbox、notification resource、`POST_NOTIFICATIONS`、Firebase metadata、`muon.push`型metadataをartifactへ含めない。

### 5.2 FCM data message contract

FCMの送信messageは`notification`を持たないdata-only messageとする。FCM data mapはUTF-8のstring key/valueで、FCM全体の上限である4,096 byte以内とする。Firebaseが予約するkeyに加え、Muonは次を予約する。

| key | 意味 |
|---|---|
| `muon.notification.title` | system notificationのtitle override |
| `muon.notification.body` | system notificationのbody override |
| `muon.notification.action` | tap後にページへ渡すapplication action ID |
| `muon.message.expiresAt` | Unix epoch millisecondsの期限 |

それ以外のkey/valueをapplication dataとしてページへ渡す。未知の`muon.` prefixは将来予約との衝突を避けるためrejectし、diagnosticへ記録する。

期限は次の順で決める。

1. `muon.message.expiresAt`が正しい未来の整数ならその値を候補にする。
2. 設定したinbox TTLによる`receivedAt + ttl`を上限にする。
3. 明示期限がない場合はinbox TTLを使用する。
4. 受信時点ですでに期限切れならapplication messageとして保存せず、invalid payload diagnosticを残す。

FCM transportのTTL、collapse、priorityはproviderから端末までのbest effort配送条件であり、native inboxへcommitした後のretry policyとは分離する。

### 5.3 `muon.push` API

公開型は`ui/muon.d.ts`へJSDoc付きで追加し、Android push有効buildだけruntime objectを生成する。

```ts
interface MuonPushRegistration {
  readonly provider: string;
  readonly value: string;
}

type MuonPushNotificationPermission = "granted" | "denied";

interface MuonPushDataMessage {
  readonly kind: "message";
  readonly id: string;
  readonly provider: string;
  readonly providerMessageId?: string;
  readonly data: Readonly<Record<string, string>>;
  readonly receivedAt: number;
  readonly expiresAt: number;
  readonly attempt: number;
  readonly deliveryReason: "foreground" | "resume" | "notificationTap";
  readonly notificationAction?: string;
}

interface MuonPushResyncMessage {
  readonly kind: "resync-required";
  readonly id: string;
  readonly reason:
    | "provider-deleted"
    | "inbox-overflow"
    | "message-expired"
    | "retry-exhausted";
  readonly receivedAt: number;
  readonly attempt: number;
  readonly deliveryReason: "foreground" | "resume" | "notificationTap";
}

type MuonPushMessage = MuonPushDataMessage | MuonPushResyncMessage;

interface MuonPushConsumer {
  readonly close: () => Promise<void>;
}

interface MuonPushDiagnostics {
  readonly pendingMessages: number;
  readonly leasedMessages: number;
  readonly oldestReceivedAt?: number;
  readonly expiredMessages: number;
  readonly rejectedMessages: number;
  readonly duplicateMessages: number;
  readonly overflowCount: number;
  readonly resyncRequired: boolean;
  readonly notificationPermission: MuonPushNotificationPermission;
  readonly notificationChannelEnabled: boolean;
}

interface MuonPushApi {
  readonly register: () => Promise<MuonPushRegistration>;
  readonly getNotificationPermission: () => Promise<MuonPushNotificationPermission>;
  readonly requestNotificationPermission: () => Promise<MuonPushNotificationPermission>;
  readonly consumeMessages: (
    consumer: (message: MuonPushMessage, signal: AbortSignal) => Promise<void>
  ) => Promise<MuonPushConsumer>;
  readonly getDiagnostics: () => Promise<MuonPushDiagnostics>;
}
```

契約は次のとおりとする。

- `register()`はページloadごとに呼び、返されたopaque registrationをapplication serverへ冪等upsertする。
- 一つのdocumentで許すconsumerは一つだけとし、二重登録はerrorにする。
- callbackをローカルに登録した後でnative subscribeを送る。
- `consumeMessages()`より前に届いたmessageも配送対象にする。
- callbackが返すPromiseの正常完了だけをACKとする。
- throw、reject、timeout、Abort、document破棄、renderer/process killはACKにしない。
- handler timeoutまたはconsumer close時は`AbortSignal`をabortする。handlerがabortを無視した場合もlate ACKは拒否する。
- message IDはnativeが生成する安定したUUIDであり、handlerの冪等keyとして使用する。
- `receivedAt`と`expiresAt`はUnix epoch millisecondsとする。
- `requestNotificationPermission()`はRESUMED Activityが存在する場合だけOS dialogを開始できる。Activity不在時は明瞭なerrorにする。
- permission拒否はprovider受信、inbox保存、次回foreground配送を停止しない。
- callback objectまたはJS instanceの生存は保証しない。

`onMessage()`という一過性listener名は使用しない。過去messageのdrain、lease、ACKを伴うことが分かる`consumeMessages()`を正式名にする。

## 6. 実装architecture

### 6.1 component境界

production backend分離後、概ね次の責務で配置する。Java packageは利用者application IDと分離した安定内部namespace、例えば`dev.muon.android.internal.push`を使用する。

```text
FCM data message / fake source
             |
             v
      MuonPushProviderAdapter
             |
             v
        MuonPushInbox  <---- SQLite / app-private
             |
             +----> MuonPushNotificationPresenter
             |
             v
       MuonPushDispatcher
             |
     Activity RESUMED + bridge ready + consumer ready
             |
             v
         MuonPushBridge
             |
             v
      renderer consumeMessages()
             |
       success ACK / failure NACK
```

予定componentは次のとおりである。

| component | 責務 |
|---|---|
| `MuonPushRuntime` | application process単位のinbox、dispatcher、Activity接続を所有する。Activityを強参照し続けない |
| `MuonPushInbox` | SQLite transaction、schema migration、enqueue、lease、ACK、retry、prune、diagnostic |
| `MuonPushDispatcher` | lifecycle、document generation、pending/live直列化、timeout、delivery reason |
| `MuonPushBridge` | trusted main frameのWebMessage transport、subscribe、ACK、NACK、registration、permission |
| `MuonPushNotificationPresenter` | channel作成、permission確認、notification生成、cancel、tap intent |
| `MuonPushProviderAdapter` | provider eventを正規化するinternal interface |
| `MuonFirebaseMessagingService` | FCM callbackをadapterへ渡す薄いpublic Android Service |
| `MuonFcmProviderAdapter` | `RemoteMessage`、FID callback、deleted callbackの正規化 |
| `MuonPushClock` | production clockとtest用manual clockの境界 |
| renderer push client | `muon.push`公開object、local callback登録、Promise ACK/NACK、AbortSignal |

`MuonRpcBridge`はrenderer起点のRPCとnative plugin runtimeへ結び付いており、Activityが存在しないFCM callbackのsource of truthにはしない。push coreはC++ plugin runtimeとQuickJS Serviceを起動せずに使用できる独立Java componentとする。WebView接続時だけ、既存の`WebViewCompat.addWebMessageListener()`と同じtrusted origin/main-frame検査を使う専用push bridgeをattachする。

### 6.2 SQLite schema

Roomや新しいcode generator dependencyは追加せず、Android公開APIの`SQLiteOpenHelper`を使用する。DBはcredential-protectedなapp-private directoryへ配置し、direct bootでは開かない。

初期schemaは少なくとも次を持つ。

```text
push_messages
  sequence                 INTEGER PRIMARY KEY AUTOINCREMENT
  id                       TEXT NOT NULL UNIQUE
  kind                     TEXT NOT NULL
  provider                 TEXT NOT NULL
  provider_message_id      TEXT NULL
  data_json                TEXT NOT NULL
  received_at_ms           INTEGER NOT NULL
  expires_at_ms            INTEGER NOT NULL
  payload_bytes            INTEGER NOT NULL
  state                    TEXT NOT NULL
  attempt                  INTEGER NOT NULL
  next_attempt_at_ms       INTEGER NOT NULL
  lease_session_id         TEXT NULL
  lease_document_id        TEXT NULL
  lease_token              TEXT NULL
  notification_tag         TEXT NULL
  tapped_at_ms             INTEGER NULL
  notification_action      TEXT NULL

push_registration
  provider                 TEXT PRIMARY KEY
  value                    TEXT NOT NULL
  revision                 INTEGER NOT NULL
  updated_at_ms            INTEGER NOT NULL

push_diagnostics
  key                      TEXT PRIMARY KEY
  value                    INTEGER NOT NULL
```

`provider`とnon-nullな`provider_message_id`にはunique indexを設ける。FCM message IDがnullの場合はprovider deduplicationを行わず、Muon local UUIDだけを発行する。

message状態は次とする。

```text
pending
   |
   | atomic lease: attempt + 1、session/document/tokenを保存
   v
leased
   |
   +-- Promise success + valid lease token -> acknowledged -> delete
   |
   +-- reject/timeout/close/reload/kill
          -> retry待ちpending
          -> attempt上限でresync-requiredへ変換
```

DBを開いたprocess sessionは、過去session IDの`leased` rowをtransaction内で`pending`へ戻す。同じprocess内で新documentがsubscribeした場合は、旧documentのleaseを失効させて回収する。ACKはmessage IDだけでは受理せず、process session、document ID、native発行lease tokenの全てを照合する。

### 6.3 receiveとnotificationの順序

provider callbackは次の順序に固定する。

1. provider messageを公開contractへ正規化する。
2. size、reserved key、期限、provider message IDを検証する。
3. SQLite transactionでmessageとsequenceをcommitする。
4. commit済みmessageについてだけ表示policyを評価する。
5. RESUMEDかつconsumer-readyならdispatcherへ通知し、system notificationは表示しない。
6. consumer-readyでなければpermissionを確認し、許可されていればsystem notificationを表示する。
7. permission拒否時は表示失敗をdiagnosticへ記録するが、messageはinboxへ残す。

persistence失敗時にnotificationだけを表示しない。callback、Intent extras、WebView memory queueをsource of truthにしない。

### 6.4 subscribeと配送順序

rendererとnativeのhandshakeを次に固定する。

1. document-startでprivate push transportを準備する。
2. application codeが`consumeMessages()`を呼ぶ。
3. renderer clientがcallbackとAbortControllerをローカル登録する。
4. renderer clientがランダムdocument IDを含むsubscribeを送る。
5. nativeが現在のprocess sessionとdocumentを関連付ける。
6. Activityが`RESUMED`である場合だけ、pending drainを開始する。
7. subscribeと同時刻の新規arrivalを同じdispatcherへ入れ、SQLite sequenceで直列化する。
8. 一件をleaseし、rendererへ送る。
9. ACK、NACK、timeoutのいずれかが確定するまで次を送らない。

同じdocumentのresumeではconsumerを再activateする。reload、navigation、renderer再生成、Activity再生成では旧documentをinvalidにし、新documentのsubscribeまで配送しない。`pagehide`通知は最適化にだけ使い、OS callbackが来ないことを正しさの前提にしない。

delivery reasonは次に固定する。

- commit時点ですでにRESUMEDかつconsumer-readyなら`foreground`
- 有効なnotification message IDをtapして起動またはresumeした場合は`notificationTap`
- それ以外のcold start、通常起動、warm resume、reload後のdrainは`resume`

tap対象messageは最初に配送し、その後は元のSQLite sequence順へ戻る。tap、dismiss、notification表示、Activity起動はいずれもACKにしない。

### 6.5 retry、expiry、overflow

時間経過をpollingしない。productionでは一つの予定時刻だけをschedulerへ登録し、testではmanual clockとmanual schedulerを注入して決定的に進める。

- attempt 1はconsumer-ready後に即時配送する。
- failure後は1秒、10秒、60秒、300秒の順でretryする。
- attempt 5が失敗したら元messageをdead diagnosticへ計上し、`retry-exhausted`の`resync-required`へ変換する。
- handlerが30秒で完了しなければleaseをtimeout扱いにし、rendererへAbortを送る。
- late ACK、古いdocument、古いsessionからのACKは無視せずrejected ACK diagnosticへ計上する。
- expiryした未ACK messageは黙って削除せず、`message-expired`の`resync-required`へcoalesceする。
- message件数またはpayload byte上限に達した場合は、配送可能な完全差分を保証できないため、未配送message群を`inbox-overflow`の`resync-required`へcoalesceする。
- 進行中のleaseをoverflow処理でACK済みにしない。lease完了後もresync markerを必ず残す。
- 同じreasonのresync markerは一件へcoalesceし、consumerがfull syncを正常完了してACKするまで保持する。
- FCM `onDeletedMessages()`も同じresync経路を使う。

### 6.6 Android notification

- API 26以降は設定から生成した固定channel IDで`NotificationChannel`を作る。
- channel importanceは初回作成後にapplication側から強制上書きしない。
- standard notification templateと`NotificationCompat.Builder`を使用する。
- notification small iconはbuild-time生成resourceを必須とする。
- title/bodyはreserved data keyを優先し、存在しなければ設定のdefaultを使う。
- `PendingIntent`は`MuonActivity`へのexplicit intentとし、`FLAG_IMMUTABLE`を必須にする。
- Intentにはpayloadを入れず、Muon message IDとaction IDだけを入れる。
- message IDごとに一意なdata URIまたはrequest codeを使い、別notificationと`PendingIntent`が意図せず共有されないようにする。
- warm tapは`onNewIntent()`、cold tapは`onCreate()`から同じvalidatorへ渡す。
- 別application、未知ID、期限切れID、ACK済みID、改変IDはページを発火せずdiagnosticへ記録する。
- dismiss検出は配送条件に不要なので、初回scopeではdelete intentを設けない。
- Android 13以降だけ`POST_NOTIFICATIONS`をruntime requestする。それ以前も`areNotificationsEnabled()`とchannel状態を診断する。
- permission requestは自動起動せず、ページ上の明示的な利用者操作から公開APIを呼ばせる。

notification small iconのmaster画像を新規作成または変更する場合は、生成density resourceを必ず目視し、単色mask、余白、透過が想定どおりであることを確認する。testはsource文字列ではなく、生成resourceと実際のnotification表示を検証する。

### 6.7 securityとprocess境界

- push bridgeは設定済みasset originのmain frameだけを受理する。
- navigationで旧documentのconsumer、lease、permission requestを失効させる。
- message IDは推測困難なnative UUIDとし、Intentから任意payloadを注入できないようにする。
- `PendingIntent`はexplicitかつimmutableにする。
- Firebase server credentialをpage、APK/AAB、generated project、logへ含めない。
- `google-services.json`のclient API keyも秘密keyとは扱わないが、不要にlogへ出さない。
- SQLiteはapp-privateだが暗号化storageではない。FCM transportもend-to-end encryptedではないため、機密payloadはapplication側でend-to-end encryptionする必要があることを文書化する。
- QuickJS Serviceを初回artifactへ残す選択をした場合も、`:muon_javascript` processからFirebase registrationを開始しない。FIDが複数processで競合しないことをdevice testする。
- force-stop中のFCM起動、uninstall後、application data消去後、direct boot中の保持は保証しない。

## 7. TDDとcommit単位の実施計画

各code stepは、先に動作を検証するtestを追加してroot全体testと該当する全device gateでREDを確認し、その後に最小実装を行って同じ全testをGREENにする。個別testだけを実行して判定しない。RED状態はcommitせず、testと実装をGREENの同一機能commitへ含める。

既存の未関連変更をcommitへ混在させない。各commit前に`git diff --check`と対象pathを確認する。

### ステップ1: 公開型、設定、生成descriptorを確定する

対象:

- `ui/muon.d.ts`の公開`muon.push`型
- Android target設定型とresolver
- `muon-android` backendの正規化済みpush descriptor
- push無効時のcapability非公開

RED test:

- push省略、正常FCM設定、application ID不一致、不正JSON、server credential形式、各上限値を検証する。
- resolver後にoptional fieldが残らないことを機能として検証する。
- push無効buildのrenderer metadataに`muon.push`が存在しないことを検証する。
- public declarationのJSDocとruntime shapeが一致する既存package testを拡張する。

GREEN条件:

- 公開設定からprovider非依存descriptorを一意に生成できる。
- Firebase固有設定はAndroid backend descriptorのprovider armに閉じる。
- root `npm test`、VM全gate、Pixel 6全gateがGREENである。

commit:

```text
feat: define Android push contracts and configuration
```

### ステップ2: fake sourceとdurable inboxを実装する

対象:

- `MuonPushClock`
- `MuonPushProviderAdapter`
- `MuonPushInbox`
- SQLite schema version 1とmigration test harness
- debug/androidTest専用fake source

RED test:

- ActivityとWebViewを一度も生成せずfake messageをcommitできる。
- process相当sessionを作り直してもrowが残る。
- provider message IDの重複が一件になる。
- null provider IDは別messageとして保存される。
- invalid key、oversize、期限切れpayloadがrejectされる。
- expiry、最大件数、最大byte、provider deletedがresync markerへ変わる。
- wall-clock待ちを使わずmanual clockでTTLとretry予定時刻を検証する。
- schema作成と将来migration失敗時のfail-closedを検証する。

GREEN条件:

- commit、lease、ACK、NACK、reclaim、diagnosticがtransactionで整合する。
- persistenceより先に外部表示またはdelivery callbackを行うpathがない。
- root `npm test`、VM全gate、Pixel 6全gateがGREENである。

commit:

```text
feat: add a durable Android push inbox
```

### ステップ3: page consumerとlifecycle dispatcherを実装する

対象:

- renderer push client
- `MuonPushBridge`
- `MuonPushDispatcher`
- `MuonActivity`の`onResume()`、`onPause()`、`onNewIntent()`、破棄接続
- document generationとlease token

RED test:

- local callback登録より前にnative subscribeを送らない。
- subscribe前pendingとsubscribe同時arrivalをsequence上取りこぼさない。
- 一document一consumerを強制する。
- ActivityがRESUMEDでない間はページへ送らない。
- foregroundでは即時配送する。
- Promise successだけで削除する。
- reject、timeout、consumer close、reload、Activity再生成、renderer再生成で再配送する。
- old document/sessionのlate ACKを拒否する。
- handler timeoutでAbortSignalをabortする。
- pageがconsumerを登録しない場合もTTLまで保持し、diagnosticで観測できる。

GREEN条件:

- pending drainとlive deliveryが一つのdispatcherで直列化される。
- callbackまたはWebView instanceの生存を配送保証に使用していない。
- QuickJS Serviceがなくても全testがGREENである。
- root `npm test`、VM全gate、Pixel 6全gateがGREENである。

commit:

```text
feat: deliver Android push messages to page consumers
```

### ステップ4: system notificationとpermissionを実装する

対象:

- `MuonPushNotificationPresenter`
- notification channel/resource生成
- notification permission API
- tap intent validator
- test fixture用notification probe

RED test:

- background受信ではinbox commit後にnotificationが表示される。
- foreground consumer-readyではnotificationを表示せずページへ配送する。
- permission拒否でもmessageを保持して次回foreground配送する。
- notification dismiss後の通常起動でmessageを配送する。
- cold tap、warm tap、通常起動のdelivery reasonを区別する。
- tapはACKにならず、consumer success後にだけ削除する。
- invalid、期限切れ、ACK済み、改変message IDを拒否する。
- channel無効状態をdiagnosticへ反映する。
- Android 13以降のpermission request結果をcallbackで確定し、sleepやUI pollingに依存しない。

GREEN条件:

- notificationにpayload全体が入っていない。
- explicit immutable `PendingIntent`だけを生成する。
- small iconをVMとPixel 6で目視し、master画像確認記録を残す。
- root `npm test`、VM全gate、Pixel 6全gateがGREENである。

commit:

```text
feat: add the Android push notification lifecycle
```

### ステップ5: FCM adapterとFID registrationを実装する

対象:

- Firebase BoMとGoogle services pluginの固定
- `MuonFirebaseMessagingService`
- `MuonFcmProviderAdapter`
- `register()`と`onRegistered()`の調停
- `onDeletedMessages()`
- ManifestとFirebase metadata

RED test:

- 公開`RemoteMessage` APIからdata map、message ID、sent time、TTLを正規化する。
- notificationを含むmessageをdurable pathとして受理しない。
- `onMessageReceived()`がActivityなしでinboxへcommitする。
- `onDeletedMessages()`が`provider-deleted` resyncを生成する。
- fake registration providerで、register成功、失敗、FID rotation、callback先行/後行を再現する。
- `register()`の戻り値がopaqueで、FCM classまたはfield名を公開型へ漏らさない。
- deprecated `getToken()`、`onNewToken()`、Instance ID APIへの参照がないことをcompile warningとcode reviewで確認する。
- secondary processが存在する構成でもregistrationをmain processからだけ開始する。

GREEN条件:

- `onMessageReceived()`は短いnative処理だけで完了する。
- FCM不在のfake sourceで全lifecycle testが引き続き通る。
- repository testの成否が外部FCM network、Firebase console、実server credentialへ依存しない。
- root `npm test`、VM全gate、Pixel 6全gateがGREENである。

commit:

```text
feat: add the Firebase Android push adapter
```

### ステップ6: production build生成へ接続する

対象:

- `muon-android` Gradle/Manifest/resource template
- push有効時のconditional source/dependency/plugin生成
- client設定copyとvalidation
- npm package内容とclean fixture
- APK/AAB/APKS verifier

RED test:

- push無効APK/AABにFirebase class、FCM Service、push bridge、DB、`POST_NOTIFICATIONS`、Firebase metadataが存在しない。
- push有効artifactだけに必要componentが存在する。
- application ID不一致をGradle実行前に拒否する。
- debug、release APK、AAB由来split APKで同じManifest/resource契約になる。
- npm packしたpackageを別のクリーンfixtureへinstallしてFCM有効buildできる。
- generated projectとlogへserver credentialまたは不要な絶対pathが残らない。
- notification icon、channel、default textが利用者設定から生成される。

GREEN条件:

- 公開CLIだけでpush有効のdebug APK、release APK、AABを生成できる。
- push無効artifactのnegative assertionがGREENである。
- root `npm test`、VM全gate、Pixel 6全gate、clean npm package gateがGREENである。

commit:

```text
feat: generate FCM-enabled Android artifacts
```

### ステップ7: process killと競合のdevice gateを完成させる

対象:

- debug/androidTestだけに存在するfake push entry
- Node.js device gate helper
- deterministic state observation

device matrix:

| 条件 | 期待結果 |
|---|---|
| Activity/WebViewなしでfake受信 | inboxへcommitしnotification表示 |
| foreground受信 | notificationなしでconsumerへ配送 |
| background受信、warm resume | resume後に配送 |
| background受信、notification tap | tap対象を先に`notificationTap`配送 |
| notification dismiss、通常起動 | messageを保持して`resume`配送 |
| Activity recreate | 新document subscribe後に配送 |
| WebView reload | 旧leaseを回収し新consumerへ再配送 |
| commit後のprocess kill | 次回processでleaseを回収して再配送 |
| handler中のprocess kill | 同じmessage ID、増加attemptで再配送 |
| consumer未登録 | 配送せずpending diagnosticを表示 |
| permission拒否 | notificationなし、inbox保持、foreground配送 |
| queue上限 | `resync-required`を配送 |
| invalid tap Intent | inboxを変更せず拒否 |
| QuickJSなし | 同一通知契約を維持 |

process kill testは、messageをcommitした後に外部device harnessからprocessを終了し、再起動後のDB状態を観測する。`force-stop`中にもFCMが起動するという保証をtestしない。固定sleepや状態pollingで成功を推測せず、instrumentation event、process終了、page consumer ACKなどの観測可能な完了条件を待つ。

GREEN条件:

- 上記matrix全件がx86_64 16 KiB VMとarm64 Pixel 6でPASSする。
- release APKとAAB由来split APKでも代表的cold/warm/tap gateがPASSする。
- root `npm test`がGREENである。

commit:

```text
feat: verify durable Android push delivery
```

### ステップ8: 利用者文書を完成させる

日本語版と英語版へ同じ内容を記載する。

- Firebase projectとAndroid app登録
- `google-services.json`の指定とapplication ID一致条件
- `muon.push.register()`を起動ごとに呼びapplication serverへFIDをupsertする方法
- HTTP v1 APIで`fid`をtargetにしたdata-only messageを送る条件
- reserved data keyと4,096 byte上限
- notification permissionを利用者操作からrequestする方法
- `consumeMessages()`、Promise ACK、message IDによる冪等処理
- foreground、resume、tap、dismiss、reload、killの挙動
- provider best effortとinbox commit後at-least-onceの保証境界
- TTL、retry、queue上限、resync-required、diagnostic
- inactive中には開発者JSを実行しないこと
- notification messageがdurable contract外であること
- callbackとJS instanceの生存を前提にしないこと
- FCMがend-to-end encryptedではないこと
- server credentialをclientへ置かないこと

READMEは利用者向けのusageと制約だけを記載し、SQLite schemaやinternal class構造は開発者向け文書へ分離する。

文書commit:

```text
doc: document Android push notification delivery
```

## 8. test実行規則

codeを追加または変更する各ステップで、次を個別testへの置換なしに実行する。

```bash
npm test
npm run test:android --workspace muon-android-prototype
npm run test:android:pixel6 --workspace muon-android-prototype
```

backend分離後にworkspace名やcommandが変わった場合は、同じroot全体test、x86_64 16 KiB Android全test、Pixel 6 Android全testを実行する正式commandへ更新する。test file一件だけ、Vitest suite一件だけ、Gradle task一件だけをPASS根拠にしない。

TDDでは、test追加後に同じ全commandで期待した理由によるREDを確認し、実装後に全commandでGREENを確認する。timeout時は単純にtestをskipせず、観測可能なevent待機へ修正し、それでも正当な長時間処理ならtimeoutを根拠とともに延長する。

最終stepではさらに次を行う。

- npm packしたproduction packageを空のfixtureへinstallする。
- push無効、有効の両fixtureからdebug APK、release APK、AAB、APKSを生成する。
- 両deviceでdebug instrumentation、release APK、AAB由来split APKを実行する。
- `git diff --check`を実行する。
- 外部submoduleとvendor codeに変更がないことを確認する。
- test用component、Firebase test設定、credentialがproduction artifactへ入っていないことを検査する。

実FCM projectとHTTP v1 APIを使うPixel 6 smoke testも一度実施する。これはFID registration、data message受信、notification表示、tap後page deliveryを人間が確認するacceptance evidenceである。ただし外部providerのbest effort、network、credentialをrepository testのPASS条件にはしない。使用したservice accountやaccess tokenはrepository、log、artifactへ保存しない。

## 9. 完了条件

実装完了時に次を一項目ずつ本計画と比較し、満たしていない項目があれば完了扱いにしない。

1. [plan3-2.md](./plan3-2.md)の最終契約どおり、inactive時にJSを実行せずmessageをnative側へ保持する。
2. FCM adapterがActivity/WebView不在時にdata messageをinboxへcommitできる。
3. inbox commitより先にnotification表示またはpage deliveryを行わない。
4. commit後はcold start、warm resume、通常起動、tap、dismiss、reload、Activity/renderer/process再生成をまたいでat-least-once配送する。
5. Promise正常完了だけがACKになり、失敗、timeout、killでは同じIDと増加attemptで再配送する。
6. callback登録とnative subscribe、pending drainとlive arrivalの競合でmessageを失わない。
7. old document/sessionのACKが新しいleaseを削除しない。
8. TTL、retry、deduplication、queue上限、overflow、consumer未登録、resync-requiredが決定的testで検証されている。
9. notification permission拒否、channel無効、tap、dismiss、invalid Intentでもinbox整合性を失わない。
10. provider-managed notification messageをdurable deliveryとして扱わない。
11. FIDベース`register()`と`onRegistered()`を使用し、deprecated token APIへ依存しない。
12. FCM固有型と設定が公開message、consumer、permission APIへ漏れない。
13. push無効artifactにFirebase SDK、Service、permission、metadata、inbox、runtime APIが存在しない。
14. Firebase version、Google services plugin、Android APIの使用法を公式documentとSDK commentの両方で確認し、固定している。
15. server credential、test credential、secret、不要なhost pathがsource、log、artifactへ存在しない。
16. QuickJSまたはNode.jsがなくても同じ通知完了条件を満たす。
17. 日本語・英語のREADMEとAPI referenceが、registration、data message、ACK、冪等性、TTL、queue、保証境界を同じ意味で説明する。
18. root全体test、x86_64 16 KiB VM全gate、Pixel 6全gate、npm package clean fixture gateが終了code 0である。
19. notification small iconを目視し、実機notificationが想定した表示であることを確認している。
20. 外部submodule、vendor code、既存の未関連変更を変更またはcommitしていない。

## 10. 実施順序の要約

```text
plan4 step 0から3をGREENにする
        |
        v
公開契約・設定
        |
        v
fake source + durable SQLite inbox
        |
        v
page consumer + lifecycle dispatcher
        |
        v
system notification + permission + tap
        |
        v
FCM adapter + FID registration
        |
        v
production build/packへのconditional統合
        |
        v
VM/Pixel 6のkill・race・artifact gate
        |
        v
日英文書、最終全test、完了条件照合
```

この順序により、配送保証の核を外部providerから分離した状態でRED/GREENにし、その後FCMを薄いadapterとして接続する。実FCMの不安定性をrepository testへ持ち込まず、同時にproduction artifactでは正式なFCM data messageとFID registrationを利用できる状態を完了条件とする。
