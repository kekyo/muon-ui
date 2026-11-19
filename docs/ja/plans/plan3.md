# Android 製品化に向けた残課題

## 1. この文書の目的

`plan.md` のステップ5までで、Android上の基礎的な実行方式、WebView、RPC、ネットワーク、NDKプラグイン、cardio、tra-ffic/libffiについて、ローカルAndroid VMとPixel 6実機を使った検証を終えた。

この文書では、その検証結果を前提として、Android対応を利用者向けの正式機能にするまでの残課題を整理する。大きな柱は次の2つである。

muon-uiからAndroidアプリをビルドし、配布用にパッケージ化できるようにする。

Android VMで動く試験用アプリを増やすこと自体は、この計画の目的ではない。既存の試作コードを、一般のmuon-uiプロジェクトから再利用できる正式なビルド経路へ移すことが目的である。

## 2. 前提となる完了済み事項

次の事項は `plan.md` で検証済みであり、この計画では原則として再検討しない。

- CEFに依存しないRPC層をAndroid WebViewから使用できる。
- muon-coreのWebViewおよびブラウザ機能をAndroid上で起動できる。
- Android用dispatcher hostにより、AndroidのLooperへcardioを統合できる。
- cardioとtra-fficのAndroid正式対応版をsubmoduleとして使用できる。
- tra-ffic/libffiのクロージャおよび動的トランポリンを、Android VMとPixel 6実機で実行できる。
- Android API Level 24以上を下限として、必要なmuon-core APIとNDKプラグインを実行できる。
- x86_64 Android VMおよびarm64-v8a Pixel 6で、代表的なプラグインとページ操作を検証できる。
- Android向けプラグインは事前に登録し、アプリ起動後に任意の共有ライブラリを探索、ダウンロード、ロードしない方針である。
- 通常のネットワーク通信は、Androidの権限とmuon側の信頼境界を満たす場合に利用できる。

確認済みのAPIと差分は[Android API対応方針](./android-api-compatibility.md)を参照する。

## 3. 現在の到達点と不足

現状は「Androidでmuon-coreを動かせる」段階には到達しているが、「muon-ui利用者が通常のプロジェクトからAndroid成果物を作れる」段階には到達していない。

| 項目 | 現状 | 残る作業 |
| --- | --- | --- |
| Androidランタイム | VMとPixel 6で検証済み | 試作専用コードから正式バックエンドへ移す |
| public target | Linux/Windowsのみ | Android用targetと設定モデルを追加する |
| build | 試作用の専用コマンドと固定構成 | 一般のmuon-uiプロジェクトを入力にして生成する |
| pack | zip、tar.gz、deb、NSIS中心 | APK、AAB、署名、複数ABIを扱う |
| Androidプラグイン | 静的レジストリを試作で検証済み | 公開設定、配布物、エラー診断を定義する |
| 補助API | 主要APIの互換性を分類済み | `content://` など必要性に応じた補完が残る |

既存実装上の主な入口は次のとおりである。

- 公開targetの定義: [ui/common/targets.ts](../../../ui/common/targets.ts)
- build処理: [ui/common/build.ts](../../../ui/common/build.ts)
- pack処理: [ui/common/pack.ts](../../../ui/common/pack.ts)
- Android設定の試作: [core/android/renderer/android-config.ts](../../../core/android/renderer/android-config.ts)
- Androidのビルドとテストは[テストホスト](../../../core/android-test/README.md)を参照する。

## 4. 課題A: Androidの正式ビルドとパッキング

### 4.1 試作バックエンドの製品化

現在のAndroid実装は `muon-android-prototype` 配下にあり、リポジトリ構成、試験用application ID、生成先、JNIクラス名などに依存している。正式機能では、利用者のプロジェクトから同じ処理を呼び出せる再利用可能なバックエンドに分離する必要がある。

検討事項は次のとおりである。

