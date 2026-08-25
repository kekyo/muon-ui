# Android製品化計画

## 1. この文書の目的

この文書は、[plan3.md](plan3.md)のAndroid製品化計画を、[plan2.md](plan2.md)に整理した2026年8月25日時点の実装状況と、[plan3-2.md](plan3-2.md)で合意した通知配送設計へ合わせて更新したものである。

最終目的は、固定された試作アプリを増やすことではない。一般のmuon-ui利用者が、公開CLIと公開設定だけを使ってAndroidアプリをbuild、test、pack、署名し、ページ主体のcodeからpush通知を利用できる状態にすることである。

計画は次の三つを分離して扱う。

1. Android WebView backend、Muon API、NDK plugin、APK/AABを正式な`android` targetとして製品化する。
2. providerからnative層へ到達した通知を永続化し、実行可能になったページJSへ取りこぼさず配送する。
3. 現在試作されているQuickJS runtime、または実際のNode.js runtimeを、誤解のない公開契約として採否、製品化する。

Android coreと通知配送の正式化は、JavaScript runtimeの正式採用を待たずに進められる。初回通知機能ではQuickJSによるbackground handlerを保留し、WebViewがinactiveな間に開発者JSを実行しない。初回Android releaseへJavaScript runtimeを含めない場合は、該当APIと設定を明示的に非公開またはunsupportedとし、暗黙のfallbackを設けない。

## 2. plan3から更新した前提

plan3作成後、現在のブランチでは実Node.jsではなくQuickJSを使う限定JavaScript runtimeが追加された。一方、公開targetと製品build/packは未着手のままである。

| plan3の項目                   | 現在の状態                                          | plan4での扱い                                     |
| ----------------------------- | --------------------------------------------------- | ------------------------------------------------- |
| Android公開契約               | 未決定                                              | 最初の決定gateとして維持する                      |
| 試作backendの分離             | 未実施                                              | P0の実装作業として維持する                        |
| `muon build --target android` | 未実装                                              | P0として維持する                                  |
| APK/AABと両ABI                | 試作内では生成済み                                  | 成果を再利用し、公開packへ接続する                |
| production署名                | 未実装                                              | P0として維持する                                  |
| NDK plugin                    | 試作内で両ABI、VM、Pixel 6まで検証済み              | backendへ移植し、公開設定を追加する               |
| Node.js実現可能性gate         | 現行ブランチでは実施せず、QuickJS案へ分岐           | runtime選択gateへ置き換える                       |
| JavaScript project packaging  | 固定test moduleだけ                                 | 利用者projectのbuild-time packagingを新規実装する |
| QuickJS runtime               | 限定module、network、resource limitまで試作済み     | API identityと互換範囲を決めてから製品化する      |
| push通知                      | provider adapter、永続化、公開APIとも未実装         | native inboxとページconsumerをP0で新規実装する    |
| QuickJS通知handler            | 未実装                                              | 初回通知scopeでは保留し、blocking gateにしない    |
| 端末gate                      | 基礎runtimeは記録済み、現行QuickJS 44件は今回未実行 | refactor前後に現行HEADで再実行する                |
| 文書整理                      | 履歴と現状が混在                                    | 公開前の必須作業とする                            |

plan3が示した「最優先は正式target、build、pack、署名、配布物への接続」という結論は変えない。更新点は、Node.jsを単純な次工程とみなさず、すでに存在するQuickJS試作との関係を先に決めることと、通知配送をJavaScript runtimeから独立した初回製品scopeへ加えることである。

## 3. 採用する基本方針

### 3.1 WebView backend

AndroidではCEFを移植せず、AndroidX WebKit上のWebViewをbackendとする。CEF非依存RPC coreとplugin runtime coreを共有し、desktop CEF adapterとAndroid WebView adapterを分離する。

WebView版はCEF版と完全に同じnetwork policyやdesktop window APIを提供しない。互換でない機能をno-opにせず、target固有の型、非公開化、validation errorで境界を表す。

### 3.2 public targetとABI

- 公開target名は`android`とする。
- `android-arm64-v8a`と`android-x86_64`を独立した公開targetにはしない。
- ABIはAndroid設定として扱い、初期既定値は`arm64-v8a`と`x86_64`とする。
- `arm64-v8a`は一般配布と実機、`x86_64`はlocal VM gateに使用する。
- Android SDKを持たない利用者の通常buildを壊さないよう、`allTargets`の暗黙対象へAndroidを含めない。
- Android toolchainを要求するのは、Android targetを明示した場合だけとする。
- `minSdk`は現在検証済みの24を下限とし、未検証値へ下げられる公開設定にはしない。

`targetSdk`、compileSdk、AGP、Gradle、NDK、CMake、bundletoolは、実装時に公式要件と現在の検証結果を確認し、対応matrixとして一組ずつ固定する。試作の現在値を理由なく自動追従させない。

### 3.3 backend package

Android固有のGradle、CMake、Java、AIDL、JNI、resource、toolchain診断を、独立した内部package `muon-android`へ分離する案を採用する。

利用者が操作するCLIは既存の`muon`へ統一し、内部packageを直接操作させない。`muon-ui`内へ直接実装する場合でも、公開CLI層とAndroid backend層の依存方向、入力、出力、errorを同じように分離する。

### 3.4 buildとpackの境界

- `muon build --target android`は、Web asset、正規化済みMuon設定、native library、Android projectを生成し、install可能なdebug APKまで作る。
- `muon pack --target android --type apk`は、配布用APKを生成する。
- `muon pack --target android --type aab`は、store配布用AABを生成する。
- APKSはbundletoolによる端末検証用の派生成果物とし、主要な配布形式にはしない。
- build/packの結果は、desktop distributionとAndroid artifactを判別できるdiscriminated unionにする。
- 結果にはartifact path、variant、ABI、application ID、version、署名状態を含める。

Android targetへdesktop launcher、CEF target、単一runtime directoryを無理に割り当てない。現在の`MuonTargetDescriptor`と`MuonBuildTargetResult`を、共通fieldとplatform固有fieldへ分ける。

### 3.5 pluginのsecurity境界

現在のbuild-time registry方式を正式版にも採用する。

- build開始時にplugin一覧、sourceまたはartifact、ABI、allow、configを確定する。
- packageへ含めたpluginだけを起動時に登録する。
- 起動後の任意path探索、download、未登録native codeのloadを行わない。
- 未登録名、ABI不一致、entry point欠落、16 KiB非整列をbuild時に説明可能なerrorとする。
- desktop用の`plugin.path`、runtime signature、saltをAndroidへ転用しない。

初回公開範囲は、Muonが提供し両ABIを検証できる既知pluginを必須対象とする。利用者sourceのcross buildとABI別prebuilt libraryは、入力の信頼境界、license、NDK ABI、16 KiB対応を検証できた後に追加する。

