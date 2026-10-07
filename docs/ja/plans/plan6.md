# Android対応を完成させdevelopへマージする計画

## 1. 目的と位置付け

muon-uiのnpm配布物を新規アプリへ導入し、公開CLIでAndroidアプリをビルド・署名・配布でき、端末上で動作する状態にする。その完了を確認してから、`feature/android`を`develop`へマージする。

この文書は、2026年10月7日の分析と対話を保存した実装計画である。実装の調査基準は`27b7f25e`、保存時のブランチ先頭は計画書の移動を行った`a8c8005`である。保存時には実装を開始していなかった。2026年10月7日に実施を開始した。進捗と検証結果は末尾の実施記録へ追記する。

[plan4.md](plan4.md)の製品化計画を、今回のマージに必要な範囲へ絞る。[plan5.md](plan5.md)のFCM実装計画は後続作業として残す。今回の優先順位と完了条件は本計画を使用し、旧計画のFCM完成条件を今回のマージ条件として扱わない。

### 1.1 利用者と合意した範囲

今回の目的は、アプリ開発者がmuon-uiを使って動作するAndroidアプリをパッケージ化できることである。ツールチェインの準備、npm内のAndroid部品の配置、公開ビルド手順、利用者アプリのE2E、配布用APKを対象とする。

FCMは制限があってもよく、通常のMuonアプリの起動や操作を妨げないことを条件とする。通知機能や配送保証の完成はマージを待たせる条件にしない。

QuickJSは、利用者のJavaScriptコードをどのようにビルド・同梱・更新するかが整理されていれば、現時点で使用できなくてもよい。実行機能の正式提供と利用者コードの実機実行は後続へ回せる。FCMとQuickJSを互いの前提にしない。

### 1.2 本計画で示す対応案

内部workspaceの`muon-android`へAndroid実装を分離し、公開npmパッケージとCLIは既存の`muon-ui`と`muon`へ統一する。通常のアプリビルドでは、事前ビルドしたAndroidランタイムを利用する構成を推奨する。

初回のビルド環境はLinux x64を保証対象とする案で進める。Android端末のABIは`arm64-v8a`と`x86_64`とし、公開ターゲットは一つの`android`とする。Windows/macOS上のAndroidビルドは追加検証として扱う。

署名済みrelease APKを今回の必須成果物にする。AABは既存の生成処理を再利用して同時に公開する候補とするが、APK配布の完成を遅らせる必須条件にはしない。AABを公開対象に含めた場合は、署名と端末別APKの動作検証までを必須にする。

これらは合意した目的を実現するための対応案である。AARの具体的な配置、設定キー、CLIオプションの詳細、最低対応OSとWebViewの条件は、ステップ0で確定して本計画へ記録する。

## 2. 現在の実装と残作業

| 観点 | 調査で確認した状態 | 今回の残作業 |
| --- | --- | --- |
| 実行基盤 | WebView、RPC、基本Muon API、ネイティブプラグイン、QuickJSの実装が試作内にある | 通常の利用者アプリから使用できる構成へ分離する |
| 公開ターゲット | `MuonTarget`はLinux/Windowsのみ | CLI、Vite、公開型、成果物型へAndroidを追加する |
| アプリ設定 | アプリID、バージョン、画面、起動URLなどが試作の固定値 | 利用者設定とWebアセットから生成する |
| npm配布 | Android試作はprivate workspaceで、正式なパッケージ生成へ未接続 | Android部品をnpm配布物へ収録する |
| ネイティブビルド | リポジトリ内のソース・サブモジュールとLinux x64のNDKを前提とする | 配布物だけで依存を解決し、通常利用者のネイティブビルドを省く |
| APK/AAB | 試作内には生成・検査する処理がある | 利用者アプリと公開CLIへ接続する |
| 署名 | releaseもテスト用のdebug鍵で署名する | 利用者の鍵による正式な署名を実装する |
| 端末検証 | x86_64・16 KiB VMとarm64・Pixel 6向けの処理がある | npm配布物から作った独立した利用者アプリで検証する |
| FCM | SDK、受信サービス、公開APIとも未実装 | 今回は未提供でも通常動作が成立するようにする |
| QuickJS | 固定の`backend.mjs`を読み込む限定ランタイム | 配布・更新方針を確定し、未提供時の挙動を整える |

実装の主な参照先は次のとおりである。

- 公開ビルドと配布形式は[targets.ts](../../../muon-ui/src/targets.ts)、[build-sequence.ts](../../../muon-ui/src/build-sequence.ts)、[build.ts](../../../muon-ui/src/build.ts)、[pack.ts](../../../muon-ui/src/pack.ts)、[vite.ts](../../../muon-ui/src/vite.ts)を参照する。
- npm配布の入口は[muon-ui/package.json](../../../muon-ui/package.json)と[build_package.sh](../../../build_package.sh)を参照する。
- Androidの固定設定と署名は[app/build.gradle.kts](../../../muon-android-prototype/android/app/build.gradle.kts)、初期化処理は[prototype.ts](../../../muon-android-prototype/src/prototype.ts)と[MuonActivity.java](../../../muon-android-prototype/android/app/src/main/java/dev/muon/runtime/MuonActivity.java)を参照する。
- リポジトリ依存は[build-native-dependencies.mjs](../../../muon-android-prototype/scripts/build-native-dependencies.mjs)、[CMakeLists.txt](../../../muon-android-prototype/android/app/src/main/cpp/CMakeLists.txt)、[generate-android-plugin-registry.mjs](../../../muon-android-prototype/scripts/generate-android-plugin-registry.mjs)を参照する。
- QuickJSの読み込みは[MuonJavaScriptRuntimeService.java](../../../muon-android-prototype/android/app/src/main/java/dev/muon/runtime/MuonJavaScriptRuntimeService.java)を参照する。
- 検証方法は[試作README](../../../muon-android-prototype/README.md)、[試作package.json](../../../muon-android-prototype/package.json)、[release検証スクリプト](../../../muon-android-prototype/scripts/test-android-release-packages.mjs)を参照する。

生成済みAPK/AABが作業環境に存在していても、現在のブランチに対する成功結果とはみなさない。調査時に接続端末はなく、端末テストを再実行していない。旧計画に記録されたWindows E2Eの失敗やテスト件数も過去の情報であり、ステップ0で現在の結果を確認する。

## 3. ツールチェインの準備

### 3.1 利用者とMuon本体の開発環境を分ける

| 用途 | 必要にする道具の案 |
| --- | --- |
| 通常のアプリビルド | Node.js/npm、対応JDK、Android SDK・Build Tools、配布物に含めたGradle Wrapper |
| 端末へのインストールと検証 | 上記にadb、実機またはエミュレーターを追加 |
| Muon本体や独自C/C++プラグインの開発 | 上記にNDK、CMake、依存ライブラリのビルドに必要な道具を追加 |
| AAB由来の端末別APKの検証 | 対象の処理でbundletoolを準備 |

標準ランタイムを事前ビルドすれば、通常のアプリ開発者にNDK、CMake、libffiのビルド、サブモジュール取得を要求しない構成にできる。Android Studioは必須にせず、JDKとSDKをCLIから利用する。端末へのインストールを行わないビルドでは、接続端末やエミュレーターを要求しない。

現在のビルドスクリプトはNDKの`linux-x86_64`を固定している。初回の保証対象をLinux x64とする根拠はこの実装と既存の検証環境であり、Windows/macOSで実行できるとはまだ主張しない。

### 3.2 検出・準備・診断

`muon prepare --target android`を環境の確認・準備の入口とする案を採用する。既存SDKとJDKを検出し、不足する道具について期待するバージョン、検出結果、準備方法を表示する。SDKの配置先を利用者が指定できるようにし、既存環境を不必要に変更しない。