- Android固有のGradle、CMake、JNI、Java/Kotlin、マニフェスト生成処理を、試験用シナリオから分離する。
- 現在リポジトリルートをたどってsubmoduleや生成物を参照する箇所を、インストール済みnpmパッケージ内でも解決できる構成へ変える。
- 試験用の `dev.muon.prototype` に結び付いたJava名前空間やJNIシンボルを、安定したmuon内部名前空間へ移す。
- 利用者のapplication IDごとにJNI関数名を生成しない。固定した内部クラスを使うか、`RegisterNatives` による明示登録を使用する。
- Androidテンプレート、Gradle Wrapper、CMake定義、NDKレジストリ、libffi対処を、npm配布物から欠落しないようにする。
- 試作用CLIと正式CLIの責務を分け、試作用コマンドを公開APIとして固定しない。

バックエンドの配置には次の案がある。

1. `muon-android` を独立した内部パッケージにして、`muon-ui` のCLIから利用する。
2. `muon-ui` パッケージ内にAndroidバックエンドを直接含める。

推奨は1である。Android SDK、NDK、Gradle、テンプレートという固有の責務を隔離でき、試作で蓄積した検証コードも整理しやすい。ただし、利用者が操作するCLIは既存の `muon` に統一し、内部パッケージを意識させない。

### 4.2 Android targetと成果物モデル

既存targetは、CEFのtarget、launcher実行ファイル、Linux/Windows向けの単一プラットフォーム成果物を前提としている。Androidは1つの配布物に複数ABIを含み、APK、AAB、APKSという異なる成果物を持つため、既存モデルへ文字列を1つ追加するだけでは不足する。

採用方針は次のとおりとする。

- 公開targetは `android` とする。
- `android-arm64-v8a` と `android-x86_64` を独立した公開targetにはしない。
- ABIはAndroid targetの設定として扱い、既定では `arm64-v8a` と `x86_64` を生成する。
- x86_64はローカルVMテスト、arm64-v8aは実機および一般的な配布に使用する。
- build/packの戻り値は、デスクトップ実行ディレクトリとAndroid成果物を判別できるdiscriminated unionへ変更する。
- Android環境を持たない利用者の通常ビルドを壊さないよう、Android targetを全targetの暗黙ビルド対象には含めない。
- Android SDK、NDK、JDK、Gradleが必要になるのは、Android targetを明示した場合だけとする。

Android用target記述子には、少なくとも次の情報が必要である。

- target種別
- ABI一覧
- minSdk
- targetSdk
- build variant
- application ID
- versionCode
- versionName
- 署名設定
- 最終成果物の種別

### 4.3 プロジェクト入力と設定

正式ビルドでは、Viteが生成したWebアセット、muon設定、Androidメタデータ、NDKプラグインを1つのAndroidアプリへまとめる必要がある。

設定の責務は次のように整理する。

- 既存の `package.json`、`muon.json`、Vite設定、CLI引数の優先順位を明文化する。
- Android固有設定は型付きの公開設定として追加する。
- Android用バリデータで、利用できないデスクトップ機能をビルド前に検出する。
- `browser.window` などAndroid向け変換が必要な設定は、暗黙に無視せず、対応、変換、エラーのいずれかを明示する。
- 開始ページとアセットoriginを生成設定へ含め、WebViewの信頼境界と一致させる。
- `muon.json` を単純にコピーするのではなく、Androidで必要な正規化済み設定を生成する。
- 開発時のvirtual moduleとパッキング後の実データが同じ意味になるようにする。

Android固有設定として最低限必要な候補は次のとおりである。

- `applicationId`
- `namespace`
- `versionCode`
- `versionName`
- `minSdk`
- `targetSdk`
- `abis`
- `permissions`
- アプリアイコンとadaptive icon
- テーマ、スプラッシュ、表示名
- network security設定
- 署名設定への参照
- Androidプラグインの登録

`minSdk` は検証済みのAPI Level 24を下限とする。`targetSdk`、NDK、Gradle Pluginは、実装時点でGoogle Play要件と使用するツールチェーンを確認して固定する。利用者が未検証の値へ自由に下げられる設計にはしない。

### 4.4 Androidプラグインの正式設定

