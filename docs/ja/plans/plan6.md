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

この例のAndroid向け動作は未実装であり、完成後の操作案である。`prepare`は環境の確認・準備、`build`はインストール可能なdebug APK、`pack`は配布用release APKを担当する。署名情報は別途設定する。

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

- [ ] Androidツールチェインを文書に記載した手順で準備でき、不足や不整合を説明できる。
- [ ] npm配布物だけでMuonのAndroid部品を解決でき、元リポジトリやサブモジュールを参照しない。
- [ ] 標準アプリのビルドでMuonのネイティブ部品を再ビルドせず、通常利用者にNDK/CMakeを要求しない。
- [ ] 新規の利用者プロジェクトから公開CLIとVite設定でdebug APKを生成できる。
- [ ] 利用者の画面、アプリ情報、アイコン、Muon設定が反映され、試作用ページやバックエンドコードが混入しない。
- [ ] 利用者の鍵で署名したrelease APKを生成でき、署名不足を成功扱いにしない。
- [ ] release APKの新規インストール、画面操作、Muon API、ファイル保存、再起動、更新後のデータ保持が成功する。
- [ ] arm64-v8aとx86_64の成果物を検査し、必須のVMとPixel 6で検証が成功する。
- [ ] 16 KiB整列をネイティブ依存とAPK配置について検査し、実行未検証の環境を区別して記録する。
- [ ] 最低対応OSとWebViewの条件、非対応時の診断が実装と文書で一致する。
- [ ] FCMとQuickJSが未提供・無効でも通常のMuonアプリが動作し、使用できない機能を明示する。
- [ ] QuickJSの入力、ESMへのbundle、APK/AAB同梱、アプリと同時更新、識別情報、永続データ分離の方針が確定している。
- [ ] Android SDKのないdesktop利用者へ追加の準備を要求せず、既存機能の全体テストが成功する。
- [ ] npm配布物からの独立E2E、Android端末テスト、ルートの全体テストの実行結果を記録する。
- [ ] 利用者文書と実装が一致し、FCM・QuickJSを含む後続作業の範囲が明示されている。
- [ ] AABを公開する場合は、署名とAAB由来の端末別APKの動作検証が成功する。
- [ ] developへのマージ結果を確認し、必要な回帰検証が完了している。

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