### 3.6 WebView network

WebView自身の通常networkは包括的にinterceptせず、Android Manifest、実行時permission、Network Security Config、WebView、CSPへ委ねる。

ただし、次のMuon固有境界は維持する。

- 構成済みasset hostはMuonがfail-closedで処理する。
- 不明asset、未登録path、不正methodを通常networkへfallbackさせない。
- Muon RPCは完全一致する信頼済みasset originのmain frameからだけ受理する。
- CEF版の`network.allow`、`network.authorizedOrigin`、`network.localAccess`がAndroidでは同じ保証を持たないことをvalidationと文書で示す。

### 3.7 JavaScript runtime

現行の`muon.node.createNode()`はQuickJSを返し、実Node.js互換ではない。この名前のまま正式公開しないことを推奨する。

初回releaseでは、次のいずれかを公開契約として選ぶ。

1. QuickJSをAndroid固有の限定runtimeとして、Node.jsを名乗らない新しいnamespaceと型で公開する。
2. 限定Node.js互換runtimeとして、対応module、非対応機能、versioning、feature detectionを明示し、`createNode`という名前を維持する。
3. QuickJSを初回releaseから除外し、実際のNode.jsを別途検証、統合するまでNode APIを非対応にする。

推奨は1である。現在の実装を再利用しつつ、Node.js package、CommonJS、npm、native addon、V8などを期待させない契約にできる。後方互換性を考慮する公開release前では、試作API名を維持する理由はない。

API名と設定keyは公開契約stepで確定する。仮に`muon.javascript.createRuntime()`と`javascript.project`を候補としても、仕様決定前に実装へ固定しない。実Node.jsを選ぶ場合は、後述の独立gateを先に通す。

[plan3-2.md](plan3-2.md)で保留したQuickJS対応は、通知のbackground handlerへ適用する。一般用途JavaScript runtimeとしての採否は3.7の独立gateとして維持し、通知配送とは結び付けない。QuickJSを一般用途runtimeとして採用した場合でも、初回通知機能ではinactive時のbackground JS実行を提供しない。background handlerは、native inboxからページJSへの配送が完成した後に、別の公開契約、resource制限、lifecycle gateを伴う後続課題として検討する。

### 3.8 push通知

公開ページAPIはprovider非依存の`muon.push`とし、最初のnative adapterはFCMとする。registrationはopaqueな値としてページへ返し、利用者applicationは通常のnetwork APIを使ってapplication serverへ登録する。provider固有の型、設定構造、token lifecycleを公開ページAPIの契約へ漏らさない。

Muonが保証するのはproviderから端末までの配送ではなく、native adapterがmessageをapp-privateな永続inboxへcommitした後の配送である。保証境界は次のとおりとする。

- providerからnative adapterまではproviderのbest effortとする。
- native inboxへのcommit後は、ページJSへat-least-onceで配送する。
- JS consumerの正常完了をACKとし、ACK後は通常再配送しない。
- consumer実行中にrendererまたはprocessが失われた場合は、未ACK messageを次回再配送する。
- exactly-onceは保証せず、安定したmessage IDを使った冪等処理を利用者契約にする。

端末のforce-stop、network断、provider TTL、providerまたはOSによるdropなど、adapterがmessageを受け取る前の事象はこの保証外とする。native background componentが存在することを、provider配送やprocess起動の保証とは表現しない。

WebView上のJS instance、callback、objectの生存は保証しない。Activityが`RESUMED`であり、bridgeとページconsumerの両方がreadyになった場合だけ配送する。inactive時はnative adapterが受信、永続化、必要なsystem notificationの表示までを担当し、notification tapまたは通常resume後にページがconsumerを登録してからpending messageを配送する。

一過性のlive eventと誤認しやすい`onMessage()`ではなく、登録前のpending messageも消費するdurable subscriptionとして`muon.push.consumeMessages()`を初回公開APIとする。callbackをlocalに登録してからnative subscribeを開始し、handlerが返すPromiseの正常完了をACKとする。throw、reject、timeout、document消失ではACKせず、同じmessageを再配送できるようにする。

FCMでは、native adapterがnotification表示前にmessageを保存できるdata messageをdurable deliveryの標準経路とする。providerが直接表示するnotification messageを対応範囲へ含める場合は、notification tap時にだけpayloadを取得でき、dismiss時やtap前の永続化を保証しない互換modeとして、標準経路と異なる契約を明示する。

## 4. 再利用する完了済み基盤

次は再検討から始めず、現行testを移植後も維持する。

- CEF非依存RPC hostとplugin runtime core
- Android WebViewのtext/binary message transport
- Promise、error、cancel、callback、plugin proxy、64 bit値、binaryのcodec
- 信頼済みasset originとmain-frame検査
- browser、environments、filesystemのAndroid実装
- Android設定の拒否、警告primitive
- cardio 1.1.0のAndroid main Looper統合
- tra-ffic 1.0.0とlibffi 3.8.0のAndroid利用
- x86_64 16 KiBページ用のmuon所有libffi patch queue
- 両ABIのnative dependency buildとmanifest
- build-time Android plugin registry
- debug/release APK、release AAB、APKS生成
- ELF machine、entry point、DT_NEEDED、16 KiB alignment、package内容のverifier
- 16 KiB x86_64 VMと4 KiB Pixel 6を厳密に識別するdevice gate
- QuickJS 2026-06-04のhash固定取得、別process Service、複数runtime、resource limit
- QuickJSの限定module、filesystem、DNS、TCP、HTTP/HTTPS、loopback server実装

これらのcodeをcopyして二重管理しない。正式backendへ移し、旧試作はintegration fixtureへ縮小するか、移行完了後に削除する。

## 5. 課題A: baselineと公開契約

### 5.1 全体テストbaselineをGREENにする

今回の`npm test`ではAndroid workspaceがPASSした一方、既知のWindows Settings uninstall E2E 1件がmenu item待機でtimeoutした。

実装作業は常にrepository全体テストを実行してGREENを確認する必要があるため、Android変更へ着手する前に、このE2Eを時間待ちではない確実な完了条件へ修正する。Android成功条件から隠してskipするのではなく、別の再現test、修正、commitとして扱う。

完了条件は次のとおりである。

1. Windows Settings uninstall E2Eの失敗条件を再現するtestがREDになる。
2. menu表示、selection、uninstall開始、完了を観測可能な状態で同期する。
3. root `npm test`が終了code 0になる。
4. Android関連test結果が変わらない。

### 5.2 現行Android device baselineを確定する

refactor前の現行HEADに対して、次を記録する。