試作で検証した静的レジストリ方式を正式ビルドにも採用する。

- ビルド時に使用するプラグイン一覧を確定する。
- 各プラグインをアプリのnative libraryとしてリンクまたは同梱する。
- 起動時に、同梱済みプラグインだけをレジストリへ登録する。
- JavaScriptから指定できる名前とnative側登録名を一致させる。
- 未登録名は即座に説明可能なエラーにする。
- アプリ起動後の任意パス探索、共有ライブラリのダウンロード、未登録コードのロードは行わない。

公開設定では、少なくとも次の入力方法を検討する。

1. muonが提供する既知プラグインを名前で選ぶ。
2. 利用者がソースを提供し、Android向けにクロスコンパイルする。
3. ABI別の事前ビルド済みライブラリを提供する。

初期実装では1を必須とし、2と3は安全な検証方式を定義できたものから追加する。3を許可する場合は、arm64-v8aとx86_64の整合、Android API、NDK ABI、16KBページ対応、ライセンスをビルド時に診断する。

### 4.5 Androidメタデータとリソース

試作で固定されている次の値を、利用者のプロジェクトから生成できるようにする。

- application IDとnamespace
- アプリ名
- versionCodeとversionName
- 通常アイコン、adaptive icon、必要な密度別リソース
- テーマ、スプラッシュ、背景色
- 必要なAndroid権限
- ネットワークセキュリティ設定
- Activity、Service、Providerなどのマニフェスト要素

application IDはJavaパッケージ名として妥当か検証する。versionCodeはAndroidの整数制約を満たし、既存成果物より単調増加させる責任を利用者へ明示する。

権限はプラグインが暗黙に追加するものと、利用者が明示するものを区別する。不要な危険権限をテンプレートへ一律追加しない。

### 4.6 buildとpackの境界

既存CLIの考え方を維持し、次の境界を推奨する。

- `muon build --target android` は、Webアセット、native library、Androidプロジェクトを生成し、デバッグ用または未署名の検証可能なAPKまで作る。
- `muon pack --target android --type apk` は、配布または実機インストール用APKを生成する。
- `muon pack --target android --type aab` は、ストア配布用Android App Bundleを生成する。
- APKSはbundletoolによるローカル検証用の派生成果物として扱い、主要な配布形式にはしない。

buildとpackの双方で、成果物パス、variant、ABI、署名状態、application ID、versionを機械可読な結果として返す。生成先の命名規則を安定させ、古い成果物と今回の成果物を取り違えないようにする。

### 4.7 署名と秘密情報

署名は開発用と本番用を分離する。

- ローカルdebug buildではAndroid標準のdebug keystoreを使用できる。
- 本番署名鍵をリポジトリや生成済みAndroidプロジェクトへコピーしない。
- keystoreパス、alias、パスワードは外部設定、環境変数、または対話しない安全な資格情報供給方法から受け取る。
- ログ、エラー、ビルド結果へパスワードを出力しない。
- 未署名、debug署名、本番署名の状態を成果物メタデータへ明示する。
- AABにPlay App Signingを使う場合も、アップロード鍵の扱いを文書化する。

### 4.8 ツールチェーンと配布物

正式バックエンドは、少なくとも次を利用者環境で診断できなければならない。

- 対応JDK
- Android SDKと必要なplatform
- Android NDK
- CMakeまたはNDKのCMake toolchain
- Gradle Wrapper
- bundletoolまたは同等のAAB検証手段
- adb
- Android VMまたは接続済み実機

不足時は、内部のGradle/CMakeエラーをそのまま見せるのではなく、何が不足しているかを先に報告する。自動ダウンロードを行う場合は、バージョン、URL、ハッシュ、キャッシュ先、ライセンスを固定する。

npm pack後のパッケージに、Gradle Wrapper、テンプレート、JNIソース、CMake定義、プラグインレジストリ、必要なパッチまたは設定が含まれることをテストする。リポジトリ内では動くが、インストール済みパッケージでは相対パスが切れる状態を許容しない。

### 4.9 テスト方針

