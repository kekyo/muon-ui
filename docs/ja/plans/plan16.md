# Linux配布版でシステムGTKを使う

## 目的と変更範囲

配布版からGTK関連の同梱ファイルを取り除き、実行先のシステムGTKを使う。2026年10月23日の追加指示に基づき、plan15のGTK同梱方針を変更する。作業開始時点は`feature/plan15`の`ed88ac3`である。Viteの異常終了通知など、GTKの配置に依存しない改善は維持する。

現在はGTK、GDK、GLib、GIO、GObjectと、それらを使うPango、ATK、画像ローダー、入力メソッドなどを一組で配布している。GTKだけを除去すると、システムGTKが同梱GLibを使って必要な関数を解決できない場合がある。そのため、この同梱グループとモジュール・キャッシュ・設定データ・ライセンスの配置処理を削除する。ランチャーやViteが追加する専用のライブラリーパスとGTK環境変数も削除し、利用者の環境を継承する。[GTKの実行環境と環境変数](https://docs.gtk.org/gtk3/running.html)

muonの既存の非同期ファイル操作にはGLib 2.74以上が必要で、`core/cef/CMakeLists.txt`もこの条件を要求している。[シンボリックリンク作成API](https://docs.gtk.org/gio/method.File.make_symbolic_link_async.html)と[上流のAPIコメント](https://github.com/GNOME/glib/blob/2.74.7/gio/gfile.c)で追加版を確認した。Ubuntu 20.04向けにGLibを独自ビルドして同梱する構成を廃止し、Debian 12のシステムGTK・GLibとGCC 12で配布物をビルドする。古いOS向けの互換処理は追加しない。実際の最低依存条件は、完成したELFとDebianのパッケージ情報から再計算する。[Debian 12のGTK](https://packages.debian.org/bookworm/libgtk-3-0)、[GCC](https://packages.debian.org/bookworm/gcc)

muon、CEF、cardioなどアプリの実行に必要な部品は維持する。削除対象はGTK同梱のための機能であり、ネイティブプラグインやアプリ資材の配布機能は維持する。外部コードは変更しない。Windows検証の資材は引き続き`C:\test\`配下を使う。

## 段階1：amd64配布版で同梱を廃止する

1. 現在の配布物にGTK関連ファイルが残り、実行時も同梱GTKが選ばれることを、削除確認用のテストで失敗として記録する。起動環境についても、専用パスが追加される現状を再現するテストを先に実行する。
2. ビルド基準をDebian 12へ変更する。GLib・GTKの独自ビルド、同梱ライブラリーの収集、モジュールと出自情報の生成・コピー・検証を削除する。依存関係の解析とdebの依存条件生成は維持する。
3. Vite、開発用supervisor、生成ランチャー、AppImageから同梱GTK向けの環境変更を取り除く。準備処理のモジュールキャッシュ生成と、ネイティブ起動時の専用GIOモジュール登録を削除する。呼び出し元がなくなった補助コードも削除する。
4. amd64配布物を再ビルドし、削除確認用テストの成功、関連テスト、`ldd`によるシステムGTK解決を確認してコミットする。

完了条件は、amd64配布物にGTK関連の同梱ファイルがなく、通常の起動環境でシステムGTK・GLibへ解決され、関連テストが成功することである。

## 段階2：起動と更新の継続的な検証へ整理する

1. 段階1のコミット後、機能削除のためだけに追加した一時テストを削除する。残すテストは、システムGTKを使う公開動作を検証するものにする。
2. 独立NPM利用側でVite、`muon run`、配布ランチャーを起動し、X11とWaylandでPlaywright MCPによる表示、描画、終了を確認する。実プロセスのメモリーマップでGTK・GDK・GLibの読み込み元を確認する。
3. ファイル選択、SVG画像読み込み、システム入力モジュールを検証する。旧版を準備済みの`.muon`へ更新し、古いGTKファイルが残らずシステムGTKで起動することも確認する。
4. 利用者向け文書の日本語原本を更新し、英語版へ反映する。ビルド基準、システム側に必要なライブラリー、同梱ファイルの説明を新しい構成に合わせてコミットする。

完了条件は、公開起動経路と既存環境からの更新でシステムGTKを使い、表示と必要なGTK機能が動作することである。

## 段階3：全配布物と全体テストを確認する

1. amd64・armhf・arm64の前提イメージと配布物をビルドする。全対象のELF依存解析が成功し、システムGTK・GLibを必要条件として記録することを確認する。
2. NPMパッケージ、アプリのtar.gz・deb・AppImageを生成し、展開した実物にGTK関連の同梱ファイル・モジュール・キャッシュ・設定データがないことを確認する。生成したdebの依存条件にもGTK・GLibが含まれることを確認する。
3. 3対象とも対応する環境のローダーでシステムGTKの解決先を確認する。amd64では実プロセスも確認する。ARMは対応CPUのコンテナーとQEMUを使い、実機や画面について確認できなかった範囲を区別して記録する。
4. 修正後の全体`npm test`を実行する。失敗時は原因を調査し、修正が必要なら再現テストから進める。全体成功後に検証記録を更新してコミットする。

完了条件は、全3対象と配布形式で同梱GTKの除去を確認し、システムGTKの参照と公開起動を上記の範囲で実証し、全体テストが成功することである。

## 完了時に確認する項目

- GTK・GDK・GLibと同梱専用の関連ファイルが、生成した配布物に残っていない。
- 配布版の依存検査と実行中のメモリーマップがシステムGTKを示す。
- 起動時にGTK専用パスを追加せず、利用者の入力設定を継承する。
- 旧配布版から更新しても、古いGTKが残って選ばれない。
- 3対象の依存情報、配布形式、利用者文書が新構成と一致する。
- 関連テストと最終の全体`npm test`が成功し、段階ごとの変更がコミットされている。

## 実施結果

### 段階1

削除確認用テストは旧amd64配布物で2件とも失敗した。同梱グループの69ファイルが検出され、`ldd`も配布ディレクトリー内のGTKを示した。起動環境を継承するテストも、変更前のViteで12件、AppImageで1件の失敗を確認してから修正した。

GTKのビルド・収集・配置と専用の起動環境設定を削除した。Debian 12の前提イメージは3対象ともビルド済み。関連するUIの195件、Viteの起動環境16件、AppImageの環境継承1件は成功した。UIテストは配布ビルドの途中で消去されたホスト用ツールを必要とするため、ツールを配置し直してから失敗した2ファイル84件を再実行した。

amd64の配布ビルドでは、GCC 12が既存の`ServerEnvelope`の`std::vector::insert`に`-Wstringop-overread`を報告した。診断対象は4バイトのヘッダーを持つ配列の再配置部分であり、JSONや本文の入力範囲に不正は見つからなかった。段階1のビルド基準変更に必要な対応として、必要容量を先に確保してからヘッダーとデータを追加する形へ変更した。通信形式と長さの扱いは維持し、警告を無効化する設定は追加していない。再ビルドと既存の公開サーバーテストで確認する。[GCCの警告オプション](https://gcc.gnu.org/onlinedocs/gcc/Warning-Options.html)と、ビルド用コンテナー内の`/usr/include/c++/12/bits/stl_vector.h`にある`reserve`のAPIコメントを確認した。

再ビルドでamd64のLinux部分とWindows 2対象は成功し、`ServerEnvelope`の警告は解消した。最後のAndroid処理はJDK環境変数の指定漏れで停止したため、既存のJDK・Android SDKを指定してその処理とSDK・配布物の配置を再実行し、成功した。

依存解析にはCEFの`libpthread.so.0`と`libdl.so.2`が未使用という警告が2件残る。ビルド環境で両ファイルの提供元が`libc6`であることを確認し、生成された`Depends`にも別パッケージの余分な要求はない。これは配布済みCEFバイナリーのリンク情報に対する診断なので、外部バイナリーを加工せず、依存レポートに診断を保持する。[dpkg-shlibdepsの診断説明](https://manpages.debian.org/bookworm/dpkg-dev/dpkg-shlibdeps.1.en.html#Warnings)

新しいamd64配布物で、削除確認2件とシステムGTKの機能確認1件、公開HTTPサーバー2件が成功した。`muon-core`と`libmuon-ui.so`のGTK・GDK・GLib・GIO・GObjectは、いずれもホストの`/lib/x86_64-linux-gnu/`へ解決された。ファイル選択、SVGの画素、システムibusモジュールの読み込みも確認した。builderのテスト全体も成功し、段階1の完了条件を満たした。

証跡は`/tmp/muon-plan16-evidence/`に保存している。主要なログは`gtk-removal-red.log`、`build-amd64-recheck.log`、`stage-amd64-recovery.log`、`amd64-runtime-tests.log`、`builder-tests.log`である。公開起動経路、全配布形式、全体テストは引き続き実施する。

### 段階2

段階1を`e93db19`でコミットした後、削除確認専用のテストと旧同梱データのテスト資材を取り除いた。関連する3ファイル178件の成功を確認し、`446c53a`でコミットした。依存情報の伝搬や利用者の環境継承、システムGTKの機能を確認するテストは維持している。

amd64を収録した検証用NPMパッケージ`startup-package/muon-ui-0.33.21.tgz`から、独立した利用側を作成した。X11とWeston上のWaylandの両方で、Vite、`muon run`、再起動後、配布ランチャーの各プロセスを検査し、GTK・GDK・GLib・GIO・GObjectがすべてホストの`/usr/lib/x86_64-linux-gnu/`から読み込まれることを確認した。Playwright MCPによる表示・緑の描画・終了の検証も成功した。

両環境とも、旧配布版の準備済みディレクトリーを配置してから更新し、古いGTKファイルが残らないことを確認した。Waylandではファイル選択、SVG読み込み、システムibusモジュールのテストも成功した。証跡は`packaged-x11.log`と`packaged-wayland.log`にある。利用者文書の日本語と英語も更新し、段階2の完了条件を満たした。全対象を収録する最終NPMパッケージと全配布形式は段階3で検証する。

### 段階3

`2850331`のソースで`build_package.sh --target all --jobs 2`が成功した。Linuxのamd64・armhf・arm64、Windowsのi686・amd64、Androidの資材を揃え、`muon-ui-0.33.23.tgz`と`muon-plugin-sdk-0.33.23.tgz`を生成した。3対象のLinux依存解析はすべて`audit.issues`が0件で、GTKとGLibのシステムパッケージを依存先として記録した。ビルドログと依存情報は`final-linux-build/`、パッケージとSHA-256は`final-package/`に保存している。

最終NPMパッケージを独立した利用側へインストールし、公開CLIで各対象のAppImage・deb・tar.gzを作成した。NPMを含む3対象×4形式の計12組で、GTK関連ファイルと旧同梱ファイルの残存がないことを確認した。各形式の依存情報はNPM内の情報と一致し、debにもGTK・GLibの依存条件が含まれる。SDKパッケージにもGTK関連の同梱ファイルはなかった。

対応するDebian 12コンテナーで、各形式の`muon-core`と`libmuon-ui.so`に`ldd`を実行した。GTK・GDK・GLib・GIO・GObjectはすべてシステムの`/lib/`配下へ解決され、未解決の共有ライブラリーはなかった。配布物が初回準備で取得するCEFは、ローダー検査に限り、ビルドに使った公式CEFの`libcef.so`を別ディレクトリーから参照させた。この参照先にはGTK関連ファイルを置いていない。ARMの結果はQEMU上のローダー検査であり、ARM実機での画面動作は確認していない。証跡は`distribution/summary.json`と`distribution/logs/`にある。

ARM AppImageの展開では、ホストのQEMU自動起動設定がAppImage固有のELF識別子に一致せず、実行形式エラーになった。アーキテクチャとELFヘッダーを確認し、QEMUを明示して同じAppImageを展開した。配布物やシステムの自動起動設定は変更していない。

armhfの`dpkg-shlibdeps`には、`__aeabi_atexit@CXXABI_ARM_1.3.3`を解決できないという追加診断が3件あった。コンテナー内の`libstdc++.so.6`はこのシンボルを公開し、対象ELFの`DT_NEEDED`と`ldd`もそのライブラリーを示している。一方、`libstdc++6:armhf.symbols`には該当シンボルが記載されていなかった。実ファイルとパッケージのシンボル情報の差による診断と判断し、実行時の依存先と生成された`libstdc++6 (>= 11)`の依存条件を確認した。さらに`muon-core`・`libmuon-ui.so`・`libcardio.so`の`ldd -r`で動的なシンボル解決を検査し、未解決シンボルがないことを確認した。警告は抑制せず、`armhf-symbol-diagnostics.log`へ根拠を保存した。[dpkg-shlibdepsの診断](https://manpages.debian.org/bookworm/dpkg-dev/dpkg-shlibdeps.1.en.html)、[deb-symbolsの形式](https://manpages.debian.org/bookworm/dpkg-dev/deb-symbols.5.en.html)

amd64では生成したAppImage・tar.gz・debを、Ubuntu 24.04.5のX11とWeston上のWaylandで起動した。AppImageはFUSEを使わずに展開して実行するオプション、tar.gzとdebは展開した実物のランチャーを使った。標準の`tested`設定で初回準備を行い、計6回すべてでPlaywright MCPによるページの読み込みと終了が成功した。CEFの起動引数も対応する画面基盤を示し、実プロセスのGTK・GDK・GLib・GIO・GObjectはすべて`/usr/lib/x86_64-linux-gnu/`から読み込まれていた。ホストのGTKは3.24.41、GLibは2.80.0である。証跡は`distribution/startup/summary.json`と各形式の`result.json`にある。

最初の検証アプリには`compat-latest`を指定していたため、複数のCEF候補の取得・比較に時間がかかった。今回のGTK検証に不要な指定だったため、その実行を停止して標準設定へ戻し、配布物の生成・12組の検査・6回の起動を実施し直した。製品コードの修正は行っていない。

全体テストのAndroid処理では、plan15と同じくbundletool 1.18.3内のProtobufから`sun.misc.Unsafe::arrayBaseOffset`の警告が出た。[JDK 24以降の実行時警告](https://docs.oracle.com/en/java/javase/24/migrate/significant-changes-jdk-24.html)に該当し、今回使用したJDK 25でも表示される。APK生成とAndroidのテストは成功している。既存の外部依存による警告として記録し、外部コードの変更や警告の抑制は行っていない。

最終の全体`npm test`は、2026年10月23日12:55 JSTに終了コード0で完了した。共通C/C++テストとbuilderのテストも成功し、件数が出力される主要な工程は次のとおりだった。配布物を指定するGTK関連の検証は、前述の専用実行でも確認している。

| 対象 | 成功 | スキップ |
| --- | ---: | ---: |
| SDK | 18 | 0 |
| Android tester | 110 | 0 |
| Android PoC | 3 | 0 |
| Node | 40 | 0 |
| UI | 507 | 4 |
| ネイティブCTest | 61 | 0 |
| Linux CEF | 241 | 32 |
| Windowsパス処理 | 62 | 0 |
| Windows builderパス処理 | 122 | 0 |
| Windows i686 CEF | 239 | 29 |
| Windows amd64 CEF | 239 | 29 |
| Windows通知（各ビット版） | 各8 | 0 |
| Windows通信（各ビット版） | 各8 | 0 |

Windows実機の資材は指定どおり`C:\test\`配下を使い、全体テスト後に実機への接続とテストプロセスが残っていないことを確認した。全体ログは`full-test-final.log`、件数の抜粋は`full-test-summary.txt`に保存している。

全体テスト後、UIとSDKを全配布対象の配置へ戻し、両パッケージの検証を通した。UIのネイティブツールと実行資材は62ファイルすべてが、検査済みNPMパッケージ内のファイルとSHA-256で一致した。完成した2つのNPMパッケージをリポジトリーの`artifacts/`へ配置し、検査したものと同じSHA-256であることも確認した。

- `muon-ui-0.33.23.tgz`: `e4074516181425319215aebe4e8d095b9a339bc6d1e97c4efe7c918c6320d4b3`
- `muon-plugin-sdk-0.33.23.tgz`: `892f6659af45c31797be1b0e6b616f855fc5b10d9b33a65a19a305414b5114ad`

以上により、同梱ファイルの除去、システムGTKの参照、入力設定の継承、旧版からの更新、全3対象の配布情報、全体テストの各完了条件を満たした。ARMの検証範囲は前述のQEMU上のローダー確認に限る。