1. `x86_64`、Android 17/API 37、16 KiB local VMでinstrumentation 44件、release APK、AAB由来split APKを実行する。
2. VM gate完了後、利用者が手動接続したPixel 6で`arm64-v8a`、4 KiBの同じgateを実行する。
3. QuickJSのmodule、network負荷、resource limit、Service kill/restartを両端末で確認する。
4. test件数、端末property、page size、artifact hash、結果を記録する。

端末が利用できない場合でも他の設計、unit test、artifact調査は進められるが、baseline完了を偽装しない。device gateが必要な実装stepの完了判定は端末接続後まで保留する。

### 5.3 初回release scopeを決める

次を一つの公開契約文書として決める。

- `android` targetとABIの表現
- build、APK、AAB、APKSの境界
- 初回対象のAndroid/API、ABI、WebView provider条件
- JavaScript runtimeを初回releaseへ含めるか
- QuickJSを含める場合のAPI identityと互換範囲
- push registration、durable consumer、message、ACKの公開型
- FCM data messageを標準経路とするprovider境界と、provider-managed notificationをdurable delivery対象外とする契約
- inboxのTTL、上限、retry、overflow、diagnostic、resync契約
- inactive時には開発者JSを実行せず、JS instanceの生存を保証しないlifecycle契約
- NDK pluginの初回入力方式
- WebView networkとpermissionの契約
- production署名と秘密情報の入力方式
- 意図的に非対応とするdesktop API

完了条件は、公開型、設定例、CLI例、成果物、warning、error、非対応条件を、実装testが参照できる仕様として保存することである。

## 6. 課題B: Android backendの分離

### 6.1 package構成

`muon-android-prototype`のproduction相当codeを`muon-android`内部packageへ移し、新規のpush通知componentも同じbackend境界へ配置する。

分離対象は次のとおりである。

- Gradle Wrapper、settings、application module template
- AndroidManifest、resource、Network Security Config template
- Activity、platform service、filesystem service
- push provider adapter、永続inbox、notification表示、tap/resume dispatcher
- JavaScript runtime Service、AIDL、bridge
- JNIとCMake定義
- WebView RPC adapterとAndroid API adapter
- native dependency recipeとverifier
- plugin registry generatorとverifier
- QuickJS取得、license、package処理
- bundletool準備とartifact検査
- device gate helper

試験用page、test plugin、fault injection、instrumentation codeはfixture側へ残し、production templateへ混入させない。

### 6.2 固定値とrepository依存を除く

- `dev.muon.prototype`を安定したMuon内部namespaceと生成application IDへ分ける。
- 利用者application IDごとにJNI symbol名を生成しない。固定内部classまたは`RegisterNatives`を使う。
- repository rootを上方向へ探索するpathをbackend APIの明示入力へ置き換える。
- `muon-core`、cardio、tra-ffic、QuickJS、template、patch、licenseをnpm package内で解決できる配置にする。
- Vite `dist`、plugin source、生成registry、native dependency出力のpathをbackend入力として型付けする。
- build cache keyへtoolchain version、source hash、patch hash、ABI、minSdkを含める。
- errorと一時directoryに秘密情報やhost固有pathを不要に残さない。

### 6.3 backend API

backend APIは少なくとも次を受け取る。

- 正規化済みAndroid metadata
- Web asset directory
- 正規化済みMuon runtime config
- ABI一覧
- plugin descriptor一覧
- push通知descriptorまたは無効指定
- JavaScript runtime descriptorまたは無効指定
- variantとartifact type
- signing reference
- output root
- toolchain discovery結果
- progress callback

戻り値はartifact path、variant、ABI、application ID、version、署名状態、生成project path、diagnosticを含む。CLI文字列のparseやconsole出力をbackendの責務にしない。

### 6.4 分離の完了条件

1. backend packageのAPIから現在の試作相当アプリを生成できる。
2. production codeが`muon-android-prototype`やrepository rootの暗黙pathを参照しない。
3. npm packしたbackendを別のクリーンfixtureへinstallしてbuildできる。
4. test用fault、test plugin、debug probeがrelease templateへ入らない。
5. 分離前後でhost test、VM gate、Pixel 6 gateの意味上の結果が一致する。
6. ActivityやWebViewを生成せずに起動できるpush受信componentとinboxをproduction templateへ配置できる。
7. 移行後に不要となる試作helperと重複templateを削除する。
8. 全体テストがPASSする。

## 7. 課題C: public target、設定、成果物型

### 7.1 target型

現在のtarget型はCEF、desktop launcher、runtime directoryを必須とする。共通target情報とplatform固有情報へ分離する。

Android descriptorに必要な情報は次のとおりである。

- target kind `android`
- ABI一覧
- minSdk、targetSdk、compileSdk
- build variant
- application ID、namespace
- versionCode、versionName
- artifact type
- signing mode
- Android backend package/version

desktop resultとAndroid resultはdiscriminated unionにし、Android resultへ存在しない`launcherPath`をoptional fieldの集合で表さない。

### 7.2 Android設定

最低限、次を型付き公開設定として定義する。

- `applicationId`
- `namespace`
- application label
- `versionCode`
- `versionName`
- `minSdk`
- `targetSdk`
- `abis`
- `permissions`
- 通常iconとadaptive icon
- theme、splash、background color
- network security設定
- signing設定へのreference
- Android plugin一覧
- push通知の有効化、provider設定、channelと表示既定値
- JavaScript runtimeの無効、有効、project設定

application IDとnamespaceはJava packageとして検証する。versionCodeはAndroidの整数制約を検証し、単調増加が配布者の責任であることを文書化する。

permissionは、明示設定と有効機能から導出した設定を区別する。危険permissionをtemplateへ一律追加しない。`ACCESS_LOCAL_NETWORK`が必要な構成では、ManifestだけでなくActivity側の説明と実行時requestを公開契約へ含める。pushまたはsystem notification表示に必要なManifest、runtime permission、Service宣言、provider metadataも、有効なprovider設定からだけ導出する。

pushは既定で無効のopt-in機能とする。FCMを有効にした場合だけclient設定を要求し、application IDとの対応をvalidationする。providerのserver credentialをclient設定として受理しない。

### 7.3 設定の合成とvalidation

既存の`package.json`、`muon.json`、Vite設定、CLI引数の優先順位を決め、同じresolverをbuildとpackで使用する。

- Androidで対応する値は正規化してbackendへ渡す。
- desktop専用値は無視せずerrorにする。
- policyを再現しないnetwork設定など、受理するが意味が異なる値はwarningにする。
- `browser.window`相当の設定は、Androidでの変換、別設計、errorをfieldごとに決める。
- `getConfigValues()`へ渡す値は、試作固定値ではなく合成済みconfigから生成する。
- Vite virtual moduleとpackage後の実dataが同じ意味になるようにする。
- pushを無効にしたbuildではprovider SDK、Service、receiver、permission、metadata、inboxを含めない。
- JavaScript runtimeを無効にしたbuildでは、該当APIとpermissionとnative libraryを含めない。