実装時は各変更をTDDで進め、個別テストだけでなくリポジトリ全体のテストを実行する。Android固有の検証は次の層に分ける。

1. 設定、target、成果物モデルの単体テスト
2. クリーンな利用者fixtureからのbuild/pack統合テスト
3. ローカルx86_64 Android VMへのインストールと起動テスト
4. Pixel 6 arm64-v8a実機へのインストールと起動テスト
5. AABから生成した端末別APKのインストールテスト

VMテストは自動テストの完了条件に含める。Pixel 6テストは、端末を手動で接続した後に実行する明示的な実機検証とする。時間経過だけを期待する待機は避け、adb、Activity状態、RPC応答、ログ上の明確な準備完了条件を使う。

### 4.10 課題Aの完了条件

次をすべて満たした時点で、Androidの正式ビルドとパッキングを完了とする。

1. クリーンなmuon-ui利用者プロジェクトから、公開CLIだけでAndroid buildを開始できる。
2. build処理が `muon-android-prototype` やリポジトリルートの非公開パスを直接参照しない。
3. 設定の型、検証、優先順位、エラーがテストされている。
4. arm64-v8aとx86_64を含むAPKまたはAABを生成できる。
5. 登録したNDKプラグインが両ABIで動作し、未登録プラグインが安全に拒否される。
6. debug APKをローカルAndroid VMへ自動インストールし、cold start後に代表的RPCを完了できる。
7. 同じ成果物系列をPixel 6へインストールし、arm64の代表的RPCを完了できる。
8. リリースAPKとAABを生成でき、署名状態と成果物情報をCLI結果から確認できる。
9. npm packした正式パッケージを別のクリーンなfixtureへインストールしてもbuildできる。
10. Androidツールチェーンがない環境では、Android以外のbuildを壊さず、Android指定時だけ明瞭な診断を返す。
11. 全体テストがPASSし、利用者向けのAndroid build、pack、署名、実機確認手順が文書化されている。

## 7. 推奨する実施順序

### ステップ1: Android公開契約を決定する

- `android` targetの型とCLI表現を決める。
- build、pack、APK、AABの境界を決める。
- 設定優先順位とAndroid固有設定を決める。
- 完了条件: 公開型、CLI例、成果物、エラー条件について、実装とテストが参照できる仕様がある。

### ステップ2: Androidバックエンドを試作から分離する

- テンプレート、JNI、CMake、Gradle、設定生成を再利用可能な内部パッケージへ移す。
- 固定application IDとリポジトリ相対パスへの依存を除く。
- 既存のVMおよびPixel 6シナリオを、新しいバックエンド経由でもPASSさせる。
- 完了条件: 試作CLIを使わず、バックエンドAPIから既存検証アプリを生成できる。

### ステップ3: `muon build --target android` を実装する

- target、設定検証、Vite成果物、muon設定、プラグインを接続する。
- クリーンなfixtureをVMへインストールし、cold startとRPCを検証する。
- 完了条件: 課題Aのbuild関連完了条件を満たし、全体テストがPASSする。

### ステップ4: `muon pack` と署名を実装する

- APK、AAB、複数ABI、成果物情報、debug/production署名を実装する。
- npm pack後のクリーン環境で再検証する。
- AABから端末用APKを生成し、VMとPixel 6で検証する。
- 完了条件: 課題Aの完了条件をすべて満たす。

### ステップ7: 補助APIと文書を整理する

- 優先度を付けたAndroid固有API差分を必要な範囲で実装する。
- ネットワーク、ファイル、非対応APIの文書を現在の挙動へ統一する。
- 利用者向けbuild、pack、署名、配布、デバッグ手順を完成させる。
- 完了条件: 実装、テスト、利用者向け文書の対応範囲が一致する。

各ステップは独立したコミットへ分ける。想定するコミット境界は次のとおりである。

- `refactor: extract Android build backend`
- `feat: build Android applications`
- `feat: pack Android application bundles`
- `doc: document Android distribution`

実際には各ステップをさらにTDDのRED、GREEN、整理の単位へ分割し、外部submoduleやベンダーコードは直接変更しない。