GradleはWrapperで固定する。SDKパッケージの準備には[公式のAndroid CLI](https://developer.android.com/tools/agents/android-cli/commands/sdk)を使用する。自動取得するものはバージョン、取得元、検証方法、キャッシュ先を決める。SDKのライセンス受諾は利用者が行い、Muonが無断で代行しない。

JDKは検出と不足時の案内を初期対応とし、OS全体のパッケージ管理まで自動化することは必須にしない。SDKを自動準備する場合のオプションと、CIでの非対話実行方法はステップ0で確定する。

Androidを明示した時だけAndroid用の診断を行う。npmのインストールや通常のdesktopビルドでSDKを要求したり、Androidツールチェインを自動取得したりしない。

### 3.3 バージョンの固定

現在の試作はAGP 9.2.1、Gradle 9.4.1、compileSdk/targetSdk 37、Build Tools 36.0.0、NDK 29.0.14206865、CMake 4.1.2を指定している。Javaソースの互換性設定は17、bundletoolは1.18.3である。これらを調査開始時の候補として記録し、利用者向けに保証する組み合わせは実際のビルドで確認する。

JDKの実行バージョンとJavaソースの互換性設定は区別する。[AGP 9.2の互換性表](https://developer.android.com/build/releases/agp-9-2-0-release-notes)と、使用するAPIの公式コメントを実装時に確認する。バージョンを自動的に最新版へ追従させない。

## 4. Android部品の分離とnpm内の配置

### 4.1 内部構成と利用者の導入先

Android固有の生成・ビルド処理、Java、JNI、リソース、依存ビルド処理を内部workspaceの`muon-android`へ分離する。利用者は`npm install -D muon-ui`だけでMuon側の必要な部品を導入する。内部workspaceを別途インストールしたり、そのスクリプトを直接呼び出したりする手順にはしない。

製品用コードと試験用ページ、プラグイン、障害注入処理を分ける。試作は検証用アプリへ縮小し、共通コードを二重に保守しない。QuickJSの試作コードと検証結果は保持できるが、利用者向け成果物への同梱とは別に扱う。

### 4.2 事前ビルドしたランタイム

Javaコード、Androidリソース、Manifest、ABI別ネイティブライブラリを含むAARを配布する構成を推奨する。AARにはこれらを収録できる。AARを直接参照するだけでは推移的依存を自動管理できないため、AndroidXなどの依存情報も配布・生成する必要がある。[Androidライブラリの公式資料](https://developer.android.com/studio/projects/android-library)

配置案は次のとおりである。確定したディレクトリ名は実装時に本節へ反映する。

```text
muon-ui/
  dist/
    cli.cjs
    android/
      templates/
      maven/
      renderer/
      toolchain.json
      licenses/
```

`templates`にはアプリ用Gradleプロジェクト、Wrapper、Manifestとリソースの雛形を置く。`maven`にはMuonのAARと依存メタデータを置く。`renderer`にはWebView側のMuon API初期化コードを置く。`toolchain.json`には対応バージョンと取得・検証に必要な情報を記録する。

npm内のMuon部品だけで元のリポジトリへの参照を不要にする。ただし、JDK、Android SDK、GradleやAndroidXなどの公式配布元からの取得まで不要になるという意味ではない。

### 4.3 アプリごとに変わる情報の分離

現在のプラグイン登録処理はC++ソースを生成してランタイムへ組み込むため、AARを作るだけではアプリごとのNDKビルドを省けない。登録名、許可するAPI、設定などを共通ランタイムから分け、ビルド時に確定した情報をアプリへ同梱する。

起動後の任意パス探索や追加ネイティブコードのダウンロードは導入しない。含めたプラグインだけを登録し、ABI、エントリーポイント、依存ライブラリ、16 KiB整列を検証する。初期の公開プラグイン入力方式はステップ0で確定し、独自C/C++ソースの汎用ビルド機能を今回の必須条件にしない。

Muon内部のJava/JNI名と利用者のapplication IDを分離する。アプリ名を変えるたびにMuonのネイティブライブラリを再ビルドする構成を避ける。Webアセット、設定、ABI、出力先は明示的に受け取り、親ディレクトリ探索でリポジトリを見つける処理を製品経路から除く。

ソース同梱方式は段階的な移行中に利用できるが、最終的に通常利用者へNDKを要求する方式へ変更する場合は、準備の簡略化に対する影響を分析し、本計画と利用手順を更新してから進める。

## 5. 利用者から見たビルド手順と設定

### 5.1 公開コマンド

既存のViteプロジェクトへ`muon-ui`を導入し、MuonのViteプラグインとAndroid設定を追加する。利用者がGradleやJNIのソースを手で編集しなくても、次の操作で成果物を得られるようにする。

```bash
npm install -D muon-ui
npx muon prepare --target android
npx muon build --target android
npx muon pack --target android --type apk
```

この例は計画時の操作案であり、ステップ4までに実装した。`prepare`は環境の確認・準備、`build`はインストール可能なdebug APK、`pack`は配布用release APKを担当する。署名情報は別途設定する。

Vite設定でAndroidを選んだ場合の`npm run build`も同じビルド処理へ接続する。ViteのWebビルドは一度だけ実行し、Gradleから再帰的に同じビルドを起動しない。APKにはビルド済みアセットを含め、配布先で開発サーバーを必要としない。

AndroidのHMR、端末の自動選択、IDE統合などは今回の必須条件にしない。生成したdebug APKのインストール、ログ取得、基本的なデバッグ方法は利用者向けに記載する。

### 5.2 設定と型

application ID、表示名、versionCode/versionName、アイコン、ABI、必要な権限、起動ページ、Webアセット、Muon設定を利用者が指定できるようにする。テーマと起動画面には動作する既定値を用意し、高度なカスタマイズを必須にしない。

`package.json`、`muon.json`、Vite設定、CLI引数の担当範囲と優先順位を確定する。buildとpackで同じ正規化処理を使い、入力が同じならアプリの識別情報と設定が一致するようにする。

公開ターゲットは`android`とし、ABIはAndroid設定で扱う。Androidを暗黙の全ターゲットビルドへ加えてSDK未導入の利用者に影響させない。desktop成果物とAndroid成果物は型で判別し、Androidに存在しないlauncherやCEFの情報を必須にしない。

ビルド結果には生成物のパス、debug/release、ABI、application ID、バージョン、署名状態を含める。出力先は`dist-muon/android/`を基本案とし、古い成果物との取り違えを防げるファイル名と整理方法を決める。

### 5.3 WebViewとMuon API

試作ページが行っているMuon APIの初期化を、通常の利用者アプリへ自動的に組み込む。利用者が試作専用のadapterをimportする必要をなくし、アプリのJavaScriptがMuon APIを使う前に初期化を完了する。

アセットの配信先とRPCを受け付けるoriginを同じ設定から生成する。不明アセットや不正なリクエストを外部ネットワークへ転送せず、RPCは信頼済みoriginのmain frameからだけ受け付ける。

Androidで使えないdesktop設定はビルド時に診断する。CEF版の`network.allow`などをAndroidでも同じように強制できるとは説明しない。現在の方針と制約は[Android設定検証](../../../muon-android/src/renderer/android-config.ts)、[ネットワーク制約の調査](../../../filter-limitation.md)、[利用者向け制約](../limitation.md)を参照して整理する。

権限は有効な機能から導出する。`ACCESS_LOCAL_NETWORK`などの実行時権限は、Manifestへ記載するだけで使用できる前提にしない。必要な構成だけで要求し、拒否時の扱いを決める。[Androidのローカルネットワーク権限](https://developer.android.com/privacy-and-security/local-network-permission)

## 6. 署名と配布物

署名済みrelease APKを今回の必須成果物とする。debug鍵による試験用APK、未署名APK、利用者の鍵によるrelease APKを区別する。release配布を要求された時に署名設定が不足していれば、debug鍵へ置き換えず診断する。

keystoreのパス、alias、パスワードは外部設定から受け取る。秘密情報をnpm配布物、生成テンプレート、ログ、ビルド結果へ含めない。CIでも利用できる入力方法を用意し、ローカルの対話入力だけに依存しない。

署名済みAPKについて署名の検証と端末へのインストールを行う。同じapplication IDと鍵を用い、versionCodeを上げた更新版をインストールして、保存データを引き続き利用できることを確認する。秘密鍵の保管とversionCodeの増加は利用者の管理事項として説明する。

AABを公開対象にする場合は、AABの署名とbundletoolによる端末別APKの生成・インストールも検証する。APKSは検証用の派生成果物とし、主な配布形式にはしない。Playへの公開や審査は今回の作業に含めない。[公式のコマンドラインビルド・署名手順](https://developer.android.com/build/building-cmdline)

## 7. FCMの扱い

現在のFCM本体は未実装である。今回のAndroid公開ビルドをFCMの実装完了に依存させず、未提供の状態でもWebViewと基本Muon APIを使用できるようにする。

FCMを追加する場合は明示的に有効化する。無効なアプリにはFirebase設定を要求せず、関連SDK、サービス、権限を追加しない。有効化に必要な設定が不正な場合はビルド時に診断する。端末上の権限拒否やFCM初期化の失敗が、通常の画面やMuon APIを停止させないようにする。

今回FCMを提供しない場合は、有効化を要求された時点で未対応と説明する。動作しないAPIを成功したように見せない。将来、制限付きで提供する場合は、受信できる条件と保証しない範囲を実装・テスト・文書で一致させる。

永続inbox、再配送、ACK、バックグラウンド処理などの詳細は[plan5.md](plan5.md)へ残す。これらを今回すべて実装する必要はない。

## 8. QuickJSのJavaScriptデプロイメント方針

### 8.1 今回確定する方針

| 項目 | 方針 |
| --- | --- |
| 入力 | 利用者がバックエンドJavaScriptのエントリーポイントを指定する |
| ビルド場所 | 開発PCまたはCI上で依存コードをまとめる |
| 出力形式 | 初期対応は単一のESMソースとし、QuickJSが提供する組み込みモジュールは別扱いにする |
| 配置 | WebView用アセットと区別し、APK/AABへ同梱する |
| 更新単位 | Androidアプリの更新と同時にJavaScriptも更新する |
| 識別情報 | アプリ用コードのハッシュと、必要なランタイム機能を記録する |
| データ | 実行コードと永続データの保存先を分ける |
| 初期対象 | 同梱できるESMと、明示した対応済み組み込みAPIを対象にする |

固定の`backend.mjs`を利用者のアプリ用bundleへ置き換える方向とする。Muonが提供するランタイム初期化コードと、利用者が提供するアプリコードを分けて管理する。エントリーポイントと依存を開発PC上で解決し、Android端末上でnpmや任意のパッケージ探索を実行しない。

初期対応ではJavaScriptの独立ダウンロード更新を扱わない。実行コードはインストールされたアプリのアセットから読む。将来、展開キャッシュを設ける場合は、コードのハッシュとランタイムの識別情報で区別し、更新前のコードを誤って再利用しない方式にする。アプリ更新で永続データを上書きしない。

### 8.2 実装を後続へ回せる範囲

利用者コードのbundle処理、コードの識別情報を記録するmanifestの生成、QuickJSへの接続、実機実行の完成は今回のマージ条件にしない。必要な入力、出力、配置、更新単位が本節の方針に沿って説明できればよい。

公開API名と設定キーは正式提供前に確定する。試作の`muon.node.createNode()`が実際のNode.jsを返すとは説明せず、QuickJSであることと対応範囲を明示する。仮のAPIを使用可能な公開機能として掲載しない。

CommonJS、動的な`require`、native addon、Node-API、任意のNode.js互換性を初期対応の前提にしない。dynamic import、JavaScript以外のアセット、source map、WASMの対応範囲は、同梱処理を実装する前に確定する。対応できない入力はビルド時に診断する。

### 8.3 未提供時に維持する動作

QuickJSを無効にした通常アプリは、QuickJSの初期化やサービス接続を待たずに起動する。試作用の`backend.mjs`や試験専用APIを利用者の成果物へ混入させない。無効な構成ではQuickJS固有のサービス、ネイティブライブラリ、アセット、権限を追加しない。

有効化を要求されてもまだ使用できない場合は、ビルド時に未対応と診断する。試作コードとその回帰テストを保持することは、正式な公開機能として有効化することとは区別する。FCM受信のためにQuickJSを必須にしない。

## 9. テストとE2E

### 9.1 独立した利用者プロジェクトから確認する

リポジトリ外の一時ディレクトリへViteとTypeScriptの利用者アプリを作り、`npm pack`したmuon-uiをインストールする。workspaceリンクや元リポジトリの相対パスを使用しない。

そのアプリで環境準備、公開CLIによるdebug APK生成、release APK生成と署名を実行する。標準アプリではNDKとCMakeを利用できない環境でも成立することを検証し、事前ビルド配布による簡略化を確認する。

画面操作、基本Muon API呼び出し、ファイル保存と読込、再起動後のデータ保持、更新APKでのデータ保持を検証する。開発サーバーを停止した状態でも同梱画面を使用できることを確認する。

既存のrelease検証はWebViewのページ読み込み完了ログを待つ。利用者E2Eでは、これに加えてアプリの操作結果とAPIの応答を確認する。ページの読み込みだけでアプリが正しく動いたとは判定しない。

### 9.2 Android端末と成果物の検証

既存のx86_64・Android API 37・16 KiBエミュレーター、arm64-v8a・API 37・4 KiBのPixel 6を基本の検証環境とする。serial、ABI、OS、ページサイズを確認し、対象外の端末で成功した結果を代用しない。

debug instrumentation全件と、利用者アプリのrelease APKを両環境で検証する。AABを公開する場合は、AAB由来の端末別APKも両環境で確認する。arm64・16 KiB環境で実行できない場合も、arm64成果物の整列検査は行い、実行未検証であることを記録する。

すべてのネイティブ依存についてABI、エントリーポイント、必要な共有ライブラリ、ELF整列、APK内の配置を検証する。ビルド設定だけから16 KiB対応を判断しない。[16 KiB対応の公式資料](https://developer.android.com/guide/practices/page-sizes)

`minSdk=24`という設定と、実際に検証したOS・WebViewの範囲は別である。最低対応環境で起動と基本APIを確認するか、保証する範囲を検証済みの条件へ合わせる。必要なWebView機能がない場合は、その理由を説明できる動作にする。

### 9.3 回帰・失敗条件・CI

設定の優先順位、SDK不足、バージョン不一致、署名不足、非対応機能の指定を検証する。FCMとQuickJSを使わない構成で基本動作を確認し、desktopビルドがAndroid SDKを必要としないことも検証する。

RPCのorigin・main frame制限、アセットの拒否動作、プラグインの登録制限、Activity再生成など、既存テストの検証内容を移行後も維持する。外部依存や既存機能の失敗をskipして完了扱いにしない。

[現在のCI](../../../.github/workflows/ci.yml)はルートの`npm run test`を実行する。Android workspaceの通常テストはinstrumentation APKを生成するが、接続端末での実行は別コマンドである。CIではAndroid用ツールチェインを明示的に準備し、npm配布物からのビルドとエミュレーター検証を組み込む。物理端末の検証は再現できるコマンドと結果を残す。

待機時間だけで成功を判断せず、アプリの応答、状態、処理完了イベントで同期する。2026年10月7日の利用者の指示により、Androidの画面操作もADB・instrumentationで検証する。Playwright MCPはこのセッションに接続されていない。利用できない検証手段や端末があれば未実施として記録し、該当する完了条件を未達のまま残す。

アイコンなどの期待画像を追加する場合は目視で確認する。アニメーションなど時間軸の挙動を検証する必要が生じた場合は、動画を記録して結果を確認するテストを追加する。

## 10. インクリメンタルな実施順序

各ステップで実行可能な検証アプリを維持する。型や配置だけを先にすべて変更して、最後までビルドできない進め方にはしない。

コード変更は要求や不具合を再現するテストを先に追加してREDを確認し、修正後にGREENを確認する。適切な粒度で`feat:`、`fix:`、`refactor:`、`chore:`、`doc:`のコミットを作る。不要になったコードは削除し、機能を削除する場合は削除を確認するテストのRED/GREENと一旦のコミットを経て、その一時テストも整理する。

### ステップ0. 現行動作と公開範囲を確定する

ルートの全体テストと既存Android端末テストを実行し、開始時点の結果を記録する。既知失敗の記録を現在の失敗と決めつけず、再現した問題だけを独立した修正として扱う。

保証するビルド環境、最低OSとWebView、AAR配布、公開設定、初期プラグイン入力、AABの採否、FCM・QuickJSの未提供時の動作を確定する。端末がない間も設計やホスト側の調査を進められるが、端末検証の完了とは扱わない。

成果物は、再現できる既存試作の検証結果と、更新した公開範囲である。完了条件は、後続の回帰判定に使える全体・端末テスト結果があり、ビルドと配布の対象範囲が明示されていることである。

### ステップ1. Android実装を分離して既存アプリを維持する

製品用コードを内部workspaceへ移し、試作アプリからそのコードを利用する。アプリ固有のID、アセット、設定を明示入力に変え、リポジトリ探索への依存を除く。試験用処理は検証アプリ側へ置く。

成果物は、分離後の実装から生成した試作相当のdebug APKである。完了条件は、APKの起動と既存の基本API・プラグインテストが成功し、製品用コードが試作ディレクトリを暗黙に参照しないことである。QuickJS試作を維持する場合も、共通コードをコピーして二重管理しない。

### ステップ2. npm配布とツールチェイン準備を成立させる

標準ランタイムのAAR、依存メタデータ、テンプレート、renderer初期化コードを配布物へ収録する。プラグイン登録情報を共通ランタイムから分離し、通常のアプリ作成でネイティブ再ビルドを不要にする。ツールチェインの検出・不足診断・準備を実装する。

成果物は、npm配布物をリポジトリ外へインストールして生成したdebug APKである。この段階では内部ビルドAPIを使う小さな検証ドライバーを利用してよい。完了条件は、元リポジトリとNDK/CMakeへ依存せず標準アプリを作れ、不足環境を説明でき、APKが起動することである。

### ステップ3. 利用者プロジェクトを公開CLIとViteへ接続する

`android`ターゲット、設定の解決、成果物型、`prepare`と`build`の経路を追加する。利用者のWebアセットとMuon API初期化を同梱し、アプリID、表示名、バージョン、アイコン、権限を生成する。FCMとQuickJSは未提供でも通常アプリが成立する構成にする。

成果物は、独自画面と設定を持つ利用者アプリのdebug APKである。完了条件は、公開CLIとVite設定から同じ意味の成果物を作れ、端末上で画面操作、基本Muon API、ファイル保存が成功することである。Androidを選ばないdesktopビルドも成功させる。

### ステップ4. 配布用署名と更新を完成させる

`muon pack --target android --type apk`を実装し、外部の署名設定を利用する。秘密情報の扱い、署名不足の診断、成果物情報を検証する。AABを公開する場合は同じ設定解決処理から生成する。

成果物は、利用者の鍵で署名したrelease APKと、その更新版である。完了条件は、端末への新規インストール、通常操作、同じ鍵での更新、保存データの保持が成功することである。AABを含める場合は端末別APKも動作させる。

### ステップ5. 最終E2E、文書、マージ判定を完了する

最終のnpm配布物から独立した利用者プロジェクトを構築し、環境準備から署名・端末操作までを通して検証する。全体テストとAndroid端末テストを実行し、ステップ0からの回帰がないことを確認する。

利用者向けの導入、設定、ビルド、署名、インストール、デバッグ、制限事項を日本語で記述し、それを原本として英語版を整える。QuickJSのデプロイメント方針とFCMの後続範囲を、この計画と矛盾させない。

成果物は、検証済みnpm配布物、利用者アプリのAPK、再現手順、テスト結果、利用者文書である。マージ前には、次節の「developへのマージ結果」を除いた必須項目を満たすことを確認する。その後、developとの差分と競合を確認してマージし、結果の確認までをステップの完了条件とする。競合解消などでコードを変更した場合は、マージ結果に対して必要な検証をやり直す。

## 11. developへのマージ完了条件

- [x] Androidツールチェインを文書に記載した手順で準備でき、不足や不整合を説明できる。
- [x] npm配布物だけでMuonのAndroid部品を解決でき、元リポジトリやサブモジュールを参照しない。
- [x] 標準アプリのビルドでMuonのネイティブ部品を再ビルドせず、通常利用者にNDK/CMakeを要求しない。
- [x] 新規の利用者プロジェクトから公開CLIとVite設定でdebug APKを生成できる。
- [x] 利用者の画面、アプリ情報、アイコン、Muon設定が反映され、試作用ページやバックエンドコードが混入しない。
- [x] 利用者の鍵で署名したrelease APKを生成でき、署名不足を成功扱いにしない。
- [x] release APKの新規インストール、画面操作、Muon API、ファイル保存、再起動、更新後のデータ保持が成功する。
- [x] arm64-v8aとx86_64の成果物を検査し、必須のVMとPixel 6で検証が成功する。
- [x] 16 KiB整列をネイティブ依存とAPK配置について検査し、実行未検証の環境を区別して記録する。
- [x] 最低対応OSとWebViewの条件、非対応時の診断が実装と文書で一致する。
- [x] FCMとQuickJSが未提供・無効でも通常のMuonアプリが動作し、使用できない機能を明示する。
- [x] QuickJSの入力、ESMへのbundle、APK/AAB同梱、アプリと同時更新、識別情報、永続データ分離の方針が確定している。
- [x] Android SDKのないdesktop利用者へ追加の準備を要求せず、既存機能の全体テストが成功する。
- [x] npm配布物からの独立E2E、Android端末テスト、ルートの全体テストの実行結果を記録する。
- [x] 利用者文書と実装が一致し、FCM・QuickJSを含む後続作業の範囲が明示されている。
- [x] AABを公開する場合は、署名とAAB由来の端末別APKの動作検証が成功する。 今回は公開対象外。
- [x] developへのマージ結果を確認し、必要な回帰検証が完了している。

テスト結果には対象コミット、コマンド、ツールチェイン、端末条件、成果物のハッシュ、成功・失敗・未実施を記録する。未実施の必須条件がある間はマージ完了としない。

## 12. 作業上の制約と後続課題

作業中に不足するパッケージや端末が判明した場合は、必要な準備を利用者へ伝える。失敗や時間切れを根拠なく無視せず、必要なら待機方法やタイムアウトを見直す。計画に起因する問題が見つかった場合は、場当たり的な修正を重ねる前に本計画を更新する。

外部サブモジュールやvendorコードを直接変更しない。既存のMuon管理下のpatchとビルド手順を再利用し、必要な変更は再現できる形で管理する。新たに外部APIを使用する際は公式文書とAPIコメントの両方を確認する。

JavaScript/TypeScript側は既存のnpm、TypeScript、Vite、Vitestと、指定された整形・バージョン管理の構成に従う。自由に補助スクリプトを選べる場合はNode.jsを使用し、ビルド時のテキスト処理には`funcity`を使用する。新しい公開型・関数には説明を付ける。

機能実装の完了時には時間がかかっても全体テストを実行する。この計画書の保存のように、ビルドへ影響しない文書だけを変更する場合はビルド確認を必要としない。

FCMの通知配送、QuickJSの正式な実行APIと利用者コードの同梱実装、独立したJavaScript更新、実Node.js、独自ネイティブプラグインの汎用ビルド、Windows/macOSでのAndroidビルド、高度な開発時連携は後続候補として扱う。今回の必須作業へ追加する場合は、追加理由と完了条件を本計画へ反映してから着手する。

## 13. 実施記録

### ステップ0の確定事項（2026年10月7日）

- 公開ホストはLinux x64。Node.jsの条件は既存パッケージに合わせる。AGP 9.2.1、Gradle 9.4.1、SDK 37、Build Tools 36.0.0を固定する。JDKは17以上のGradle対応版を必要とし、実行検証はJDK 25.0.3で行う。
- 公開する形式はdebug APKと利用者の鍵で署名したrelease APK。AABは試作の回帰テストに残し、今回は公開しない。
- ランタイムの内部Java名はdev.muon.runtimeとし、利用者のapplication IDから独立させる。AARとPOMはdist/android/maven、Gradle雛形とWrapperはtemplates、初期化スクリプトはrendererへ収録する。
- 最低インストールAPIは24を維持する。保証する実行環境は今回検証するAPI 37とし、古いOSの動作保証は追加検証まで保留する。WebViewの必要条件はWEB_MESSAGE_LISTENER、WEB_MESSAGE_ARRAY_BUFFER、DOCUMENT_START_SCRIPTの機能検出で判定し、不足時は画面に理由を表示する。
- Android設定はmuon.jsonのandroid節とビルドAPIのandroidオプションで受け取る。優先順位はAPI/Viteの明示指定、muon.json、package.json由来の既定値とする。CLIは既存のアプリ設定・出力オプションを再利用する。applicationId、label、versionCode、versionName、abis、icon、permissions、SDKの場所を扱う。
- ネイティブプラグインの初期入力は事前ビルド済み共有ライブラリと登録情報に限定する。登録情報はアプリのアセットとして読み込み、共通AARにアプリごとの登録名・ポリシー・設定をコンパイルしない。独自C/C++ソースのコンパイルは後続とする。
- prepareは既存JDK・SDKを検出して検証し、Wrapperを使える状態にする。不足SDKのインストールコマンドを案内する。SDK・JDKの自動インストールとライセンスの代理受諾は行わない。
- FCM・QuickJSは公開アプリでは未提供とし、有効化の指定をビルドエラーにする。未指定・無効のアプリには関連サービス、ライブラリ、アセット、権限を加えない。
- 画面操作は利用者の回答によりADB・instrumentationへ変更した。独立した利用者アプリに対する操作とAPI応答を検証し、製品に試験用ブリッジを入れない。

### 開始時の検証

対象は506e932。Pixel 6はAPI 37、arm64-v8a、4 KiBページ、serialはadb-23231FDF600652-Nj8Dyu._adb-tls-connect._tcp。エミュレーターはemulator-5556、API 37、x86_64、16 KiBページで実行した。

- ANDROID_SERIALを各端末へ指定したnpm run test:androidとnpm run test:android:pixel6は成功した。各端末でinstrumentation 44件とrelease APK・AAB由来APKSの検証が成功した。
- ルートnpm testはmuon-builderのtest_launcher_progress.shでWindows用ハーネス終了後のwineserver -wが終了しなかった。テスト専用prefixのサービスがXサーバー終了後も残っていることを確認し、約10分待機後に当該テストを中断した。他のworkspaceの実行結果は別途追記する。
- この問題はテストの終了処理として独立修正する。ハーネスの終了コードを確認した後、専用prefixのサービスを停止して待機する。Windows製品コードやWine本体は変更しない。

### ステップ1の実施

共通renderer、基本Android API、RPC、JNIとprocess runtimeをmuon-androidへ移した。Java/JNIの内部名はdev.muon.runtimeへ統一した。試作のActivity、QuickJS、テスト用画面は検証アプリに残した。共通CMakeへ依存先と生成registryの場所を明示して渡す構成にした。

muon/config.jsonのアプリ設定を基本APIから取得するテストを追加した。追加直後はapplicationSettingが返らず失敗し、設定の読み込み実装後に成功した。共通側29件、試作側23件のVitestと、エミュレーター・Pixel 6のinstrumentation各44件が成功した。アプリIDはdev.muon.prototypeのままで内部Java名を変更できることも確認した。

端末テストのコマンドがrelease APKを作り直さず、古いAPKを使用する問題を今回のクラス名変更で再現した。test:androidとtest:android:pixel6へassembleReleaseを加え、現行ソースから作るようにした。debug APKのSHA-256は8ddb8a5d9c1fd9f7ffe7b1289662e372d8565d4404af692f9db9685b241a346c、release APKは15200df70d1592e3def847fe25bac51b318aa89b4209ecfce87cb2ad049cc82b。

### 開始時検証の補足

開始時の全体テストは終了した。Android 52件、muon-node 40件、muon-coreのCTest 42件、muon-core-testerの209件が成功した。muon-core-testerには環境条件による既存の26件のskipがあった。muon-uiは307件中1件がWindows接続先のNode.js不足で失敗した。利用者がNode.jsをインストールしてagent-roverを再起動し、Windows E2Eの再実行は2件とも成功した。muon-builderは前述のWine待機を中断したため、開始時の全体テスト自体は失敗として記録する。最終判定では全体を再実行する。

SDK Platformの実際のパッケージ名はplatforms/android-37.0である。compileSdkとtargetSdkの数値は37とし、配布するtoolchain.jsonでパッケージ名も固定する。

### ステップ2の実施

共有ランタイムをdev.muon:runtime:0.1.0のAAR/POMとして生成した。npmのdist/androidにはmaven、renderer、templates、toolchain.json、licensesを配置する。ステップ2の検証用に内部ビルドAPIをlibへ同梱した。build_package.shとmuon-uiのビルドからも収録処理を呼び出す。

アプリのプラグイン登録はmuon/plugins.jsonへ移した。ネイティブライブラリを変更せず登録設定を変えるテストは、変更前に旧設定値が返って失敗し、移行後に成功した。試作と通常アプリは同じWebViewホストを利用する。通常アプリにはQuickJSサービス・ライブラリ、試験用プラグイン、試験用ページを含めない。起動に必要なWebView機能やアプリ設定が不足する場合は、起動失敗の理由を画面に表示する。

独立アプリの検証コマンドは次のとおり。ANDROID_HOMEとANDROID_SERIALを指定し、npm packで作ったtgzを渡す。検証アプリには試験用RPCブリッジを追加せず、画面に表示した基本APIの応答をADBから確認する。

```bash
node muon-android/scripts/test-packaged-application.mjs /path/to/muon-ui.tgz
```

検証ドライバーはリポジトリ外へtgzをインストールする。SDKにはplatforms、build-tools、licensesだけを公開し、NDKを除外する。追加したcompilerガードでCMake・C/C++コンパイラの起動も拒否する。標準アプリのGradle処理は事前ビルド済みライブラリを使用し、同梱アセットから起動する。

エミュレーターとPixel 6の両方で、独立アプリのビルド・インストール・起動とgetRuntimeInfo/getConfigValuesの応答を確認した。初回実行では共通ランタイムに残っていたcardio試験用プラグインの必須条件により失敗した。この条件を試験ホストだけに適用し、両端末で成功した。ライセンス追加前の両APKのSHA-256は同一の7bda7a9614b82f4a145be5f7ecc37f9accb0b433b90c2e77d5cd539488737f60。画面も目視確認した。

共通側Vitest 32件、共有WebViewホストへ移行後のVM instrumentation 44件、試作workspaceの通常テストとネイティブ依存・プラグイン・release成果物の検査も成功した。公開CLI、ViteからのAndroidビルド、利用者の署名はステップ3以降で接続する。

### ステップ3の実施

公開ターゲットにandroidを追加した。既定値と--allは従来のdesktopターゲットを維持する。Androidの結果にはAPK、applicationId、ABI、variant、署名状態を返し、desktopのlauncherやCEFに相当する架空の値を加えない。CLIのprepare/buildとViteのbuild設定を接続し、アプリID、ラベル、バージョン、PNGアイコン、権限、設定値を反映した。設定の優先順位と非対応機能の指定は、失敗するテストを追加してから実装した。

初期の公開プラグイン入力はandroid.pluginsの事前ビルド済みELFとallow/configに限定する。選択ABIのライブラリ、公開エントリーポイント、SONAME、依存ライブラリ、16 KiB整列をビルド前に検査する。登録名がJava側の条件と異なる問題もテストで再現し、同じ条件へ合わせた。公開モードはsimpleとし、ViteではpluginAccess: falseを指定する。validateモードはビルド時に拒否する。browser.initialWindowStateのfullscreen指定も未提供とし、実行時のfullscreen APIを案内する。

muon.fsの相対パスはアプリ専用のfiles領域を基準にした。追加したinstrumentationは変更前にEROFSで失敗し、修正後は既存分を含む45件がVMで成功した。絶対パスと相対シンボリックリンクの意味は維持する。SDKのディレクトリ名だけでなくsource.propertiesのAPI・Build Toolsバージョンも検証する。JDKの対応範囲外も診断する。

独立した利用者プロジェクトをVite・TypeScriptのアプリへ変更した。公開CLIのprepare/build、TypeScriptの型検査、直接のvite buildを実行し、CLIとViteが同一APKを生成することを確認した。NDKを除いたSDKとコンパイラ起動ガードも維持した。

Pixel 6では単発のuiautomator dumpが出力後に終了コード137となり、継続的な画面検証に使えなかった。画面とアプリのログでは正常動作していたため、製品コードは変更せず、独立したobserver APKのUI Automator instrumentationへ移した。接続を維持して表示内容を待ち、保存ボタン、ページ再読込回数、API応答、プロセス再起動後のデータ保持を確認する。observerは利用者APKに含めない。これは第9節で許可されたADB・instrumentationによる検証である。

通常アプリはVMとPixel 6で成功した。両者のdebug APKのSHA-256は1cd8a729f0eadc641f2fc442d1899869e7da0697d18cacb6fd2fe7fc48e8d06c。画面とアイコンを目視確認し、明るい背景で判読できるようシステムバーの文字色も調整した。--pluginsを付けた検証では、利用者プロジェクトへコピーした事前ビルド済みプラグインの加算、設定値、非許可関数を公開しない動作がVMで成功した。

共通側Vitestは39件が成功した。muon-uiの関連184件の初回実行では、build:jsで消えたnativeツールの不足によりWindowsアイコン関連4件が失敗した。ツールの再生成・再配置後は184件すべて成功した。desktopビルドではAndroid SDKを参照せず、Android専用設定も実行用設定へ埋め込まない。全体テストと両端末の最終検証はステップ5で改めて行う。

### ステップ4の実施

muon pack --target android --type apkを実装した。android.signingにはkeystore、keyAlias、storePasswordEnv、必要ならkeyPasswordEnvを指定する。パスワードの値は環境変数で渡し、Gradleの生成設定や成果物情報へ書き込まない。署名設定の不足、平文パスワードの設定、エラー出力からの秘密値漏洩をテストした。Webアセットにkeystoreのコピーがある場合も、ファイル名に依存せずビルドを拒否する。

releaseビルドを利用者の鍵で署名し、apksignerによる検証とzipalignによる16 KiB配置の検査を行う。検証後の成果物だけをartifacts/apkへ配置する。debug鍵への代替は行わず、Androidの既定debug証明書もrelease署名として受け付けない。成果物には署名状態と公開証明書のSHA-256を記録する。

独立E2Eをreleaseまで拡張した。テスト用の鍵を利用者プロジェクト外へ作り、versionCode 2のrelease APKを新規インストールした。保存・再読込・再起動を行い、同じ鍵のversionCode 3へ更新した。observerは更新後の表示バージョン1.0.1と保存済みデータを確認する。初回保存前は空であることも確認するため、既存データで保存操作の失敗を見落とさない。

VMとPixel 6の両方でこのE2Eが成功した。VMのrelease APKのSHA-256はc599f3191a43ff26b5982fcdb8f8e093b2212a4ac1e4c55582de561b04bad4b9、更新版はffa1ca3bd8626f9c7c6a74e4e28012d0d44cd0ef16a1b389902af60ca80f51a2。テストでは実行ごとに別の一時鍵を作り、同一実行内の更新で証明書が一致することを確認した。Pixel 6ではNDK/CMakeが追加インストールされていないことも確認した。共通側Vitest 42件、muon-uiの関連187件が成功した。

### ステップ5の実施

独立E2EにAPK自体の検査を加えた。アプリID、表示名、PNGアイコン、バージョン、権限、debuggable、署名を確認する。両ABIのELFのLOAD整列・offset・依存関係とAPK内の16 KiB配置を検査し、Firebase、QuickJS、instrumentationのクラスが通常アプリへ含まれないことも確認する。プロトタイプの試験用プラグインは、利用者が事前ビルド入力として明示したE2Eだけに含める。

CIにJDK 25.0.3、固定版SDK・NDK・CMakeの準備と、API 37.1のx86_64・16 KiBエミュレーターでの独立npm E2Eを追加した。JDKとエミュレーター用Actionはcommitで固定する。SDK Command-line Tools 23.0のアーカイブは公式配布元のチェックサムを確認したうえでSHA-256を固定した。GitHub Actions上での実行結果は、ローカルの検証結果とは区別する。

最終全体テストの初回では、既存テストの準備不足を3件検出した。Windows E2Eが手動配置する依存リストにfuncityがなく、配布スクリプトのfixtureにはAndroidステージ処理がなかった。CIテストの期待値も従来の45分とAction一覧のままだった。製品の依存定義と計画のCI追加は正しいため、テスト環境と期待値を更新した。失敗を確認した後の個別再実行では、Windows E2Eを含む22件とCIの1件が成功した。接続先WindowsのNode.jsは24.21.0で正常に動作している。

### 最終端末・配布物の検証

製品コードと検証ドライバーの最終変更はabd8659。利用者文書を加えた89a4757をビルドしてnpm配布物を生成した。その後の4dba1b9は文書のみの変更である。成果物とログはリポジトリ内のartifacts/plan6へ保存した。このディレクトリはGit管理の対象外であり、署名の秘密鍵は含めていない。

| 項目 | 検証環境 |
| --- | --- |
| ホスト | Linux x64、Node.js 24.15.0、JDK 25.0.3 |
| Android製造 | AGP 9.2.1、Gradle 9.4.1、SDK Platform android-37.0 revision 2、Build Tools 36.0.0、NDK 29.0.14206865、CMake 4.1.2 |
| 利用者ビルド | Platform・Build Tools・licensesだけを参照するSDK、C/C++コンパイラ起動禁止、元リポジトリへの参照なし |
| Pixel 6 | API 37、arm64-v8a、4096 byteページ、WebView 153.0.8010.36 |
| エミュレーター | API 37、x86_64、16384 byteページ、WebView 149.0.7827.5、Emulator 37.2.12 |
| エミュレーターイメージ | system-images/android-37.1/google_apis_ps16k/x86_64 revision 9 |

再現コマンドは次のとおり。ANDROID_HOMEはSDKのパス、ANDROID_SERIALは接続端末を指定する。実行したserialは、VMがemulator-5556、Pixel 6がadb-23231FDF600652-Nj8Dyu._adb-tls-connect._tcpである。

```bash
npm run build --workspace muon-ui
mkdir -p /tmp/muon-plan6-final-package
npm pack --workspace muon-ui --pack-destination /tmp/muon-plan6-final-package
ANDROID_SERIAL=emulator-5556 npm run test:android --workspace muon-android-prototype
ANDROID_SERIAL=adb-23231FDF600652-Nj8Dyu._adb-tls-connect._tcp npm run test:android:pixel6 --workspace muon-android-prototype
ANDROID_SERIAL=emulator-5556 node muon-android/scripts/test-packaged-application.mjs /tmp/muon-plan6-final-package/muon-ui-0.0.1.tgz
ANDROID_SERIAL=adb-23231FDF600652-Nj8Dyu._adb-tls-connect._tcp node muon-android/scripts/test-packaged-application.mjs /tmp/muon-plan6-final-package/muon-ui-0.0.1.tgz
ANDROID_SERIAL=emulator-5556 node muon-android/scripts/test-packaged-application.mjs /tmp/muon-plan6-final-package/muon-ui-0.0.1.tgz --plugins
```

両端末でinstrumentation 45件、試作release APK、AAB由来APKSの回帰検証が成功した。通常の独立アプリは両端末で、プラグインを追加した独立アプリはVMで成功した。それぞれ、CLI/Viteのdebug APK一致、releaseの署名、保存・再読込、プロセス再起動、同一鍵でversionCode 2から3へ更新した後のデータ保持を確認した。画面も目視で確認した。プラグイン構成では加算、設定値、非許可関数を公開しない動作も成功した。

| 配布物 | SHA-256 |
| --- | --- |
| muon-ui-0.0.1.tgz | 78ec391fc922a62a7fd0deb85617145dd7684301f6b286910f5904dfeadffb49 |
| 通常debug APK、両端末共通 | 365d0fc4ef14d1196502bd471e2208fd5587c55e3b9347f9e87c4010547dd7d6 |
| Pixel 6、release versionCode 2 | d255f66d935cd4e2386630b929a82add0ee39da714958e32327e2db533727d97 |
| Pixel 6、release versionCode 3 | 645926a7eadfe5f943a6705596a5c4c4dcb6fb061943dfe263a986bc226abbd5 |
| VM、release versionCode 2 | d1a532bc941bd63f4f462e7f838d73cf65412a6ee8cd8b284fc0a1b059d95849 |
| VM、release versionCode 3 | 9b61b1e350af214e3f05985322d513a4b3347fab07bc34fa8387be45afdba7a1 |
| プラグイン追加VM、release versionCode 3 | e884eaa95429a895d105c0bacefcc0c31eaa1aad64abbea8acb4dd7493920163 |

通常APKはABIごとにlibc++_shared.so、libcardio.so、libmuon_android_rpc.soを含む。全LOAD segmentの整列は16384 byteで、依存ライブラリは同梱物かAndroidシステムライブラリに解決できた。全APKでzipalignの16 KiB検査と署名検証が成功した。npm内のAAR・POM・renderer・Wrapper・型定義を確認し、試作・observer・QuickJS・keystoreを含まないことも検査した。

古いOS、arm64での16 KiB実行、Windows/macOSでのAndroidビルド、GitHub Actions上での実行は未検証として残す。FCM・QuickJS・公開AABは今回の公開範囲に含めない。利用者向けの導入から署名・更新までをdocs/ja/android.mdへ記述し、英語版をdocs/en/android.mdへ反映した。

### 全体テストとマージ前の判定

4dba1b9のソースに対してルートのnpm testを再実行し、終了コード0で完了した。muon-android 42件、試作23件、muon-node 40件、muon-ui 324件、muon-coreのCTest 42件、muon-core-tester 209件が成功した。muon-builderのシェル検証も完了した。muon-core-testerの条件付きskipは開始時と同じ26件である。Windows E2Eはmuon-uiの324件に含まれる。最後のCEF検証は797.90秒かかった。ログはartifacts/plan6/final-all-green.logへ保存した。

第11節のマージ前の必須条件を照合し、すべて満たした。日本語の計画・Android利用手順・セルフビルド手順はyomiyasuで確認し、lintの指摘はない。全体テスト開始後の追記はこの計画書だけで、製品コード、テスト、ビルド設定は変更していない。

### developへのマージ結果

origin/developを取得して分岐がないことを確認し、ローカルdevelopを8b3386cからfeature/androidの14eacc2へfast-forwardでマージした。競合はなく、マージ直後の作業ツリーもクリーンだった。全体テストを実行した4dba1b9との差分は本計画書だけで、製品コード・テスト・ビルド設定が一致することをgit diffで確認した。マージ結果を記録するこの追記も文書のみの変更であり、追加のビルドは必要ない。

これにより第11節の全条件を満たし、plan6を完了した。ローカルdevelopへの取り込みまでを実施し、リモートへのpushとnpmへの公開は行っていない。検証用に起動したエミュレーターは終了した。

### GitHub Actionsで判明した環境依存への対応

developへのリモート反映後、[CI実行37567894842](https://github.com/kekyo/muon-ui/actions/runs/37567894842/job/112619706210)の全体テストが失敗した。Androidの実行テストには到達していない。CodeQLの3ジョブは成功している。

原因は2点ある。Androidの依存ビルドはlibffiのHEADとv3.8.0タグを照合するが、CIの浅いチェックアウトにはタグがない。公式配布アーカイブのSHA-256固定は維持し、[公式v3.8.0](https://github.com/libffi/libffi/releases/tag/v3.8.0)のコミット12ffd1f9dc56fcea79d2f742f424301ae668d663と直接照合する。サブモジュールの内容は変更しない。

ExpressのE2Eは独立したfixtureのlockfileを使うため、ルートのnpm ciが取得するバージョンとは一致しない。今回はcontent-type 2.0.0がキャッシュになく、--offlineで失敗した。ローカルに残っていたキャッシュで準備不足を見落としていた。[npmの--prefer-offline](https://docs.npmjs.com/cli/v11/using-npm/config/#prefer-offline)でキャッシュを優先し、不足する固定版を取得する。lockfileによるバージョン固定と整合性検証は維持する。

追補作業は次の順序で進める。

1. タグなしの浅いチェックアウトで依存ビルドを実行する回帰テストを追加する。変更前の失敗を確認し、コミットの直接照合へ修正して成功させる。
2. Express E2Eの一方を空の専用npmキャッシュで実行し、変更前の失敗を確認する。LinuxとWindows向け準備のインストール指定を修正し、実際のHTTP応答と終了処理を再検証する。
3. ルートのnpm testを最後まで実行し、Androidの配布物と16 KiBエミュレーターでの独立アプリも再検証する。結果をここへ記録してコミットする。

完了条件は、両原因の再現テストと全体テストが成功し、Androidの配布から署名APKの更新までを確認できること。GitHub Actions上の再実行結果は、ローカル検証とは分けて記録する。

タグを持たないdepth 1のチェックアウトを一時ディレクトリに作り、依存ビルドの失敗を再現した。コミットの直接照合へ変更した後は、arm64-v8aとx86_64の両方でlibffi.aの生成とmanifestのハッシュ検証が成功した。回帰テストの所要時間は26.93秒だった。公式アーカイブのSHA-256とサブモジュールのコミット検査は維持している。

修正後に生成したnpm配布物でも、16 KiBエミュレーターのinstrumentation 45件と試作release APK・APKSの検証が成功した。独立プロジェクトではNDKを使わず、CLI/Viteのdebugビルド、署名release APKの生成、画面操作、再起動、versionCode 2から3への更新後のデータ保持まで成功した。最終画面のVersion: 1.0.1と保存内容も目視確認した。配布物・APK・画面・ログはartifacts/plan6-ciへ保存し、検証用エミュレーターは終了した。

| 配布物 | SHA-256 |
| --- | --- |
| muon-ui-0.0.1.tgz | 53b2091f88078b38b5e2c59a82f894cd06cda59770d2e02760ed1478f2bbb3c3 |
| VM、release versionCode 3 | 5ca61aaa05350bcad27e3c5a5702ffc90504d58220f86c8759d2d841d64f4635 |

Expressの回帰テストも、空の専用キャッシュでは変更前にENOTCACHEDとなることを確認した。Linuxの2箇所とWindows向けfixture準備を--prefer-offlineへ変更した。修正後の全体テストでは、独立ExpressアプリのHTTP応答と、listenerが開いた状態でのNodeプロセス終了の両方が成功した。

ルートのnpm testは終了コード0で完了した。muon-android 43件、試作23件、muon-node 40件、muon-ui 324件、muon-coreのCTest 42件、muon-core-tester 209件が成功した。muon-builderのシェル検証とmuon-uiのWindows E2Eも通過した。muon-core-testerのskipは既存と同じ26件で、CEF検証の所要時間は802.50秒だった。全体ログはartifacts/plan6-ci/muon-ci-all.logへ保存した。

追補の完了条件を照合し、再現テスト、全体テスト、Androidの配布から署名APKの更新までの検証がすべて成功した。修正はローカルdevelopへコミットする。GitHub Actions上での修正後の実行は未確認であり、push後のCI結果で別途確認する。

### CIのウィンドウ操作テストで判明したページ遷移の競合

[CI実行37574326651](https://github.com/kekyo/muon-ui/actions/runs/37574326651/job/112639710070)では、前回修正したタグなしチェックアウトの回帰テストとExpress E2Eの2件が成功した。今回はLinuxのページ内ドラッグ領域を検証するテストが、遷移準備中にInspected target navigated or closedで失敗した。ウィンドウのドラッグ操作には到達していない。全体では208件成功・1件失敗・既存の26件skipとなり、後続のAndroid端末E2Eは実行されなかった。

このテストはlocation.hrefを変更した直後、ページ内のPromiseで移動先の要素を待っていた。Promiseの評価が移動前の文書で始まると、遷移時に実行コンテキストが破棄されて失敗する。同じ処理がWindowsの対応テストにもある。既存のCdpDriver.navigateはフレームとloaderを照合し、遷移・読み込み完了の通知を待つため、両テストをこの操作へ揃える。

対応は、遷移先URLと読み込み完了の検証を追加して失敗を確認し、Linux・Windows両方の遷移待ちを変更する。その後、ウィンドウ操作の対象テストとルートのnpm testを実行する。完了条件は、遷移先の準備完了後に既存のクリック・スクロール・ドラッグ検証が成功し、全体テストも成功すること。Androidの製品コードとビルド設定は今回の変更対象に含めない。

修正前の失敗は上記CIログで確認した。手元では遷移先の状態を検証するassertionを追加し、対象テストを8回実行したが、いずれも成功して競合を再現しなかった。ローカルで再現できたとは扱わず、CIの失敗記録とコード上の待機条件を根拠に修正する。両OSの処理をCdpDriver.navigateへ変更した後、Linuxの対象テストは遷移先URL・document.readyStateの検証を含めて成功した。Windowsは既存のagent-rover接続先で、windows-amd64の対応テストを確認する。

Windows実機テストの準備では、試験用muon_test_plugin_cardio.cppのコンパイルが失敗した。Android対応中のdbe872c5で追加したdispatcherProbeが、POSIXのpipeとcardio::from_fdを無条件に使っていた。ルートのnpm testに含まれるWindows E2Eはパッケージングを検証するもので、Windows向けコアの試験プラグインをビルドしていなかった。この回帰確認の不足を追補する。

失敗したwindows-amd64ビルドを再現記録とし、FDの試験処理とその登録を[cardioが提供するCARDIO_HAS_POSIX_FD](https://github.com/kekyo/libcardio/blob/31e8149ce52cdfdaaa48838ca9de39288074885c/README.md#L764)の有効時に限定する。全プラットフォーム共通のdispatcher初期化・呼び出し・終了の検証は維持する。修正後はWindowsのウィンドウ操作と共通cardio検証を実機で実行し、AndroidのFD検証もinstrumentationで確認する。製品ランタイムや外部ライブラリには変更を加えない。

条件分岐の修正後、windows-amd64のDebug・ReleaseビルドとCDP relayのビルドが成功した。Androidも16 KiBページのエミュレータでinstrumentation 45件、Release APK/APKSの検査、Release APKのインストール・起動を再検証し、すべて成功した。AndroidのdispatcherProbeは引き続きFD・タイマー・ワーカーからの完了通知を検証している。

Windows実機への配置は、最初の2回ともFailed to send binary transfer: write ECANCELEDで中断した。agent-roverのログ画面から保存先を読み取り、稼働中のログをPowerShellのGet-Content経由で取得した。ログには両試行とも、処理プロセスとの通信がThe pipe has been endedで終了した記録があった。通常のfile.readでは共有違反になるため、書き込み中のログを共有して読める方法を使った。

Windows側のagent-roverは0.7.0で、[ファイル転送の既定上限は64 MiB](https://github.com/kekyo/agent-rover/blob/7b12bfdc9d3bb6b626fc4e610706987510c4b385/README.md#agent-limits-advanced-topic)だった。今回のCEF DLLは264,940,544バイト、Debug実行ファイルは291,381,916バイトで上限を超えていた。同じ実行ファイルを別ポートの一時インスタンスとして--max-transfer-size 512付きで起動すると、両ファイルの転送が成功した。クライアント0.5.0とのバージョン差を理由に依存パッケージを更新する必要はなかった。既存インスタンスは維持し、一時インスタンスは検証後に終了する。今後WindowsのコアE2Eを実行する際も、Debug実行ファイルを扱える転送上限を指定する。

転送後の最初の実行では、3件ともCDP接続時のHTTP 500で失敗した。接続先にはホスト名が設定されていたが、[ChromiumのCDPはHostヘッダーをIPアドレスまたはlocalhostに制限する](https://chromium.googlesource.com/chromium/src/+/refs/heads/main/content/browser/devtools/devtools_http_handler.cc)。同じ端末のIPv4アドレスをAGENT_ROVER_WIN11_HOSTに指定して再実行すると、3件すべてが33.20秒で成功した。クリック・スクロール・ドラッグ操作、cardioの初期化・呼び出し、非同期終了をWindows実機で確認できた。204件のskipは今回の実行対象を3件に絞ったためである。一時agent-roverを終了し、元のインスタンスが引き続き利用できることも確認した。

CIの失敗ログ、ローカルの修正前8回・修正後のLinux検証、Windowsビルドの修正前後と実機検証、Android再検証のログはartifacts/plan6-ci2へ保存する。agent-roverのログも認証情報を除いて保存し、転送失敗時の記録を残す。

ルートのnpm testは終了コード0で完了した。muon-android 43件、試作23件、muon-node 40件、muon-ui 324件、muon-coreのCTest 42件、muon-core-tester 209件が成功した。muon-builderのシェル検証とmuon-uiのWindows E2Eも成功した。muon-core-testerは既存と同じ26件skipで、CEF検証は819.97秒だった。全体ログもartifacts/plan6-ci2/muon-ci2-all.logへ保存した。

追補の完了条件を照合し、Linux・Windows両方で遷移先の読み込み完了後にウィンドウ操作を検証できること、全体テストが成功することを確認した。追加で修正した試験プラグインも、Windowsのビルド・実機実行とAndroidのFD検証が成功した。修正をローカルdevelopへコミットする。修正後のGitHub Actionsは未実行であり、push後のCI結果で別途確認する。

### CIのAndroidエミュレータで使われるCLIの不一致

[CI実行37580305378](https://github.com/kekyo/muon-ui/actions/runs/37580305378/job/112658199602)では、前回修正したページ遷移の検証を含むルートのnpm testが成功した。続くAndroid E2Eでは、約15 MBの試験用APKをインストールする段階でRequested internal only, but not enough spaceが発生し、instrumentationの開始前に停止した。独立npm利用アプリのビルド・署名・更新検証にも到達していない。

CIはAPI 37.1・google_apis_ps16k・x86_64・Pixel 6のAVDを新規作成するが、データ領域の容量を指定していなかった。手元で成功したPixel_6 AVDはAndroid Studioで作成したもので、disk.dataPartition.sizeが10Gだった。既存AVDでの成功だけを確認し、CIで新規作成するAVDとの差を検証していなかった点を見直す。

最初の対応案は、CIと同じ条件で一時AVDを新規作成してインストール失敗を再現し、[android-emulator-runnerのdisk-size入力](https://github.com/ReactiveCircus/android-emulator-runner/blob/a421e43855164a8197daf9d8d40fe71c6996bb0d/README.md#configurations)で必要な容量を明示することだった。公開入力の説明と実装を確認し、この入力がAVDのdisk.dataPartition.sizeへ反映されることを確認した。

修正後は新規AVDで、CIと同じ順序で16 KiBページの確認、instrumentation、Release APKの起動、npm packから独立利用アプリの署名APK更新までを実行する。ルートのnpm testも実行する。完了条件は、新規AVD上でこれらがすべて成功すること、既存のPixel 6実機と手元のAVDに変更を加えず一時AVDを終了・削除できることである。変更範囲はCIのSDK準備・エミュレータ設定と本計画の検証記録とする。

手元のCommand-line Tools 23では、新規AVDも10Gで作成された。CIの[ランナーイメージにはCommand Line Tools 12.0が入っており](https://github.com/actions/runner-images/blob/ubuntu24/20260927.320/images/ubuntu/Ubuntu2404-Readme.md#android)、android-emulator-runnerはその実行パスを優先していた。[Command-line Tools 12の公式配布物](https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip)を一時ディレクトリで調べ、既定の内部ストレージ容量が800 MiBであることを確認した。配布元のチェックサムとの一致も確認した。

同じCLI 12・システムイメージ・Pixel 6プロファイルで一時AVDを新規作成すると、disk.dataPartition.sizeは800Mだった。起動後の/dataは全792,608 KiBに対して空き7,636 KiBとなり、試作アプリのAPKインストールはCIと同じRequested internal only, but not enough spaceで失敗した。これをローカルでのRED確認とする。既存のSDKやAVDは変更していない。

検証のためdisk-sizeを4Gに指定し、起動後の/dataの容量もログに出力した。CLI 23による予備検証では、4G指定に対してエミュレータが実容量を6 GiBに補正し、instrumentation 45件とRelease APKの起動が成功した。ただし、CIで使われるCLI 12で作った新規AVDでも確認する必要があった。

CLI 12で4Gを指定するとAPKのインストールは成功したが、SurfaceFlingerがAssertion failed: !rcEnc->featureInfo()->hasReadColorBufferDmaで終了し、Androidのシステムプロセスが再起動を繰り返した。instrumentationはProcess crashedとなり、テスト開始前に停止した。CLI 12のAVDは、ルートのiniファイルにtarget=android-0を出力していた。CLI 23ではtarget=android-37.1となる。[エミュレータはこのtargetからAPIレベルを判定する](https://android.googlesource.com/platform/external/qemu/+/refs/heads/emu-master-dev/android/emu/avd/src/android/avd/info.c)ため、容量だけを指定する当初の案では、新しいイメージに対して古いAPI用の設定が使われる問題も残っていた。

対応方針を、製造用に取得済みのCLI 23をエミュレータ作成にも使用するよう変更する。[android-emulator-runnerのSDK準備処理](https://github.com/ReactiveCircus/android-emulator-runner/blob/a421e43855164a8197daf9d8d40fe71c6996bb0d/src/sdk-installer.ts)はSDKのcmdline-tools/latestをPATHの先頭に追加するため、GitHub Actionsでは[公式の配置方法](https://developer.android.com/tools/sdkmanager)に従って、検証済みのCLIをこの位置へ配置する。既存CLIはランナーの一時ディレクトリへ退避する。容量の追加指定は取り除き、CLI 23が生成する設定で十分な容量と正常な起動を確認する。起動後の/data容量のログ出力は残す。

追加の完了条件は、ランナーにCLI 12がある状態から修正後の準備手順を実行し、actionと同じPATHでもCLI 23が選択されること、新規AVDでAndroid検証の全工程が成功することである。CLI、エミュレータ、システムイメージのコードは変更しない。

修正したワークフローから準備スクリプトを取り出し、CLI 12をlatestに置いた一時SDKで実行した。公式ZIPのSHA-256検証とSDK準備は終了コード0で完了した。actionと同じPATHを設定すると配置済みのCLI 23が選択され、新規AVDにはtarget=android-37.1、16 KiBページ用のタグ、10Gのデータ領域が設定された。

ルートのnpm testは終了コード0で完了した。muon-android 43件、試作23件、muon-node 40件、muon-ui 324件、muon-coreのCTest 42件、muon-core-tester 209件が成功した。muon-uiのWindows E2Eとmuon-builderのシェル検証も成功した。muon-core-testerは従来と同じ26件skipで、CEF検証は846.20秒だった。

修正後の新規AVDで、CIのAndroid検証スクリプトも終了コード0で完了した。16 KiBページ、instrumentation 45件、Release APKとAPKセットの起動、npm packからの独立利用、署名APKの生成・起動・再起動・データを保持した更新を確認した。最終画面ではバージョン1.0.1と保存済みデータを目視でも確認した。検証後の/dataには約8.2 GiBの空きがあった。

検証後のログ確認では、バックグラウンドの試作アプリでWebView 149.0.7827.5のonTrimMemoryからSIGILLが1件記録されていた。追加確認として、公開パッケージから作成したアプリをバックグラウンドへ移し、ADBでBACKGROUNDのメモリ解放通知を送ると同じクラッシュを再現した。HIDDENの通知と復帰は成功した。CIで発生した容量不足・SurfaceFlingerの再起動とは別に、muonの処理とシステムWebViewを切り分ける必要がある。muonを含まない最小WebViewアプリを一時ディレクトリで作り、同じ通知で再現するかを確認する。製品コードの変更は原因が分かるまで行わない。

最小アプリはAndroid標準のActivityに[WebView](https://developer.android.com/reference/android/webkit/WebView)を1個配置し、固定のHTMLを読み込むだけとした。muonやAndroidXには依存しない。このアプリでもBACKGROUND通知後に同じlibwebviewchromium.so内の命令位置でSIGILLが発生し、WebView側の問題をmuonなしで再現できた。確認した範囲はAPI 37.1・x86_64・16 KiBページとWebView 149.0.7827.5の組み合わせである。この問題は未解決として残し、対応案は更新後のWebViewまたはシステムイメージで同じ最小アプリを検証し、その後に独立利用アプリのバックグラウンド復帰を再検証することとする。

CIの失敗再現、CLI配置修正後の検証、全体テスト、署名APKと画面、WebView単体の再現コードとログをartifacts/plan6-ci3に保存した。一時AVDと一時SDKを終了・削除し、利用アプリの一時署名鍵も削除した。Pixel 6実機への接続は維持し、実機と既存AVDは変更していない。

今回のCI修正の完了条件を照合し、CLIの選択、新規AVDでのAndroid検証、全体テスト、一時環境の後片付けが完了したことを確認した。変更をローカルdevelopへコミットする。修正後のGitHub Actionsは未実行で、push後の結果は別途確認する。追加で再現したWebViewの問題は、このCI検証の成功とは分けて扱う。

### WebViewのバックグラウンド時クラッシュの追加検証

利用者から、実際にmuonアプリが強制終了する可能性と、アプリ側または実行環境側で取れる回避策を明らかにするよう依頼を受けた。修正後のCIは利用者が実行中であり、この調査ではWebViewの問題を扱う。

前回は、API 37.1・x86_64・16 KiBページ・WebView 149.0.7827.5の環境で、試作アプリがバックグラウンド中に自然発生したクラッシュと、公開パッケージ利用アプリおよび最小WebViewアプリへのBACKGROUND通知による再現を確認した。自然発生時の通知レベルは記録できていない。SIGILLというシグナル名だけで、CPUの命令不足やWebView内の意図的な停止のどちらかと断定しない。

調査開始時点のPixel 6はAndroid 17・API 37・arm64・4 KiBページで、WebViewは153.0.8010.36だった。エミュレーターとの差にはOS・CPU・ページサイズ・WebViewの版が含まれるため、実機で再現しない場合も、版の更新だけで解決したとは扱わない。

次の単位で調査を進め、各段階で再実行できるコード、ログ、結果をartifacts/plan6-webviewへ保存する。

1. 既存の最小APKと公開パッケージ利用アプリで、起動、HOMEへの移動、HIDDEN通知、BACKGROUND通知、復帰を検証する。PIDとクラッシュ記録を照合し、再起動によって障害が隠れていないことも確認する。Pixel 6と隔離したAPI 37.1のAVDで同じ手順を使い、端末・WebViewの版を記録する。既存の利用アプリのデータは保持する。
2. インストール済みの別システムイメージや、入手可能な公式WebView配布物を用いて、再現条件を比較する。同じOSでWebViewだけを替えられる場合は優先する。できない場合は条件差を明記する。クラッシュ位置の逆アセンブルと対応するChromiumの公開ソースも確認し、停止命令と通知経路を調べる。外部コードは改変しない。
3. 根拠のある回避案を最小アプリで試し、有効なものをmuon利用アプリでも検証する。通知を握り潰すだけの処理や非公開APIの利用は製品の回避策に採用しない。バックグラウンド復帰後の表示・RPC・保存データも確認し、回避の代償や適用範囲を記録する。実機のシステムWebViewは変更せず、環境を替える試験は隔離AVDで行う。
4. 判明した原因、実証した影響範囲、未確認の範囲、回避策の有効性を本計画へ記録する。製品コードの変更が必要と判明した場合は、この計画を具体化してから再現テスト、修正、成功確認の順で実施する。最後にルートのnpm testを実行し、検証用のプロセス・AVD・実機の診断用APKを片付ける。

完了条件は、Pixel 6と既知の再現環境の結果を区別して示せること、実アプリの強制終了が起きる条件を証拠とともに説明できること、回避策を実測結果と制約付きで提示できることとする。回避策が確認できない場合は、その事実と試した範囲を記す。特定の組み合わせでの成功を、未検証のすべてのAndroid端末への保証としない。公開パッケージ利用アプリの表示・RPC・データ保持、全体テスト、一時環境の後片付けも完了条件に含める。

調査途中で、WebViewの停止位置は[ChromiumのPmfUtilsによるメモリ量の計算](https://chromium.googlesource.com/chromium/src/+/refs/tags/149.0.7827.5/base/android/pmf_utils.cc)と特定できた。エミュレーターの/proc/pid/statmでは常駐ページ数が共有ページ数より少なくなり、その差を非負のByteSizeへ変換する際にUD2へ到達していた。[稼働カーネル214d1615c480のtask_statm](https://android.googlesource.com/kernel/common/+/214d1615c480/fs/proc/task_mmu.c#98)には、x86の16 KiBエミュレーションで共有ページ数を二重に換算する処理が残っている。[Android側の修正772e4465](https://android.googlesource.com/kernel/common/+/772e4465d7db4282b797c28310aa45861804ddcf)も、この処理によってresident < sharedになる問題を説明している。

この証拠に合わせ、版比較はWebView 149のAPKを4 KiB環境へ配置する試験と、別カーネルを使う既存の16 KiBシステムイメージとの比較を優先する。必要なら、隔離AVDの起動条件だけを変更して16 KiBエミュレーションの有無を比較する。OSから自然に届く通知も診断アプリで記録し、ADBによる疑似通知だけで発生する問題かを確かめる。アプリ側の候補としてWebViewのバックグラウンド破棄を試すが、効果がなければ製品へ組み込まない。カーネルやWebViewを自前で改変する対応は行わない。

#### 原因と通常利用への影響

2026年10月7日の追加検証で、API 37.1・google_apis_ps16k・x86_64のシステムイメージrevision 9に含まれるカーネルの不具合と切り分けた。カーネルは6.12.69-android16-6-g214d1615c480-ab15053784、WebViewは149.0.7827.5である。WebViewの版を変えず、同じイメージ・同じカーネルの起動条件を4 KiBにすると、最小アプリとmuon利用アプリの両方でクラッシュがなくなった。

このカーネルのtask_statmは、共有ページ数を4 KiB単位から16 KiB単位へ換算した後、常駐ページ数の計算でも共有ページ数を再び換算する。そのため、/proc/pid/statmが返す常駐ページ数が共有ページ数より少なくなる場合がある。最小アプリの自然発生時には、通知直前の記録がresident=7676、shared=11346だった。WebViewは両者の差を私有メモリ量として計算するため、負数を扱えないByteSizeへの変換で停止する。

ネイティブライブラリの停止位置0x65dac39にはUD2命令があり、その直前にresident - sharedが負数かを調べる分岐があった。参照する文字列と/proc/self/statm・statusの読み込みもPmfUtilsの実装と一致した。BuildIdはfe49a4ff595b9ff446040a9a7148405da7e1cbd8で、試作・公開パッケージ利用・最小WebViewアプリの各クラッシュが一致する。これはCPUがWebViewの通常の処理命令を実行できない現象ではない。

[ChromiumのMemoryPressureMonitor](https://chromium.googlesource.com/chromium/src/+/refs/tags/149.0.7827.5/base/android/java/src/org/chromium/base/memory/MemoryPressureMonitor.java)は、BACKGROUND通知からプロセス凍結前のメモリ処理を呼ぶ。[PreFreezeBackgroundMemoryTrimmer](https://chromium.googlesource.com/chromium/src/+/refs/tags/149.0.7827.5/base/android/pre_freeze_background_memory_trimmer.cc)がその際にメモリ量を集計する。HIDDEN通知からの復帰では今回のクラッシュを再現しなかった。

ADBでメモリ通知を送らず、HOMEへ移動するだけの試験でも再現した。最小アプリでは17:03:13にHIDDEN通知を受け、17:04:13にOSからBACKGROUND通知が届き、同じ位置でSIGILLになった。公開パッケージから作ったmuon APKも、17:06:39のHOME移動から約61秒後に自然にクラッシュした。ApplicationExitInfoはAPP CRASH(NATIVE)、status=4を記録していた。したがって、この環境では通常のアプリ切り替えでも強制終了に遭遇し得る。発生までの約60秒は今回の環境での観測値であり、すべての端末で同じ時間になるという意味ではない。

停止するのはmuonアプリのメインプロセスであり、JavaScriptの例外処理で回復できる範囲ではない。FCM、QuickJS、muonのRPC、ネイティブプラグインを含まないアプリでも起きるため、それらの利用を避けてもこの原因は除けない。

#### 比較試験

各反復でアプリのデータを保持したまま新しいプロセスを起動し、HIDDEN通知と復帰、BACKGROUND通知と復帰の順で試験した。通知の前後でPIDが一致することと画面内容を確認した。Androidが疑似通知のレベルをプロセスごとに記憶するため、反復の間だけforce-stopしている。クラッシュ後の自動再起動は成功に数えない。

| 環境 | WebView | 最小アプリ | 公開パッケージ利用アプリ |
| --- | --- | --- | --- |
| Pixel 6、Android 17、arm64、4 KiB | 153.0.8010.36 | 両通知・復帰が各5回成功 | 両通知・復帰が各5回成功 |
| API 37.1 revision 9、x86_64、16 KiB、6.12.69 | 149.0.7827.5 | HIDDEN成功、BACKGROUNDでSIGILL。HOME移動だけでも再現 | 同左 |
| 同じAPI 37.1イメージ・同じカーネル、起動時に4 KiBを指定 | 149.0.7827.5 | 両通知・復帰が各3回成功 | 両通知・復帰が各3回成功。保存後の追加1回も成功 |
| API 35、x86_64、4 KiB、6.6.50 | 同じ149のAPKを配置 | 両通知・復帰が各3回成功 | 両通知・復帰が各3回成功 |
| API 36 revision 7、x86_64、16 KiB、6.6.66、RAM 4 GiB | 同じ149のAPKを配置 | 両通知・復帰が各3回成功 | 両通知・復帰が各3回成功。保存後の追加1回も成功 |

Pixel 6のビルドはgoogle/oriole/oriole:17/CP3A.260905.009/16091614:user/release-keysだった。実機のWebView、OS設定、既存アプリのインストール状態は変更していない。muon利用アプリには検証前からsaved-on-deviceが保存されており、通知・復帰とプロセスの再起動後も保持されていた。通知後の画面操作でファイルの書き込み・読み込み、RPCによるページ再読込も成功し、再読込回数が1から2へ増えた。同じ機能は4 KiBの比較AVDとAPI 36の16 KiB AVDでも成功した。

API 36での最初の試験は、エミュレーターのRAMが約2.5 GiBの状態で、HOME移動時にOSがアプリをLOW_MEMORYとして終了させた。ネイティブクラッシュ記録はなく、今回のSIGILLとは区別する。RAMを4 GiBにして再実行した結果を上表に記載した。Pixel 6ではuiautomatorのXML出力完了後に、操作ツール自身が終了コード137になる場合があった。出力完了メッセージと生成済みXMLを確認し、アプリのPID・表示・クラッシュ記録とは別に判定した。

#### 回避策と適用範囲

開発時の回避策として、通常の4 KiB AVDが使える。さらに原因を確認するため、問題のAPI 37.1イメージに対して起動引数の末尾へ-qemu -append page_shift=12を追加した。[カーネルが受け取るpage_shiftの処理](https://android.googlesource.com/kernel/common/+/214d1615c480/mm/page_size_compat.c)も確認し、起動後のgetconf PAGE_SIZEが4096であることを実測した。この条件では同じWebView 149と同じmuon APKが動作し、保存内容を保持した通知・復帰とRPCの検証も通過した。

4 KiBでの成功は16 KiB対応の検証を代替しない。16 KiBのまま確認する場合は、今回成功したAPI 36 revision 7・カーネル6.6.66-android15-8-gd0c43a640eab-ab13812146・RAM 4 GiBが候補になる。将来、API 37.1のイメージを更新する際は、上流の修正を含むカーネルになったことと、同じバックグラウンド試験の成功を確認する。調査時点の[Google公式イメージ一覧](https://dl.google.com/android/repository/sys-img/google_apis/sys-img2-4.xml)では、該当イメージの最新は使用中と同じrevision 9だった。上流に修正があることだけで、SDKから取得できるイメージも修正済みとは扱わない。

アプリ側の回避案として、Activity.onStopでWebViewを親Viewから外してdestroyし、復帰時に作り直す処理を最小アプリで試した。[WebView.destroyの前提](https://developer.android.com/reference/android/webkit/WebView#destroy())に従って破棄しても、BACKGROUND通知からのメモリ集計は残り、同じ位置でクラッシュした。破棄完了のログに続いてtrim=40、resident=7785、shared=11535を記録している。この案には回避効果がないため、muonの製品コードには追加しない。

今回、アプリから公開APIだけで安全に回避できる方法は確認できなかった。通知の握り潰しやWebView内部のコールバック解除は、通常のメモリ管理に干渉するため採用しない。WebViewの版更新だけで解消するとも確認できていない。153.0.8010.36の[PmfUtilsの公開ソース](https://chromium.googlesource.com/chromium/src/+/refs/tags/153.0.8010.36/base/android/pmf_utils.cc)にも同じ差分の変換が残るので、Pixel 6での成功をWebView更新の効果とは扱わない。

特定した不具合は、x86で4 KiBページを16 KiBに見せる処理で換算が重複するものだった。Pixel 6の通常利用でこの不具合を再現する証拠はなく、5回の通知・復帰とRPC・保存データの検証では正常だった。arm64の実16 KiB端末は今回の実測対象に含めていない。未検証の端末や、別のOS・WebViewの不具合まで含めて強制終了しないことを保証するものではない。

この結論から、muonの製品コード、FCMの方針、JavaScriptの配置・パッケージングを変更する必要はない。今後のCIには、テスト用プロセスを生存確認するバックグラウンド復帰試験と、対象カーネルの確認を加える余地がある。今回は原因と回避策の調査として記録し、CI設定の変更は行っていない。

#### 記録と後片付け

比較に使った操作スクリプトはartifacts/plan6-webview/memory-probe.mjs、復帰後のRPC・保存データの検証はconsumer-after-trim.mjs、自然なバックグラウンド移行の観測はnatural-background.mjsに保存した。最小アプリの元のソースとAPKはartifacts/plan6-ci3/webview-only、通知ログと破棄処理を追加した診断アプリはartifacts/plan6-webview/lifecycle-probeにある。診断アプリの変更だけで試験し、muonの製品コードには変更を加えていない。

各実行の環境情報、PID、画面のXML、ApplicationExitInfo、クラッシュログと、成功したRPC操作後の画面をartifacts/plan6-webviewに保存した。WebViewのAPKとSHA-256、逆アセンブル、文字列の参照位置、対応する上流コード、カーネル修正の差分も保存している。これらの外部コードは参照用であり、改変・ビルド・組み込みは行っていない。

検証用の4個のAVDを終了して削除し、Pixel 6へ追加した診断APKも削除した。既存のSDK・AVDと、Pixel 6の既存アプリは保持している。実機と比較環境の最終画面では、ready表示、保存内容、バージョン1.0.1、再読込回数2を目視確認した。

ルートのnpm testは最後まで実行し、終了コード8だった。muon-android 43件、試作23件、muon-node 40件、muon-ui 324件、muon-core-tester 209件が成功した。muon-core-testerは既存と同じ26件skipで、CEF検証は856.56秒だった。muon-uiのWindows E2Eとmuon-builderのシェル検証も成功した。

失敗はmuon-coreのCTestに含まれるtray_linux_dbusの1件で、fixed tray icon had unexpected initial bytesという結果だった。単独で再実行すると成功し、続いてCTest全42件を再実行しても成功した。ただし、初回の全体実行が成功したとは扱わない。現在のテストは2個のトレイを登録し、通知の到着順から固定アイコンと追従アイコンを区別している。順序への依存が不安定要因の候補だが、今回の再実行では再現せず、原因の確定や修正は行っていない。Androidのクラッシュとは別の残課題として記録する。全体ログ、単独再実行、CTest全件再実行のログもartifacts/plan6-webviewへ保存した。

追加調査の完了条件を照合し、Pixel 6と再現環境の比較、通常操作での自然発生、カーネルの原因特定、回避環境での通知・復帰・RPC・データ保持、全体テストの実行と結果の記録、後片付けを完了した。アプリ側の破棄処理には回避効果がなく、製品への追加は不要と判断した。全体テストの初回失敗とarm64の実16 KiB端末が未検証である点は残る。今回の変更は本計画の記録のみとし、ローカルdevelopへコミットする。

#### 主要端末への修正配信と影響範囲の確認

2026年10月7日、利用者から主要端末へのAndroid修正の配信状況を調べるよう依頼を受けた。修正が必要になるCPU・ページサイズの条件を先に確認し、上流ブランチへの取り込み、SDKイメージの配布、端末のOTA配信を調べた。完了条件は、主要な実機で修正を待つ必要があるかを説明し、公開情報で確認できた配布状況と確認できない範囲を区別して記録することとした。

今回特定した二重換算の不具合は、x86_64で4 KiBのページを16 KiBに見せる環境に限られる。[再現カーネルのpage_size_compat_defs.h](https://android.googlesource.com/kernel/common/+/214d1615c480/include/linux/page_size_compat_defs.h#38)では、CONFIG_X86_64以外の__PAGE_SHIFTは常にPAGE_SHIFTと同じになる。換算関数__page_size_countの除数は__PAGE_SIZE / PAGE_SIZEなので、arm64では4 KiB・16 KiBのどちらも1になる。したがって、修正前の計算式でも共有ページ数は縮小されず、この二重換算によるresident < sharedは生じない。これはソースからの判断であり、すべての端末を実測した結果ではない。[AOSPの16 KiB説明](https://source.android.com/docs/core/architecture/16kb-page-size/16kb)も、arm64のネイティブ16 KiBと、x86_64での16 KiBシミュレーションを区別している。

| 対象 | 今回の不具合に対する判断 | 確認した範囲 |
| --- | --- | --- |
| 接続中のPixel 6 | この修正の配信を待つ必要はない | arm64、4 KiB、カーネル6.1.162、セキュリティパッチ2026-09-05。前回の実機試験で通知・復帰が各5回成功 |
| Pixel・Galaxy・Xiaomiなどのarm64端末、4 KiB | 二重換算の発生条件に該当しない | 上記のカーネル実装から判断。機種別の実測ではない |
| arm64端末、ネイティブ16 KiB | 同じく発生条件に該当しない | 上記のカーネル実装から判断。実16 KiB端末の動作試験は未実施 |
| API 37.1・google_apis_ps16k・x86_64、revision 9 | 修正済み環境への移行か、検証済みの別環境が必要 | 前回クラッシュしたイメージと同じrevisionが、現在も公式一覧の最新版 |

前回記した「arm64の実16 KiB端末は未検証」は、実機試験の範囲を示す。今回のソース確認により、この二重換算についてはarm64のネイティブ16 KiBも影響範囲から除外できる。これを、16 KiB対応に関する別の不具合や、WebView全般のクラッシュが起きないという保証には広げない。

Androidの公開レビューをChange-Id Iad11b73859278b65820a17df40bcc41b8009c0e4で照合した。取り込み日はGerritのsubmittedをUTCで記載する。

| 上流ブランチ | 状態・取り込み日 | 修正 |
| --- | --- | --- |
| android17-6.18 | MERGED、2026-08-19 | [feb997b9、レビュー4239843](https://android-review.googlesource.com/c/kernel/common/+/4239843) |
| android16-6.12 | MERGED、2026-08-19 | [772e4465、レビュー4240502](https://android-review.googlesource.com/c/kernel/common/+/4240502) |
| android16-6.12-2026-06 | MERGED、2026-08-24 | [a7ecd26f、レビュー4245582](https://android-review.googlesource.com/c/kernel/common/+/4245582) |
| android16-6.12-2025-12 | ABANDONED | [レビュー4241563](https://android-review.googlesource.com/c/kernel/common/+/4241563)。このレビューを取り込み済みの根拠にはしない |

[Googleの10月Pixel更新告知](https://support.google.com/pixelphone/thread/471669543/google-pixel-update-october-2026?hl=en-GB)では、10月6日から端末・通信事業者ごとに段階的に配信するとしている。[Pixelの10月更新一覧](https://source.android.com/docs/security/bulletin/pixel/2026/2026-10-01)と[Samsungの10月SMR](https://security.samsungmobile.com/securityUpdate.smsb)には、今回の不具合番号548500630やtask_statmの修正に対応する記載を確認できなかった。Samsungも地域・機種によって配信時期が変わると説明している。これらの告知だけでは、当該コミットを含むOTAの機種別一覧や、利用者への導入率は分からない。一覧に記載がないことを、未修正の証拠にも使わない。[Xiaomiの更新ページ](https://trust.mi.com/misrc/updates/phone)は取得したHTMLに詳細が含まれず、当該修正の配信を確認する根拠は得られなかった。

一方、[Google公式SDKイメージ一覧](https://dl.google.com/android/repository/sys-img/google_apis/sys-img2-4.xml)は、system-images;android-37.1;google_apis_ps16k;x86_64についてrevision 9とx86_64-ps16k-37.1_r09.zipを返した。上流ブランチで修正済みでも、今回の再現環境で使う配布イメージの更新は確認できない。SDKイメージ更新後にカーネルとバックグラウンド復帰を再検証する方針は維持する。

muonのarm64アプリ配布を、この修正のOTA普及待ちにする必要はないと判断した。実機側で回避コードを追加する必要もない。現在必要な対応は、開発・CIで今回の不具合を持つx86_64の16 KiB環境を避け、既に成功した比較環境を使用することになる。端末ごとのOTA収録率や導入率は不明だが、主要なarm64実機でこの原因を回避できるかの判断は、配信率に依存しない。

レビューの取得結果、公開更新ページ、SDKイメージの該当項目、Pixel 6の構成の再確認結果をartifacts/plan6-webview/rolloutへ保存した。今回の完了条件を満たしたことを確認した。変更は調査記録のみで、製品コード・テスト・ビルドへの影響はないため、全体テストは再実行しない。

#### ローカルAVDのシステムイメージ更新

利用者からVMイメージの更新を依頼された。既存のPixel_6 AVDを対象として、次の順で進める。

1. 公式SDK一覧から更新候補を取得し、一時AVDでカーネルの修正、16 KiBページ、最小WebViewアプリとmuon利用アプリのバックグラウンド復帰を確認する。保存・読込・RPCによる再読込も検証する。
2. 既存AVDの設定・ディスク・スナップショットをバックアップし、ユーザーデータの一致を確認する。既存AVDを検証済みイメージへ切り替え、データを初期化せずに起動する。旧イメージに依存するスナップショットは退避する。移行後の起動・表示・データ保持・バックグラウンド復帰を確認し、起動できなければバックアップから戻す。
3. 更新後のAVDでAndroidのテストを実行し、ルートのnpm testも実行する。結果と復元に必要な場所を記録し、一時AVDを片付ける。

完了条件は、Pixel_6 AVDが修正を含む公式イメージで起動すること、16 KiBを維持すること、既存データを消去しないこと、今回のクラッシュの再現操作とmuonの機能検証に成功すること、Androidと全体テストの結果を記録することとする。実機Pixel 6、Pixel_4 AVD、製品コード、CI設定は今回の変更対象に含めない。

2026年10月7日に全パッケージを再確認すると、API 37.1はrevision 9のままだが、別パッケージのsystem-images;android-37.2;google_apis_ps16k;x86_64がrevision 6として公開されていた。前節の調査はAPI 37.1内の更新確認にとどまり、API 37.2への移行候補を見落としていた。「取得できる修正版がない」という説明は訂正する。配布元は[Google公式SDKイメージ一覧](https://dl.google.com/android/repository/sys-img/google_apis/sys-img2-4.xml)で、取得ファイルはx86_64-ps16k-37.2_r06.zipである。

[Android CLIの公式手順](https://developer.android.com/tools/agents/android-cli)に従い、既存SDKへAPI 37.2のイメージを追加した。含まれるカーネルは6.12.81-android16-6-g4f69fc7b210c-ab16167562で、[対応するtask_statmのソース](https://android.googlesource.com/kernel/common/+/4f69fc7b210c/fs/proc/task_mmu.c#98)には共有ページ数を二重換算しない修正が入っている。WebViewは再現環境と同じ149.0.7827.5だった。

移行手順は、データ保持の検証結果を受けて見直した。システムイメージの参照先だけを変更すると、端末のAndroid IDが変わり、既存の5個のアプリがなくなった。元のAVDはバックアップから復元した。バックアップのコピーを旧イメージで起動すると元のアプリが存在し、そのコピーを新イメージへ切り替えると同じ問題が起きた。スナップショットのディレクトリを残しても結果は変わらなかった。

[エミュレーターのdrive-share.cpp](https://android.googlesource.com/platform/external/qemu/+/refs/heads/emu-master-dev/android-qemu2-glue/drive-share.cpp)を確認すると、build.propのincremental版がversion_num.cacheの値と異なる場合、ユーザーデータ・暗号化情報・SDカードなどのQCOW2差分ディスクを作り直す。元の保存内容は差分側にあるため、設定変更とコールドブートだけでデータを維持できるという当初の想定が誤っていた。

以後の手順は、停止中のバックアップを読み取り、[qemu-img convertとcompare](https://www.qemu.org/docs/master/tools/qemu-img.html)で各差分と元ディスクを統合した独立ディスクを作り、論理内容の一致を検証する方法へ変更する。ユーザーデータ、暗号化情報、SDカード、キャッシュを一組として扱う。一時AVDで、更新前後のAndroid ID、既存5アプリ、保存データの一致を確認できた場合にだけ、既存AVDへ適用する。元のバックアップは変換の入力として保持し、外部コードとSDKの配布イメージは変更しない。

変換した4ディスクは、いずれも元のQCOW2との差分を含む論理内容が一致した。一時AVDへの適用で、更新前と同じAndroid ID、5アプリ、muon利用アプリのsaved-on-deviceが保持されることを確認した。その後、既存のPixel_6 AVDにも同じディスクを配置し、配置後の内容を再び比較して一致を確認した。設定はAPI 37.2のイメージとRAM 4 GiBへ変更し、データを保持したコールドブートに成功した。

最終構成は次のとおり。

| 項目 | 更新後 |
| --- | --- |
| AVD名 | Pixel_6。既存の名前を維持 |
| システムイメージ | API 37.2、google_apis_ps16k、x86_64、revision 6 |
| カーネル | 6.12.81-android16-6-g4f69fc7b210c-ab16167562 |
| ビルド | google/sdk_gphone16k_x86_64/emu64xa16k:17/CP41.260831.007/16416850:userdebug/dev-keys |
| ページサイズ | 16384バイト |
| WebView | 149.0.7827.5 |
| RAM | 4 GiB |

一時AVDでは、最小WebViewアプリとmuon利用アプリのHIDDEN・BACKGROUND通知と復帰が各3回成功した。通知を送らずHOMEへ移動する試験でも、18:16:20のHIDDENに続き、18:17:20にOSからBACKGROUNDが届いて処理を完了した。直前のstatmはresident=16149、shared=11233で、正常な大小関係になっていた。復帰後もPIDは9253のままで、同じWebView 149による以前のSIGILLは再現しなかった。

移行後の既存AVDでも、muon利用アプリの両通知・復帰が各3回成功した。保存済みノートは最初の表示から残っており、保存・読込・RPCによるページ再読込も成功した。再読込回数2と保存内容を画面でも確認した。さらにAndroidを再起動し、元のAndroid ID、5アプリ、保存済みノートが維持されることを確認した。移行のために既存アプリを再インストールする必要はなかった。

既存アプリの状態を保つため、APKを入れ替えるAndroidテストは、同じ更新済みイメージの一時AVDで実行した。ANDROID_SERIALで一時AVDだけを指定し、test:androidのinstrumentation 45件、Release APKとAPKセットの検証がすべて成功した。ルートのnpm testも終了コード0で完了した。muon-android 43件、試作23件、muon-node 40件、muon-ui 324件、muon-coreのCTest 42件、muon-core-tester 209件が成功し、既存の26件skipは維持された。Windows E2Eも成功した。前回失敗していたtray_linux_dbusも、今回は全体実行の中で成功した。

更新前の完全なバックアップはartifacts/plan6-avd-update/backup/Pixel_6.avdとPixel_6.iniに残した。元へ戻す必要がある場合はAVDを停止し、現在のAVDを退避したうえで、この2項目を/home/kouji/.android/avdへ戻す。元のAPI 37.1イメージもSDKに保持している。

更新情報、ディスク比較結果、移行前後の識別情報、画面、通知・復帰・RPCの結果、再起動後の検証、Androidテストと全体テストのログをartifacts/plan6-avd-updateへ保存した。一時AVD2個と作業用のディスクコピーは削除し、既存のPixel_6も検証後に停止した。AVD一覧は元と同じPixel_4とPixel_6である。今回の完了条件を照合し、修正済み公式イメージへの更新、16 KiBの維持、データ保持、muonの動作、Androidと全体テスト、後片付けが完了したことを確認した。