### 7.4 toolchain診断

Android targetを実行する前に、次を診断する。

- 対応JDK
- Android SDK rootと必要platform
- Build Tools
- Android NDK
- CMakeまたはNDK CMake toolchain
- Gradle Wrapper
- bundletool
- adb
- 指定したVMまたはdevice

不足時はGradle/CMake内部errorへ進む前に、期待version、検出値、修正方法を示す。自動downloadを行うものはversion、URL、SHA-256、cache、licenseを固定する。

### 7.5 課題Cの完了条件

1. `android`が公開型、CLI、Vite optionで受理される。
2. Androidを指定しないdesktop buildの型と挙動が変わらない。
3. 設定priority、default、正常値、不正値、warningを機能testで検証する。
4. Androidとdesktopの成果物を型安全に判別できる。
5. toolchain不足をAndroid指定時だけ明瞭に診断する。
6. pushの有効、無効、provider設定、notification permission導出を型と生成artifactで判別できる。
7. 全体テストがPASSする。

## 8. 課題D: 利用者projectとpluginのpackage

### 8.1 Web assetとMuon設定

- Vite buildを一度だけ実行し、生成assetをAndroid assetへ配置する。
- `browser.startPage`とasset originを正規化済み設定から生成する。
- 構成済みasset host全体をfail-closedで処理するrouteを生成する。
- 不明asset、path traversal、不正methodが外部networkへ到達しないことをtestする。
- application label、icon、theme、splash、permission、Manifest要素を利用者設定から生成する。
- fixed test pageとdebug bridgeをproduction artifactへ含めない。

### 8.2 plugin

- 公開plugin descriptorから一つの正規化済みregistryを生成する。
- JavaScript名、native登録名、soname、allow、configを一意にする。
- 両ABIに同じplugin集合を要求する。
- known pluginはbackend package内のsourceまたはartifact provenanceを検証する。
- source pluginを許可する段階ではC++20、公開Muon plugin API、license、NDK build flagを固定する。
- prebuilt pluginを許可する段階ではELF machine、API、STL、DT_NEEDED、16 KiB、entry point、licenseを検査する。
- registryにないlibraryがAPK/AABへ入らず、package内libraryをruntimeが勝手に列挙しないことをtestする。

### 8.3 課題Dの完了条件

1. クリーンな利用者fixtureのVite assetとMuon設定をAndroid appへ組み込める。
2. 利用者metadataからManifestとresourceを再現可能に生成できる。
3. 登録pluginが両ABIで動き、未登録pluginが安全に拒否される。
4. production artifactへ試作固有ID、page、bridge、fault injectionが入らない。
5. VMとPixel 6でcold start、代表的組み込みAPI、plugin RPCがPASSする。
6. 全体テストがPASSする。

## 9. 課題E: push通知のdurable delivery

この課題では、providerから端末へ配送されたmessageをnative層で受信して永続化し、ページJSが次に安全に実行可能になった時点で取りこぼさず配送する。WebViewの非稼働中に開発者JSを実行することは完了条件に含めず、QuickJSの採否やlifecycleから独立して実装する。

### 9.1 providerと公開APIの境界

- 公開namespaceはprovider非依存の`muon.push`とする。
- 最初のnative provider adapterはFCMとし、adapterを差し替えてもpage consumerとmessage型を変えない。
- `muon.push.register()`はproviderのregistrationをopaqueな値として返し、利用者applicationが通常のnetwork APIでapplication serverへ登録する。
- registrationの初回取得、更新、無効化、再登録時にapplication serverを更新できる契約を決める。
- notification表示permissionの状態取得とrequestを`muon.push`のprovider非依存APIとして定義し、拒否後もmessage受信とinbox配送を可能な範囲で継続する。
- providerのserver credentialをpage、APK/AAB、logへ含めない。
- registration、permission、consume APIは他のMuon RPCと同じ信頼済みasset originのmain frameだけに公開し、navigationで古いdocumentの権限とsubscriptionを失効させる。
- 実装時点の公式documentとSDK API commentを確認してFirebase Android SDKとbuild pluginのversionを固定し、deprecated、internal、unsafeなAPIへ依存しない。
- durable consumerは`muon.push.consumeMessages(async (message) => ...)`とし、登録前のpending messageも対象になることを名前、型、JSDocで表す。
- 同じdocumentで許すconsumer数、解除方法、再登録時の挙動を決め、一つのmessageを複数の一過性listenerへbroadcastするAPIにはしない。

公開messageには少なくとも次を含める。

- Muonが発行する安定したmessage ID
- application payload
- provider名と、存在する場合だけprovider message ID
- provider adapterが受信した時刻
- delivery attempt
- delivery reasonとnotification action
- expiry

provider payloadの型、最大size、文字encoding、予約field、不正payloadの拒否とdiagnosticをversioned contractにする。providerまでの到達はbest effort、native inboxへのcommit後はat-least-onceという保証境界をREADMEとAPI referenceの両方に記載する。

### 9.2 native inboxとmessage状態

nativeのapp-privateな永続inboxをsource of truthとし、callback、Intent extras、WebView内queueだけにmessageを保持しない。provider callbackでは、system notificationの表示やページ配送より先にmessageと単調増加sequenceをtransactionalに保存する。

message状態は次のように扱う。

```text
pending
   |
   v
leased to document/session
   |
   +-- JS consumer成功 -> acknowledged -> 削除
   |
   +-- throw/reject/timeout/reload/renderer kill/process kill
           -> lease失効 -> pendingへ戻す
```

- leaseにはdocument generationとnative session generationを記録し、古いdocumentや再生成前のprocessからのACKを受理しない。
- app processが正常なshutdown callbackなしにkillされても、次回起動時にleased messageを回収できる状態遷移にする。
- ACKをcommitした後だけmessageを削除する。notificationの表示、tap、dismissはACKにしない。
- `PendingIntent`にはpayload全体ではなくmessage IDを格納し、tap時はinboxの正規化済みmessageを参照する。
- persistenceが失敗したmessageを配送済みまたは表示済みとして扱わず、local diagnosticを残す。providerが再配送するとは保証しない。
- storageの保証範囲はprocess kill、Activity再生成、renderer reload、通常のapp updateまでとし、uninstallまたはapplication data消去後までの保持は保証しない。

### 9.3 page lifecycleと購読handshake

ページ状態は次のように扱う。

```text
absent -> loading -> bridge-ready -> consumer-ready
                                    |
                       pause/stop -> suspended
                                    |
                       reload/destroy -> invalid
```