## 8. 補助的に残る課題

二つの主要課題とは別に、次の項目が残っている。

### 8.1 `content://` とAndroidファイル選択

AndroidのStorage Access Frameworkは通常のファイルパスではなく `content://` URIを返す。現在のファイル、dialog、media APIへそのまま渡せるとは限らない。

- URIをJavaScriptへ公開するのか、一時ファイルまたはfile descriptorへ変換するのかを決める。
- persistable URI permissionのlifecycleを決める。
- これを必要とする利用者機能が決まるまでは、Android build/packのP0条件にはしない。

### 8.2 ネットワーク文書の統一

[filter-limitation.md](../../../filter-limitation.md) には、初期検討時のdeny-all方針と、その後に検証した通常ネットワーク許可の条件が混在する可能性がある。最終実装では次を一貫して説明する。

- 信頼されたアセットoriginからの通常通信
- Android INTERNET権限
- cleartext通信の扱い
- navigation、subresource、fetch/XHR、WebSocketの境界
- 信頼判定が失敗した場合のfail-closed動作

### 8.3 16KBページサイズ

Pixel 6でのarm64動作確認とは別に、16KBページサイズ端末を正式対象に含める場合は、全native libraryをその条件で検証する。利用者提供のネイティブライブラリを追加した時点で再度ゲートを通す。

### 8.4 Android固有の製品API

次は具体的な製品要求が出た時点で追加する。

- notification
- foreground Service
- share intent
- app link/deep link
- in-app update
- task/window挙動
- Android固有permission request UI

これらはmuon-coreがAndroidで起動するための必須条件ではなく、現段階の未完了バグとして一括実装しない。

### 8.5 意図的な非対応

デスクトップの概念をAndroidへ機械的に移さない。

- system tray
- デスクトップlauncher/updater
- 任意の外部実行ファイル
- アプリ起動後の任意native pluginロード
- NSIS、debなどデスクトップ専用pack形式

これらはAndroid targetでは説明可能なvalidation errorにするか、target固有設定から除外する。

### 8.6 既知の非Androidテスト課題

Windows SettingsのE2E timeoutなど、Android作業中に判明した既存の不安定テストは、Android機能の成功条件と混同しない。ただし全体テストの完了を妨げる場合は、再現条件を分離し、時間待ちに依存しない確実な同期へ直す別課題として扱う。

## 9. 優先度

| 優先度 | 課題 | 理由 |
| --- | --- | --- |
| P0 | Android公開target、正式build、APK/AAB pack、署名 | 現状を試作から利用者向け機能へ変える必須条件 |
| P1 | ネットワーク、配布、非対応機能の文書整理 | 実装と利用者の期待を一致させるために必要 |
| P2 | `content://`、16KB追加検証、Android固有API | 利用者機能と対象端末に応じて追加できる |

## 10. 計画全体の完了条件

Android製品化の計画全体は、次の条件で完了と判定する。

1. `plan.md` で検証したAndroid runtimeを、試作専用経路ではなく正式なmuon-ui targetから使用できる。
2. クリーンな利用者プロジェクトを、公開CLIでbuildし、APKおよびAABへpackできる。
3. ローカルAndroid VMで自動テストを最後まで実行し、インストール、cold start、代表的RPCがPASSする。
4. 手動接続したPixel 6で同じ機能をarm64-v8aとして確認できる。
5. 署名、権限、プラグイン、ABI、ツールチェーン不足が説明可能かつ安全に処理される。
6. npm配布物だけを使うクリーン環境で成果物を再生成できる。
8. 実装したすべての変更に機能を検証するテストがあり、リポジトリ全体のテストがPASSする。
9. 実装、テスト、利用者向け文書で、対応target、API、成果物、制限事項が一致する。
10. 外部submoduleおよびベンダーコードを直接変更せず、必要な対処をmuon管理下のコード、設定、patch、build scriptで再現できる。

最優先の課題は、Androidを公開targetに加え、build、pack、署名、配布物へ接続することである。