配送条件はActivityが`RESUMED`で、現在のdocument generationに対応するbridgeとconsumerがreadyであることとする。backgroundに残ったWebViewとcallbackは最適化には利用できるが、生存を保証せず、配送保証の根拠にしない。

購読開始は次の順序へ固定する。

1. document-start bootstrapがnative transportを準備する。
2. application codeがcallbackをページ内へ登録する。
3. callback登録後に、document generationを含むsubscribe要求をnativeへ送る。
4. nativeはsubscribe成立後にだけpending messageと新規messageを同じdispatcherから配送する。

callback登録前にnativeへsubscribeしない。native dispatcherはsubscribe開始時のpending drainと同時刻の新規到着をsequence順に直列化し、snapshotとlive deliveryの間に取りこぼしを作らない。初回scopeでは一つのconsumerへ一件ずつ配送し、現在のhandlerがACKまたはretry判定へ到達する前に次のmessageを実行しない。

- background中にmessageを受信した場合は、inboxへ保存して必要なnotificationを表示し、ページへは配送しない。
- 同じWebViewのままresumeした場合は、現在のsubscriptionを再activateしてpending messageを配送する。
- Activity、renderer、documentが再生成された場合は、古いsubscriptionをinvalidにし、新しいpageがsubscribeするまで配送しない。
- `onCreate()`または`onNewIntent()`でtapを受けても、bridge-readyやconsumer-readyより先にmessageを発火しない。

### 9.4 ACK、retry、queue policy

初回APIではconsumerが返すPromiseの正常完了をACKとする。consumerがthrowまたはrejectした場合、handler timeout、document消失、renderer/process killの場合は未ACKのままにし、lease失効後に再配送する。

JS側の外部副作用が成功してからACK前にprocessが失われる可能性は排除できない。このためexactly-onceを謳わず、安定したmessage IDによる冪等処理を利用者へ要求する。

実装前に次の既定値、上限、設定可能範囲を公開契約として決める。

- handler timeout
- retry上限とattemptの増加条件
- TTLとexpiry後の扱い
- inboxの最大message件数とbyte数
- 重複provider messageのdeduplication範囲
- retry上限、TTL、queue上限到達時のdead-letter、collapse、resync-requiredの扱い

上限到達時に古いmessageを黙って削除しない。application固有の完全な差分配送を継続できない場合は、consumerが全状態を再同期できる`resync-required`相当の状態へ変換する。

pageがconsumerを登録しない場合は、messageをTTLまでinboxへ保持する。debug buildではpending messageがあるのにconsumerが未登録であることを診断し、pending件数、最古受信時刻、retry、expiry、overflow状態を開発者向けdiagnosticから確認できるようにする。

### 9.5 system notificationとFCM経路

FCMの標準経路はdata messageとし、Muonのnative adapterがinboxへのcommit後、lifecycleと利用者設定の表示policyに従ってsystem notificationを表示する。これによりnotificationをdismissした場合も、messageはACKされるまで次回foreground配送の対象に残る。

- notification channel、表示既定値、small icon、tap actionを利用者設定から生成する。
- notification表示permissionが拒否された場合も、provider受信とinbox保存が可能ならmessageを保持し、次回foregroundで配送する。表示できなかった理由はdiagnosticとpermission APIから確認できるようにする。
- notification tapはmessageを優先表示する契機にはできるが、tapだけを配送やACKの唯一の契機にしない。
- notification actionとIntentのmessage IDを検証し、別application、期限切れ、削除済み、改変されたIDを安全に拒否する。
- notificationをtapしない通常起動、warm resume、cold startのいずれでも、consumer登録後にpending messageを配送する。

provider-managed notification messageは、tap前にMuon inboxへ保存できずdismiss時にはページへ配送できないため、初回releaseのdurable delivery契約へ含めない。application serverにはdata messageの送信を要求する。将来互換modeを追加する場合は、tap時だけ取得できる別のdelivery capabilityとして公開型、warning、文書で標準経路と区別する。

### 9.6 通知機能の完了条件

1. ActivityとWebViewが存在しない状態で、決定的なfake native event sourceからmessageを受信し、inboxへcommitできる。
2. commit済みmessageがcold start、warm resume、通常起動、notification tap、notification dismiss、Activity再生成、renderer reload、process killを経てもconsumer登録後に配送される。
3. callbackのlocal登録とnative subscribeの競合、pending drain中の新規到着を再現するtestがREDになり、修正後はsequence上の取りこぼしなくGREENになる。
4. Promise正常完了時だけACKして削除し、throw、reject、timeout、古いgenerationからのACK、killでは同じIDと増加したattemptで再配送する。
5. consumer未登録、TTL、retry上限、queue上限、deduplication、resync-requiredを、時間待ちに依存しないclockと状態観測で検証する。
6. notification permission拒否、tap、dismiss、無効または期限切れのIntentでもinboxの整合性を失わない。
7. FCM data message adapterを統合し、provider依存部分を使わないfake sourceでlifecycle test全件を再現できる。外部providerのbest effort配送をrepository testの成否条件にしない。
8. page API、message型、registration、保証境界がprovider非依存であり、FCM固有設定はAndroid backendに閉じている。
9. inactive時に開発者JSを実行せず、QuickJS Serviceが存在しない構成でも同じ配送契約を満たす。
10. VMとPixel 6のnotification lifecycle gateおよびrepository全体テストがPASSする。

## 10. 課題F: QuickJS runtimeの製品化

この課題は、3.7の選択でQuickJSを一般用途runtimeとして正式対象にした場合だけ実施する。初回Android releaseから除外する場合は、該当API、Service、QuickJS、module asset、network permissionをartifactへ含めないことをtestする。採用する場合も課題Eの通知配送には使用せず、inactive時のpush background handlerは後続scopeとして保留する。

### 10.1 公開identityとcapability

- runtimeがQuickJSでありNode.jsではないことを型とruntime情報で取得できるようにする。
- 対応moduleと機能flagをmachine-readableに返す。
- Node.js versionを偽装しない。
- moduleごとの対応export、option、limit、error codeをversioned contractにする。
- 一覧外moduleはmodule-not-found、未対応optionは明瞭なerrorにする。
- 現行`muon.node.createNode()`を変更する場合は、正式公開前にtestとcodeから旧名を除く。

### 10.2 利用者JavaScript projectのpackage

現在の固定`backend.mjs`を、利用者projectのbuild-time pipelineへ置き換える。

- entry pointとdependencyを開発host上でbundleまたは収集する。
- Android端末上でpackage managerを実行しない。
- ESMと対応組み込みmoduleだけを初期対象にする。
- dynamic import、asset、source map、WASMの対象範囲を決める。
- CommonJS、`require`、native addon、Node-API、runtime package探索をbuild時に拒否する。
- module graph、content hash、licenseをmanifest化する。
- incompleteまたは古いassetをruntimeが再利用しない更新方式を定義する。
- writable data、cache、temporary directoryをapp private storageへ分離する。

### 10.3 filesystemとnetwork

- filesystemは現在のprivate virtual rootを既定とする。
- `content://`とshared storageをpathへ偽装しない。
- Storage Access Frameworkが必要になった場合は別APIとpermission leaseを設計する。
- network moduleを有効にした構成から`INTERNET`を導出する。
- LANを許可する構成では`ACCESS_LOCAL_NETWORK`、実行時request、拒否後のerrorを統合testする。
- HTTPS certificate検証を必須とし、検証無効化optionを追加しない。
- server listenは初期版ではloopbackだけに制限する。
- CEF版network policyがQuickJS codeへ適用されないことを公開文書へ明記する。

### 10.4 lifecycleと障害処理

次を状態遷移としてtestする。

- cold start
- 複数runtimeの独立性
- renderer reload
- Activity再生成
- foregroundからbackgroundへの移行
- Service bind切断
- Service process killと再生成
- app process kill後のcold start
- runtime release中の未完了timer、DNS、socket、HTTP
- app更新後のmodule asset切替
- resource limit到達後の別runtime継続

UI側とService側でgenerationを持ち、旧processの応答を新しいruntimeへ混入させない。Androidがprocessをkillした場合に正常shutdown callbackが必ず呼ばれる前提を置かない。

### 10.5 security、resource、保守

- QuickJS version、source URL、archive hash、build flag、licenseを固定する。
- CVEとupstream releaseの追従期限を決める。
- heap、stack、連続実行、runtime数、socket、listener、HTTP、body、乱数など現在の上限を公開contractへ含める。
- timeoutとinterrupt後にServiceが再利用できることをtestする。
- runtimeとmodule packageのlicense/NOTICEを成果物へ含める。
- debug diagnosticへ利用者code、secret、certificate private keyを不用意に出力しない。

### 10.6 QuickJS製品化の完了条件

1. API名からNode.js完全互換と誤認しない公開契約になっている。
2. クリーンな利用者ESM projectをAPK/AABへpackageし、VMとPixel 6で実行できる。
3. 対応module、非対応module、option、limitが実装、test、文書で一致する。
4. Activity、renderer、Service、OS kill、app updateのlifecycle testがPASSする。
5. 異常JS、memory limit、network limit後にもServiceと他runtimeが継続する。
6. QuickJSとmodule assetのversion、hash、license、security update手順がある。
7. npm pack後のクリーン環境でも同じartifactを生成できる。
8. 全体テストがPASSする。

## 11. 課題G: 実Node.jsを選ぶ場合の独立gate

実Node.jsが製品要件である場合は、QuickJSをNode.jsの代替とみなさず、plan3の実現可能性gateを独立して実施する。

### 11.1 build gate

実装時点で保守中かつmuon-nodeのversion要件を満たすNode.js版を選び、公式documentとsource内API commentを確認する。

- `arm64-v8a`と`x86_64`向けlibnodeを再現可能にbuildする。
- API Level 24のappから初期化する。
- 4 KiBと16 KiB page、ELF alignment、W^X、JITを検証する。
- source、patch、toolchain、configure flag、artifact hashを固定する。
- Node.js、V8、OpenSSL、ICU、libuvのlicenseとsecurity更新方法を定義する。

古い非保守版へmuon-nodeの要件を下げることを既定の回避策にしない。

### 11.2 execution model

初期候補は、exportしない別process Android Serviceにruntimeを置く方式とする。

- UI processからcrashを分離する。
- Binderはlifecycle制御に限定し、既存`muon-node/1` protocolを運べるtransport境界を維持する。
- singleton、同時1 runtime、複数runtime poolのどれを公開するか決める。
- 複数`createNode`が同じglobalを暗黙共有しない。
- Service kill、rebind、generation、pending request、shutdown timeoutをtestする。

### 11.3 projectとAPI

- pure JavaScript dependencyをbuild hostで収集しAPK/AABへ入れる。
- device上でnpmを実行しない。
- `fs`、timer、DNS、TCP、HTTP/HTTPS、WASMを初期対象として実機検証する。
- `child_process`、worker、signal、inspector、native addonの対応可否を明示する。
- native addonを初期対象外にする場合は`.node`をbuild時に拒否する。
- private storage、`content://`、cache、temporary directoryの意味を定義する。

### 11.4 採用判定

採用する場合は、両ABI build、VMとPixel 6のruntime/RPC/lifecycle、project package、API対応表、license/security保守、公開CLIまでを完了条件にする。

不採用の場合は、再現可能な失敗条件、検証環境、将来再検証する上流条件を記録し、AndroidのNode APIと`node.project`を明瞭に拒否する。

## 12. 課題H: APK/AAB、署名、配布物

### 12.1 artifact

- debug APK
- unsignedまたはproduction署名済みrelease APK
- production署名またはupload keyで署名したrelease AAB
- AABから生成した端末別APKS
- mapping、native symbol、NOTICEなど選択した付随artifact

生成先とfile名へapplication、version、target、variantを含め、古いartifactとの取り違えを防ぐ。build resultは実際に生成したpathだけを返す。

### 12.2 署名と秘密情報

- local debug buildでは標準debug keystoreを使用できる。
- production keyをrepository、npm package、生成templateへcopyしない。
- keystore path、alias、passwordは外部credential sourceから受け取る。
- passwordをcommand line、log、error、result、Gradle cacheへ出力しない。
- unsigned、debug署名、production署名をartifact metadataで判別する。
- AABとPlay App Signingを使う場合はupload keyの責務を文書化する。
- CIで非対話実行でき、local対話入力だけに依存しない。

### 12.3 npm配布物

`npm pack`結果について次をtestする。

- Gradle Wrapper
- Android templateとresource
- Java、AIDL、JNI、CMake source
- push provider adapter、inbox schemaとmigration、notification resource
- provider SDKとbuild pluginの固定version、dependency metadata、license/NOTICE
- native dependency recipeとpatch
- plugin registry generator
- QuickJSを採用する場合はsource recipe、runtime asset、license
- bundletool取得情報
- verifierとdevice helper

クリーンfixtureではrepository submoduleや親directoryを参照せず、npm artifactだけからbuildする。Android toolchain自体を同梱しない場合は、必要versionを診断して利用者環境から解決する。

### 12.4 課題Hの完了条件

1. public CLIからAPKとAABを生成できる。
2. `arm64-v8a`と`x86_64`を含むartifactを検査できる。
3. debug、unsigned、production署名状態を結果から判別できる。
4. secretがrepository、artifact、logへ漏れないtestがある。
5. AAB由来split APKをVMとPixel 6へinstallしてcold startできる。
6. pushを有効にしたartifactには必要なadapter、Service、permission、metadata、inboxだけが入り、server credentialとtest用fake sourceが入らない。
7. npm pack後のクリーンfixtureで同じbuild/packが成功する。
8. 全体テストがPASSする。

## 13. 課題I: 補助APIと文書

### 13.1 初回releaseで非対応とするもの

次をdesktop APIから機械的に移植しない。

- system tray
- desktop launcher/updater
- 任意の外部process
- runtime外部native plugin load
- desktop window位置、任意size、minimize/maximize
- NSIS、debなどdesktop pack形式

Android targetでは非公開にするか、設定時に説明可能なerrorを返す。

### 13.2 後続候補

次は具体的な利用者要件があるものだけを追加する。

- `content://`とStorage Access Framework
- Android file/directory picker
- persistable URI permission
- QuickJSまたは別のheadless runtimeによるpush background handler
- foreground Service
- share intent
- app link/deep link
- in-app update
- task、multi-window、Picture-in-Picture
- notification以外のgeneral-purpose permission request UI

`content://`をfilesystem pathへ変換するfallbackは設けない。URI、file descriptor、permission leaseのlifecycleを別APIとして表す。

### 13.3 文書整理

- [android-api-compatibility.md](android-api-compatibility.md)を両ABI、NDK plugin、push通知、QuickJS採否を含む現在の契約へ更新する。
- [filter-limitation.md](filter-limitation.md)は検証履歴と確定network仕様を明確に分ける。
- 開発者向けにはtoolchain、backend、test gate、diagnosticを記載する。
- 利用者向けREADME日本語版と英語版には、公開CLI、設定、build、pack、署名、install、push通知の利用方法、制約だけを記載する。
- WebView provider依存、network policy差、permission、非対応API、JavaScript runtime identityを両言語で一致させる。
- 通知について、providerとMuonの保証境界、data message標準経路、registration、durable consumer、ACK、冪等性、TTL、queue上限、inactive時にJSを実行しない制約を両言語で一致させる。
- `consumeMessages()`をapplication起動ごとに登録すること、callbackの生存を前提にできないこと、未登録時のdiagnosticと再同期方法を利用者向けに記載する。
- prototype READMEを正式利用手順として流用せず、公開backendの手順へ置き換える。

## 14. テスト戦略

すべてのcode修正は、問題または要求を再現するtestを先に追加してREDを確認し、最小修正でGREENにする。個別testだけで完了せず、各変更でrepository全体の`npm test`を実行する。

### 14.1 host層

1. target、設定、成果物unionのunit test
2. Android backend入力、生成project、diagnosticのunit test
3. clean利用者fixtureからのbuild/pack integration test
4. npm pack後の別fixture test
5. SDKなし、version不一致、ABI不一致、署名不備のfailure test
6. push設定、provider adapter正規化、inbox状態遷移、lease、ACK、retry、TTL、overflowのtest
7. native dependency、QuickJS、plugin、ELF、APK/AAB/APKS verifier
8. desktop target全体の回帰test

### 14.2 Android device層

1. 16 KiB `x86_64` Android 17/API 37 VM
2. 4 KiB `arm64-v8a` Pixel 6
3. 利用可能な場合は16 KiB `arm64-v8a`環境

各必須deviceでは次を実行する。

- debug instrumentation全件
- release APK install、cold start、page-ready
- AAB由来split APK install、cold start、page-ready
- asset 404/405のfail-closed
- trusted main-frame RPCと拒否経路
- browser、environment、filesystem
- plugin scalar、binary、callback、proxy、cancel、lifecycle
- fake native event sourceによるforeground、background、tap、dismiss、resume、consumer再登録、ACK、再配送
- Activity/WebView不在時のinbox commit、Activity再生成、renderer reload、process kill後のpending回収
- notification permission拒否、期限切れIntent、queue diagnostic、resync-required
- JavaScript runtimeを採用する場合はmodule、network、limit、異常終了、再起動
- resource count、closure、task、fd、Service/runtime leak

待機時間だけで成功を判断しない。adb device property、Activity状態、native completion、latch、RPC result、page-ready log、resource countを使用する。

### 14.3 image asset

icon、adaptive icon、splash、notification small iconなどのmaster画像を追加または変更する場合は、生成結果を実際に目視し、想定したAndroid resourceになっていることを確認する。testはsource内の文字列ではなく、生成resourceとpackage動作を検証する。

## 15. 実施順序

依存関係は次のとおりである。

```text
公開scope・runtime選択・通知契約・GREEN baseline
├── Android backend分離
│   ├── public targetと設定
│   ├── Web assetとplugin package
│   ├── push adapter・durable inbox・page consumer
│   └── JavaScript runtime package（採用時のみ、通知とは独立）
├── build/pack・署名
└── host/VM/Pixel 6/npm package gate
     └── 利用者向け文書と正式公開判定
```

### ステップ0: baselineと公開scopeを確定する

- Windows Settings E2Eを確実な同期へ修正し、root testをGREENにする。
- 現行QuickJS込みのVMとPixel 6 baselineを記録する。
- 初回releaseへQuickJSまたは実Node.jsを含めるか決める。
- `android` target、artifact、plugin、network、署名の契約を決める。
- provider非依存push API、FCM標準経路、inbox、ACK、lifecycle、queue policyを決める。
- 完了条件: 課題Aをすべて満たす。

### ステップ1: Android backendを分離する

- production templateとtest fixtureを分ける。
- 固定namespaceとrepository相対pathを除く。
- backend APIとnpm package内容を定義する。
- 分離前後で全host/device gateを比較する。
- 完了条件: 課題Bをすべて満たす。

### ステップ2: public targetと設定を実装する

- target descriptorとresultをplatform別unionへ変更する。
- Android設定resolver、validation、toolchain診断を実装する。
- Androidを全target暗黙buildから除外する。
- 完了条件: 課題Cをすべて満たす。

### ステップ3: 利用者projectをAndroid buildへ接続する

- Vite asset、Muon config、metadata、resource、known pluginを接続する。
- `muon build --target android`でdebug APKを生成する。
- clean fixtureをVMとPixel 6で起動する。
- 完了条件: 課題Dをすべて満たす。

### ステップ4: push通知のdurable deliveryを実装する

- fake native event sourceでActivity/WebView不在時の受信と永続化を先に実装する。
- FCM data message adapter、system notification、tap/resume dispatcherを接続する。
- `consumeMessages()`のdocument generation handshake、lease、ACK、再配送を実装する。
- cold/warm start、reload、kill、未登録、permission拒否、queue上限をVMとPixel 6で検証する。
- 完了条件: 課題Eをすべて満たす。

### ステップ5: JavaScript runtimeを接続する

- QuickJSを採用した場合は課題Fを実施する。
- 実Node.jsを選んだ場合は、先に課題Gの独立gateを完了する。
- 初回releaseから除外した場合は、runtimeがartifactとAPIへ混入しないtestを追加する。
- いずれを選んでも、初回scopeではpush messageをinactive時のruntimeへ配送しない。
- 完了条件: 選択した公開scopeと実装、test、文書が一致する。

### ステップ6: pack、署名、npm配布物を実装する

- APK、AAB、APKS、artifact metadataを実装する。
- debugとproduction署名を分離する。
- npm pack後のclean fixtureで再buildする。
- 完了条件: 課題Hをすべて満たす。

### ステップ7: device、回帰、配布gateを完了する

- 16 KiB x86_64 VMと4 KiB Pixel 6で全gateを実行する。
- AAB由来splitとrelease APKを検証する。
- desktop、Android、upstream dependencyを含む全体testをGREENにする。
- 完了条件: 14章の必須gateがすべてPASSし、結果が記録される。

### ステップ8: 補助APIと文書を整理する

- API対応表とnetwork資料を現行仕様へ統一する。
- 利用者向け日本語、英語文書を完成させる。
- 初回release外のAPIを明示する。
- 完了条件: 課題Iを満たし、実装、test、文書に不一致がない。

各ステップは一つの巨大commitにしない。TDDの再現、修正、refactor、文書を適切な粒度へ分け、`feat:`、`fix:`、`refactor:`、`chore:`、`doc:`を使用する。外部submoduleとvendor sourceは直接変更しない。

## 16. 優先度

| 優先度 | 課題                                     | 理由                                                |
| ------ | ---------------------------------------- | --------------------------------------------------- |
| P0     | GREEN baselineと現行device baseline      | 後続変更の回帰判定を成立させるため                  |
| P0     | Android公開scopeとruntime選択            | 誤ったAPI名とpackage構成を固定しないため            |
| P0     | backend分離                              | 試作依存を製品buildから除くため                     |
| P0     | public target、設定、build               | 利用者がAndroid成果物を作る入口のため               |
| P0     | push通知とdurable inbox                  | inactive時の受信をページ主体のappへ失わず渡すため   |
| P0     | APK/AAB、production署名、npm clean build | 配布可能性の必須条件のため                          |
| P0     | VM、Pixel 6、全体回帰                    | 正式support判定のため                               |
| P0     | 利用者向け制約文書                       | network、通知、permission、runtimeの誤解を防ぐため  |
| P1     | QuickJS製品化                            | 初回scopeに含める場合はP0へ昇格する                 |
| P1     | 実Node.js gate                           | 実Node.jsが製品要件の場合はP0へ昇格する             |
| P2     | `content://`、picker、Android固有API     | 具体的な利用者要件ごとに追加するため                |
| P2     | push background JS handler               | native inboxからページへの配送完成後に検討するため  |
| P2     | 16 KiB arm64 runtime追加環境             | 成果物整列は必須、runtimeは環境確保後に追加するため |

## 17. 計画全体の完了条件

Android製品化は、次をすべて満たした場合に完了とする。

1. 公開`android` targetをCLI、Vite、型付きAPIから選択できる。
2. Androidを指定しないdesktop build、pack、runtimeの挙動が変わらない。
3. クリーンな利用者projectから公開CLIだけでdebug APKをbuildできる。
4. 公開CLIだけでrelease APKとAABをpackできる。
5. application ID、version、icon、theme、permission、asset、Muon configが利用者設定から生成される。
6. `arm64-v8a`と`x86_64`のnative library、plugin、16 KiB alignmentを検査できる。
7. 登録pluginが両ABIで動き、未登録または不正pluginがbuild時または起動時に安全に拒否される。
8. debug、unsigned、production署名を区別し、secretをrepository、artifact、logへ漏らさない。
9. WebViewの通常network、asset fail-closed、RPC main-frame境界、permissionの契約が実装と文書で一致する。
10. FCM adapterがActivity/WebView不在時にもmessageをnative inboxへcommitでき、provider固有の型と設定構造が公開page APIへ漏れない。
11. inboxへのcommit後は、page consumer登録前、background、tap、dismiss、reload、Activity/renderer/process再生成をまたいでat-least-once配送される。
12. consumerの正常完了時だけACKし、失敗、timeout、kill時は安定したmessage IDとattemptを伴って再配送される。
13. callbackやJS instanceの生存を前提にせず、document generation handshakeがsubscribe前後とpending/live境界の競合を防ぐ。
14. TTL、retry、queue上限、deduplication、consumer未登録diagnostic、resync-requiredの契約が実装、test、文書で一致する。
15. inactive時に開発者JSを実行せず、QuickJSを含めない構成でも通知配送の完了条件を満たす。
16. JavaScript runtimeを含める場合は、engine identity、project package、対応module、limit、lifecycle、security保守が確定している。
17. JavaScript runtimeを含めない場合は、関連API、設定、permission、native assetが存在しないことをtestしている。
18. npm packした正式packageを別のクリーンfixtureへinstallしてもAndroid build/packできる。
19. Android toolchainがない環境ではdesktopを壊さず、Android指定時だけ明瞭なdiagnosticを返す。
20. 16 KiB x86_64 VMでinstrumentation、release APK、AAB由来split APKがPASSする。
21. 4 KiB arm64-v8a Pixel 6で同じgateがPASSする。
22. 利用可能なarm64 16 KiB環境がない場合も、arm64 artifactの16 KiB整列を必須検査し、runtime未実施を明記する。
23. root `npm test`が終了code 0で、既知失敗をAndroid成功条件の外へ隠していない。
24. API対応表、network制約、通知保証、build、pack、署名、install、debug手順が日本語と英語で一致する。
25. 旧試作のproduction codeが重複して残らず、fixtureとして必要な範囲だけに整理されている。
26. 外部submoduleとvendor codeを直接変更せず、必要なpatchとbuild recipeをMuon管理下で再現できる。

この計画の最初の製品成果は、QuickJSまたはNode.jsの採用有無にかかわらず、WebView、Muon API、build-time NDK plugin、nativeのdurable inboxを使うpush通知を持つAndroidアプリを、一般の利用者projectから公開CLIでAPK/AABとして生成できることである。通知はinactive時に開発者JSを実行せず、ページがconsumerを登録した後にat-least-onceで配送する。JavaScript runtimeは、そのengineと互換範囲を正しく表現できる場合だけ正式supportへ含め、push background handlerは後続課題とする。
