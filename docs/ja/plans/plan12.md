# Windowsランタイムのlong path対応

## 目的と調査結果

Windows版muonで、従来の`MAX_PATH`を超えるパスを扱うための変更と検証方法を整理する。本書でいう長いパスはディレクトリとファイル名を合わせたパス全体を指す。8.3形式より長いファイル名への対応とは区別する。

調査日は2026年10月19日。コードの基準コミットは`c9fda0402ec1f27ddec03e1e7f74e9bc17dd7bd2`である。対象は`windows-i686`と`windows-amd64`。参照CEFは`147.0.14+g76d2442+chromium-147.0.7727.138`、builderのlibarchiveは`3.8.7`である。今回実施したのは、muonと依存コードの読取り、公式資料との照合、MinGWのプリプロセッサによる定数確認である。Windows実機での長いパスの動作試験と実装はまだ行っていない。

`longPathAware`の宣言は必要だが、宣言だけではランタイム全体の対応は完了しない。builderには実際に260文字となる固定バッファ、ANSI版のファイル操作、狭い文字列で受け取る起動引数が残っている。コアにも、子プロセス起動時の実行ファイル名の渡し方や、パスを文字列へ変換する箇所の確認が必要である。

さらに、参照Chromiumには`MAX_PATH`を前提とする独自処理が残っている。アプリが扱うファイルのパス、配布物の配置元、CEFを実際に起動する場所、プロファイルの保存先を分けて対応する必要がある。muon側の変更だけで、これらすべてを約32,767文字まで使用できるとは判断しない。

本計画の基本案は、Windowsの`LongPathsEnabled=1`を前提として、通常のパス表記によるmuonのファイル操作と準備処理を対応させることである。CEF、シェル、自動起動、外部プロセスの制約は個別に検証し、対応できる配置と利用方法を明示する。

## Windowsの仕様と対応範囲

### マニフェストとOS設定

Windows 10 version 1607以降の対象Win32 APIでは、次の両方を満たすと、通常のパス表記に対する`MAX_PATH`制限が緩和される。

1. `HKLM\SYSTEM\CurrentControlSet\Control\FileSystem`の`LongPathsEnabled`が`REG_DWORD`の`1`である。
2. 呼出し元の実行ファイルに`longPathAware=true`を含むマニフェストがある。

追加する要素は次の形になる。既存の`assembly`要素の内側へ配置し、OS互換性やCommon Controlsの宣言も残す。

```xml
<application xmlns="urn:schemas-microsoft-com:asm.v3">
  <windowsSettings xmlns:ws2="http://schemas.microsoft.com/SMI/2016/WindowsSettings">
    <ws2:longPathAware>true</ws2:longPathAware>
  </windowsSettings>
</application>
```

OS設定はプロセス内でキャッシュされる。設定変更後の試験には新しいプロセスを使用し、必要な再起動もテスト環境側で済ませる。muonの起動やインストールをきっかけに、マシン全体の設定を変更する処理は追加しない。[Microsoftの長いパスの説明](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation)、[マニフェストの仕様](https://learn.microsoft.com/en-us/windows/win32/sbscs/application-manifests#longpathaware)

この宣言は各プロセスの実行ファイルに必要である。ランチャーの宣言が、別に起動するbuilderやNode.jsへ引き継がれるとは扱わない。DLLへ宣言を追加してホストの設定を代替する方式も採らない。

### 通常のパスと拡張長パス

`\\?\C:\...`や`\\?\UNC\server\share\...`は拡張長パスの表記である。対応するUnicode APIでは、通常のパスと異なる解析規則で扱われる。相対パスへそのまま接頭辞を付けることはできず、区切りの`/`や`.`、`..`の扱いにも注意が必要である。[Microsoftのパス解析の説明](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation)

| 条件 | 本計画での扱い |
| --- | --- |
| OS設定が有効で、通常表記の長いローカル絶対パス | muonが管理するファイル操作とbuilderの準備処理の基本的な対応対象とする。 |
| OS設定が有効で、通常の相対パスを解決した結果が長くなる | muonが管理するファイル操作では、元の基準ディレクトリを保って扱う。必要ならWin32へ渡す前に絶対化する。長い作業ディレクトリからのプロセス起動とは分けて検証する。 |
| 通常表記のUNCパス | ファイル操作の対象に含め、利用できる共有先で検証する。共有先の権限やファイルシステム固有の制約は別に扱う。 |
| 呼出し元が明示した拡張長パス | 対応するファイルAPIへ渡す際に接頭辞を破損させない。DLL、シェル、URIなどへ一律に流用しない。 |
| OS設定が無効で、通常表記の長いパス | 基本案の成功条件には含めない。既存の短いパスでの動作を保ち、長さに起因する失敗を切捨てや別ファイルへのアクセスに変えない。 |
| OS設定が無効でも、すべての入力を自動変換して長いパスを扱う | 別の対応範囲とする。要求される場合は、接頭辞の付加・除去と戻り値の仕様を計画へ追加してから実装する。 |

マニフェストとOS設定による対応を採る場合、全入力への`\\?\`の自動付与は必須ではない。接頭辞を自動付与していないこと自体を、必須修正の根拠にはしない。Cランタイム（CRT）などの特定の呼出しで必要と判明した場合に、対象となるファイルI/Oの入口で変換する。

### 長さの数え方と残る制約

Windowsへ渡す長さはUTF-16のコード単位で確認する。UTF-8のバイト数、JavaScript上の見た目の文字数、終端NULを含むバッファ長を混同しない。拡張長パスの最大長は概ね32,767文字であり、各パス要素の長さはファイルシステムの制約にも従う。各要素を長くする試験と、多段のディレクトリによってパス全体を長くする試験を分ける。[Microsoftの最大パス長の説明](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation)

`CreateProcessW`では、`lpApplicationName`を省略した場合、コマンドライン中の実行ファイル名部分に`MAX_PATH`制限がある。コマンドライン全体の上限は終端NULを含め32,767文字であり、ファイルパスの上限とは別である。[CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)

`SetCurrentDirectory`の公式資料には、`MAX_PATH`より長いカレントディレクトリを設定すると`CreateProcessW`が失敗するとの注意がある。長いパスにあるデータへアクセスできても、同じディレクトリをプロセスの作業ディレクトリにできるとは限らない。[SetCurrentDirectory](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setcurrentdirectory)

## 現在の実装

### 実行ファイルとマニフェスト

| 実行単位 | 現状 | 必要な作業 |
| --- | --- | --- |
| `muon-core.exe` | [muon_core.manifest](../../../core/cef/cmake/muon_core.manifest)にはWindows互換性の宣言があるが、`longPathAware`はない。[CMakeLists.txt](../../../core/cef/CMakeLists.txt)からRC経由で埋め込む。 | 宣言を追加する。CEFの子プロセスも同じ実行ファイルを使う構成を維持し、子プロセスの実動作を確認する。 |
| `muon-launcher.exe`とアプリ名へ変更したランチャー | [muon_launcher.manifest](../../../builder/src/muon_launcher.manifest)にはCommon Controlsの宣言だけがある。 | 宣言を追加し、コピー・改名・アイコン更新後も維持されることを確認する。 |
| `muon-builder.exe` | [build.sh](../../../builder/build.sh)はバージョン用RCを生成するが、manifest引数を空にしている。 | builder用マニフェストを埋め込み、生成RCとMakefileの依存関係へ追加する。 |
| `muon-plugin-inspector.exe` | [Makefile](../../../builder/Makefile)のリンク規則にマニフェスト用リソースがない。 | inspectorにも宣言を埋め込み、長いパスのプラグイン検査を確認する。 |
| NSISのインストーラーとアンインストーラー | [pack.ts](../../../ui/cef/pack.ts)の生成スクリプトに`ManifestLongPathAware`がない。 | NSISの宣言と生成物を確認する。ショートカットなどの別制約は後述のとおり扱う。 |
| 取得したNode.js、OSのcurl、利用者が起動する外部EXE | muonとは別のプロセスになる。 | 取得・起動する実物で確認する。バイナリを書き換えて宣言を追加しない。 |

[builder/build.sh](../../../builder/build.sh)では、Windows向けの`RUNTIME_HELPER_EXECUTABLE_NAME`は空である。Linux専用の`muon-runtime-helper`へWindowsマニフェストを追加する作業は不要である。

### 固定バッファ

[builder/src/common.c](../../../builder/src/common.c)の`muon_ensure_directory`と、[muon_launcher.c](../../../builder/src/muon_launcher.c)の`get_launcher_path`は`char buffer[PATH_MAX]`を使用する。手元のMinGWで、実際の各ソースを必要なincludeパス付きでプリプロセスすると、どちらも`PATH_MAX=260`となった。`#ifndef PATH_MAX`内の4096という値はこのビルドでは採用されない。

したがって、Unicode APIへの置換やマニフェスト追加だけでは、この2か所の制限は解消しない。パスの取得とディレクトリ作成には、入力やAPIの必要サイズに応じて確保する領域が必要である。

コアの[muon_paths.cpp](../../../core/cef/src/config/muon_paths.cpp)は`GetModuleFileNameW`と32,768要素のバッファを使用する。[muon_close_debug_log.cpp](../../../core/cef/src/log/muon_close_debug_log.cpp)は不足時にバッファを拡張する。`muon_builtin_fs.cpp`の`RealpathAsync`も`MAX_PATH`を初期値にして必要サイズへ拡張している。これらを、260文字で切り捨てる実装として一括修正しない。

### builderとランチャー

| 参照箇所 | 確認した処理 | 変更・検証する内容 |
| --- | --- | --- |
| [common.c](../../../builder/src/common.c)のWindows用ヘルパー | `_access`、`_chmod`、`_mkdir`、`_open`、`_rmdir`、`stat`、`_unlink`を狭い文字列で呼ぶ。 | ファイル名をUTF-16で渡す経路へ変更する。実際のCRTで残る長さ制限は、公開Win32 APIで処理する。 |
| 同ファイルの走査、コピー、削除、ハッシュ計算 | `opendir`、`readdir`、`fopen`などを使用する。 | ファイルの読書きだけでなく、列挙結果、属性、タイムスタンプ、再帰処理を含めて対応する。 |
| 同ファイルの再解析ポイント削除と更新 | `GetFileAttributesA`、`RemoveDirectoryA`、`DeleteFileA`、`MoveFileExA`を使用する。 | W版へ変更し、リンク先を誤って削除しないことと更新失敗時の復元を検証する。 |
| 同ファイルのパス加工 | `muon_normalize_path_separators`が`\`を`/`へ置換する。再帰mkdirはドライブのルートだけを特別扱いする。 | 通常UNCと拡張長パスのルートを識別する。接頭辞や共有名をmkdirの対象にせず、Win32へ渡す表記を整える。 |
| [prepare.c](../../../builder/src/prepare.c) | `common.c`を経由しない`stat`、`opendir`、`readdir`、`rename`がある。 | 入力のfingerprint計算、コピー、準備済みディレクトリの確定まで確認する。 |
| [prepare_cef.c](../../../builder/src/prepare_cef.c) | 外部curlで取得し、展開結果を`rename`で確定する。 | 取得先、キャッシュ、展開先、確定処理、失敗時の削除を対応させる。 |
| [prepare_node.c](../../../builder/src/prepare_node.c) | 展開先への`fopen`、アーカイブ読込み、runtimeの入替えを行う。 | 展開後の検査とバックアップからの復元を含めて対応する。 |
| [muon_launcher.c](../../../builder/src/muon_launcher.c) | `GetModuleFileNameA`、`getenv`、`_chdir`、`_spawnv`を使用する。 | 自身の場所、設定ファイル、状態保存先、引数転送、再起動をUTF-16とUTF-8の対応を保って扱う。 |
| [muon_prepare.c](../../../builder/src/muon_prepare.c)、ランチャー、inspector | Windowsでも`main(int, char**)`で引数を受け取る。 | Unicodeの起動引数を受けてUTF-8へ変換する入口を設ける。既に文字が失われたANSI引数を、後からUTF-8として扱わない。 |
| [muon_plugin_inspector.c](../../../builder/src/muon_plugin_inspector.c) | DLL読込みは`LoadLibraryW`だが、署名検査用の読込みは`fopen`である。 | DLL読込みの前段にあるファイル検査も対応させる。 |

`LOCALAPPDATA`、`USERPROFILE`、`MUON_CACHE_DIR`など、パスを含む環境変数の取得も対象とする。ASCIIのパスだけで長さを確認すると、ANSI版からUnicode版への移行漏れを検出できない。

### アーカイブとPEリソース更新

`common.c`と`prepare_node.c`は`archive_read_open_filename`へ狭い文字列を渡している。参照libarchiveの実装では、この経路は`open`を使用する。公開APIの`archive_read_open_filename_w`はWindowsで`_wopen`を使用し、特定の失敗時に拡張長パスを試す実装がある。Windowsではこの公開W版を使用する案とする。アーカイブの入力ファイルと、アーカイブ内のエントリー名は別に処理する。[libarchive 3.8.7のヘッダー](https://github.com/libarchive/libarchive/blob/v3.8.7/libarchive/archive.h)、[ファイル読込み実装](https://github.com/libarchive/libarchive/blob/v3.8.7/libarchive/archive_read_open_filename.c)、[公式API説明](https://github.com/libarchive/libarchive/blob/v3.8.7/libarchive/archive_read_open.3)

長いパスへの対応によって、既存のアーカイブ展開先の検査を弱めない。親ディレクトリへの移動、絶対パス、リンクなどの既存判定を維持する。出力側にはmuon自身のファイル操作が残るため、入力APIの変更だけで展開全体が対応したとは扱わない。

[muon_prepare_resource.c](../../../builder/src/muon_prepare_resource.c)は、engraverの既定I/Oを使ってJSON、アイコン、PEを読み書きする。参照engraverの既定I/Oは`open(const char*)`を使用する。一方、公式READMEとヘッダーには`eg_io_ops`および`*_with_io`の公開APIがある。muonのWindows用ファイル操作をこのAPIへ渡せば、外部コードを変更せずに対応できる。JSONの読込み、JSONから参照するアイコン等の読込み、PEの入力と出力のすべてへ同じI/Oを渡す。[参照engraverのREADME](https://github.com/kekyo/engraver/blob/2651fe58640f493731f4f97d26ee8996ca30ab5b/README.md)、[公開APIと所有権の説明](https://github.com/kekyo/engraver/blob/2651fe58640f493731f4f97d26ee8996ca30ab5b/libengraver/include/engraver.h)

### コアのファイル操作と設定

[muon_builtin_fs_helpers.cpp](../../../core/cef/src/plugins/builtin/muon_builtin_fs_helpers.cpp)の`CreateLocalFilesystemPath`は、ファイルURIを処理した後、WindowsではUTF-8をUTF-16へ変換する。[muon_builtin_fs.cpp](../../../core/cef/src/plugins/builtin/muon_builtin_fs.cpp)の通常の読書きは`CreateFileW`を使うため、マニフェスト追加で改善する見込みがある。

ただし、すべての操作が同じ実装ではない。`exists`、ディレクトリ列挙、作成、削除、改名、コピー、truncateなどは`std::filesystem`、追記は`std::ofstream`を使用する。`watch`のWindows側はディレクトリのスナップショットを作る方式であり、`std::filesystem`と属性取得を使用する。`CreateFileW`の成功だけを全APIの対応確認にしない。

`RealpathAsync`と`ReadWindowsSymbolicLinkTarget`は結果から`\\?\`等を除去する。この結果を次のファイル操作へ渡す往復も確認する。基本案ではOS設定が有効な通常表記を使用するが、設定が無効な環境まで対応を広げる場合は戻り値の扱いを再設計する必要がある。相対シンボリックリンクのリンク先を、無条件に絶対化してはならない。

コアの他のファイル入力も対象になる。

- [muon_config.cpp](../../../core/cef/src/config/muon_config.cpp)の`-c`、環境変数、JSON中の設定パスと相対パスの基準。
- [muon_app_storage.cpp](../../../core/cef/src/app/muon_app_storage.cpp)のディレクトリからのアセット配信、ZIP読込み、`canonical`による配信範囲の検査。
- [muon_log.cpp](../../../core/cef/src/log/muon_log.cpp)のログ作成とCEFログの転送。
- [muon_sha256.cpp](../../../core/common/src/muon_sha256.cpp)の署名・ハッシュ検査用の読込み。

設定やプラグイン読込みには、`std::filesystem::path`と狭い`std::string`を相互変換する箇所がある。長さだけでなく、非ASCII文字が途中で失われないことを確認する。調査環境のMinGWプリプロセッサは`__MSVCRT_VERSION__=0x700`を定義していた。実際にリンクするCRTとlibstdc++を記録し、MSVCの新しいCRTに関する説明だけで挙動を決めない。

### DLLと子プロセス

[muon_plugin_runtime.cpp](../../../core/common/src/plugins/muon_plugin_runtime.cpp)はプラグインのパスを`path.string()`でサービスへ渡し、[muon_plugin_runtime_desktop.cpp](../../../core/cef/src/plugins/muon_plugin_runtime_desktop.cpp)は再び`std::filesystem::path(locator)`へ変換して`LoadLibraryExW`を呼ぶ。ここはUTF-8として渡すのか、Windows固有の文字列表現なのかを揃える。既存の`LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR`等の検索条件は維持する。[LoadLibraryExWの仕様](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)

[muon_builtin_executor.cpp](../../../core/cef/src/plugins/builtin/muon_builtin_executor.cpp)は、コマンドと引数をUTF-16へ変換する一方、`MuonWindowsJobProcessLaunchOptions.application_name`を設定していない。このため、[muon_windows_job_process.cpp](../../../core/cef/src/platform/muon_windows_job_process.cpp)の`CreateProcessW`には`nullptr`が渡される。パスで指定された実行ファイルは、独立した`application_name`として渡す必要がある。

単なるコマンド名を指定した場合の検索、拡張子の補完、引数の引用、環境変数、標準入出力、終了コード、Job Objectによる子孫の管理も維持する。パス指定とコマンド名指定を区別せずに`application_name`へ移すと、従来の検索動作が変わる。

[node_plugin.cpp](../../../node/native/node_plugin.cpp)は既に`application_name`を明示している。ここをexecutorと同じ欠陥として修正しない。長い場所にあるNode.jsのプロジェクトやbridgeを渡した場合、取得したNode.js自身の挙動、子プロセスに継承する作業ディレクトリを確認する。

### CEFとChromium

参照Chromiumのソースで、次の制限を確認した。

| 外部コード | 確認した内容 | muonへの影響 |
| --- | --- | --- |
| [base_paths_win.cc](https://github.com/chromium/chromium/blob/147.0.7727.138/base/base_paths_win.cc)の`PathProviderWin` | 実行ファイルやモジュールの場所を`MAX_PATH`固定バッファで取得する。 | CEF本体を深い場所へ移すと、muonのパス取得を直しても別の取得経路が残る。 |
| [file_util_win.cc](https://github.com/chromium/chromium/blob/147.0.7727.138/base/files/file_util_win.cc) | `MakeAbsoluteFilePath`、カレントディレクトリ・一時ディレクトリ取得に固定長処理がある。コピー、移動、削除等の一部には長さによる拒否もある。 | プロファイル、キャッシュ、TEMP、Chromium自身のファイル操作は個別に検証する必要がある。 |
| [launch_win.cc](https://github.com/chromium/chromium/blob/147.0.7727.138/base/process/launch_win.cc) | 通常の起動経路で`CreateProcess`の実行ファイル引数を省略している。 | CEF子プロセスの起動を、muonのexecutorの修正だけで解決できるとは限らない。 |

これらは対応を妨げるコード上の根拠であり、今回すべての経路の失敗を実機で再現したという意味ではない。短い実行場所から長いデータパスを扱う試験と、実行場所自体を長くする試験を分ける。

CEFの`browser_subprocess_path`が空の場合、Windowsでは主プロセスと同じ実行ファイルを使用する。現在の[muon_cef_settings.cpp](../../../core/cef/src/config/muon_cef_settings.cpp)はこの設定を指定していない。また、`cache_path`と`root_cache_path`には同じ解決済みプロファイルパスを設定する。絶対パスや親子関係など、CEFの公開条件を維持する。[CEF設定の公式資料](https://cef-builds.spotifycdn.com/docs/147.0/structcef__settings__t.html)

CEF側の`NormalizePath`も確認したが、`MakeAbsoluteFilePath`を呼ぶ部分はPOSIX条件内だった。「Windowsのプロファイル設定が、この関数だけで必ず拒否される」とは判断しない。プロファイルについては、起動だけでなくCookieやlocalStorageの保存、再起動、キャッシュの更新と削除まで確認する。[参照CEFのcontext.cc](https://github.com/chromiumembedded/cef/blob/76d2442/libcef/browser/context.cc)

### シェル、ダイアログ、自動起動、NSIS

[muon_builtin_fs_dialogs.cpp](../../../core/cef/src/plugins/builtin/muon_builtin_fs_dialogs.cpp)は`IFileDialog`と`SHCreateItemFromParsingName`を使用する。初期フォルダーの解析に失敗しても、その場ではエラーを通知せずに表示を続ける経路がある。長い初期パス、選択結果、保存先と、その後の`muon.fs`操作を一続きで確認する。ファイルシステムAPIとシェルの対応範囲は同一ではない。[Microsoftのシェルに関する注意](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation)

[muon_autostart.cpp](../../../core/cef/src/config/muon_autostart.cpp)の`kWindowsRunCommandMaxLength=260`は、Runキーの仕様に対応した制限である。長いパス対応のためにこの定数を拡大してはならない。引用符などを含む登録コマンドが制限内なら登録し、超える場合の既存エラーを確認する。別の自動起動方式の追加は本計画に含めない。[RunとRunOnceの公式説明](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)

NSISには`ManifestLongPathAware true`があるが、公式資料は`CopyFiles`と`CreateShortcut`などが長いパスに対応しないと明記している。変数の既定サイズも1024文字である。現在の生成スクリプトはショートカット作成、起動、アンインストーラー生成、状態ディレクトリの再帰削除を含むため、宣言の追加だけで任意長のインストール先を保証できない。[NSISのマニフェスト設定](https://nsis.sourceforge.io/Docs/Chapter4.html#longpathaware)、[NSISの変数](https://nsis.sourceforge.io/Docs/Chapter4.html#variables)

## 実装方針

### パス表現と所有権

公開JavaScript APIではパスをUnicode文字列として扱い、ネイティブ側へ渡す文字列とJSONファイルの符号化にはUTF-8を使用する。Windowsのファイル操作へ渡すところでUTF-16へ変換する。既に`std::filesystem::path`で保持しているWindowsパスは、そのネイティブ表現を利用する。UTF-8とANSIコードページの文字列を同じ型のまま往復させない。

Cのbuilderでは動的に確保した変換結果と列挙結果の解放元を明示する。C++側は既存の変換処理を整理して利用する。CとC++のために新しい大規模な抽象化層を設けず、パス変換とOS操作の責務を分ける。

起動引数はWindows用のUnicode入口で取得する。`wmain`を採る場合は、MinGWのUnicodeエントリーポイント設定とGUIランチャーのリンク条件を合わせて変更する。環境変数もwide版で取得してからUTF-8へ変換する。OS全体のコードページ変更を前提にしない。[wmainとUnicode環境変数の説明](https://learn.microsoft.com/en-us/cpp/c-language/using-wmain?view=msvc-170)

パス取得のバッファは、APIが返す必要サイズと終端NULの規則に従って確保する。`PATH_MAX`を別の大きい定数へ置き換えるだけでは完了としない。絶対化が必要な場合は元の基準ディレクトリを保ち、他スレッドのためにプロセス全体の作業ディレクトリを変更しない。[GetFullPathNameWのサイズと相対パスの注意](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfullpathnamew)

拡張長パスの変換を導入する箇所では、ローカル絶対パス、UNC、既に接頭辞があるパスを区別する。デバイス名や名前付きパイプをファイルパスとして加工しない。シンボリックリンクの相対ターゲット、ファイルURI、公開APIの戻り値も、用途に合わせて扱う。

### ファイル操作の置換

Windows用の`open`、属性取得、列挙、作成、削除、移動をmuon側の共通処理へ集約し、`prepare.c`等の直接呼出しも通す。W版CRTで長いパスを扱えることを実際のビルドで確認する。失敗する操作は`CreateFileW`、`GetFileAttributesExW`、`FindFirstFileW`などの公開APIで扱う。CRTやlibstdc++の全面的な入替えは前提にしない。

`std::filesystem`を使うコア側は、操作ごとの試験で不足を特定する。正常な実装まで一括でWin32へ書き換えず、失敗を再現した操作だけを修正する。改名・更新の原子性、上書きの条件、リンクをたどるかどうか、ファイル時刻なども元の用途に合わせて保つ。

Win32 API失敗時は、直後の`GetLastError`を保存して報告する。例えば現在の`muon_atomic_replace`はWin32 API失敗後に`errno`を表示するため、この置換に合わせて診断も修正する。文字列の切捨てや、不足したパスで処理を続行することは許容しない。

### 起動場所とCEFの制約

基本的な利用形態は、CEF本体と子プロセスを起動できる場所にruntimeを置き、長い場所にあるデータ、設定、アセット、プラグイン等へアクセスする構成とする。配布元を長い場所に置くことと、CEFの実行場所を長くすることは別の試験項目にする。

既存の`user-state`方式は配布元とruntimeの配置を分けられる。ただし、`LOCALAPPDATA`自体が長い場合もあるため、この方式だけで短い実行場所が保証されるとは扱わない。`in-place`方式は実行場所の制約を受ける。[現在のインストール先の説明](../package-install-location-advanced-topics.md)

CEFが使うruntime、プロファイル、TEMPに制約が残る場合は、実機結果に基づいて対応範囲を記載する。明示されたプロファイルを別の場所へ黙って移したり、保存失敗をメモリー内の動作へ置き換えたりしない。短い場所への配置が必要な場合は、その条件と設定方法を利用者へ示す。

CEFの実行場所やプロファイル自体を無制限に長くすることが必須になった場合は、外部コードを変更しない条件で成立するかを先に確認する。新しい配置方式、パスの別名、CEFの変更などを暗黙に追加しない。既存の公開設定で解決しなければ、その対応を追加計画として確定する。基本案のファイル操作への対応と、この追加範囲の未達は分けて記録する。

### 配布と外部ツール

PEのリソース更新後、配布用コピー後、署名後の実行ファイルで、マニフェストと動作が維持されることを確認する。リソース更新は署名前に完了させ、署名済みの成果物を後から編集しない。

curlは別プロセスなので、muonのマニフェストだけで長い取得先へ書けるとは限らない。実際のWindows用curlで確認する。失敗する場合は、短い一時領域へ取得した後、muonのファイルI/Oで最終キャッシュと同じディレクトリの一時ファイルへコピーし、検証後に確定する方式を検討する。異なるボリュームからの移動を原子的な更新とみなさない。

NSISでは、通常のインストール、ショートカットからの起動、終了後のアンインストールまで成功する配置を確認する。未対応の長さはインストール前に検出し、途中まで展開した状態を成功として返さない。NSIS自体の改造、外部バイナリのマニフェスト書換え、対応していない配布形式の追加は行わない。

## 検証方法

### テスト環境

実機のWindowsで`LongPathsEnabled=1`と`0`の環境を分けて用意する。共有PCの設定をテスト中に切り替える方式は採らない。管理者による準備が必要なら、設定済みの環境の用意を依頼する。OSのバージョン、ポリシー値、CPU対象、CRT、CEF、Node.js、NSIS、curlのバージョンと成果物を記録する。

両CPU対象を実行して確認する。WineはbuilderやDLLの早期確認に利用できるが、WindowsのマニフェストとOS設定の組合せを検証した代わりにはしない。Wine固有の不具合と特定できた場合は、Wine向けの変更を追加せず実機へ切り替える。

実機試験は既存の`agent-rover`接続と[plugin-e2e-windows.test.ts](../../../core/cef-test/plugin-e2e-windows.test.ts)を利用する。ビジーなら接続できるまで待ち、実機が必要な試験が終わったら解放する。現在の接続処理は接続失敗時にskipを返すため、テストコマンドの終了コードだけで実機確認済みと判断しない。接続と対象テストの実行件数まで確認する。

試験用ファイルの準備や後片付けが先に長さ制限へ当たらないようにする。短い制御ディレクトリから、Unicodeと拡張長パスを扱えるテストドライバーで対象を作成する。agent-roverのファイル転送、PowerShell、`cmd.exe`の成功を、muonの成功条件に混ぜない。ブラウザーを介した操作と観測にはPlaywright MCPを使う。

### パスの組合せ

| 観点 | ケース |
| --- | --- |
| パス全体の長さ | 終端NULを除くUTF-16長が259、260、261となる境界と、300程度、1024超、4096超の多段パス。各要素の長さはファイルシステムの上限内にする。 |
| 文字 | ASCII、空白、日本語、使用中のANSIコードページでは表せない文字、サロゲートペアを含む名前。UTF-8では260バイト超でもUTF-16では短いケースを含める。 |
| 表記 | ドライブ絶対パス、短い基準ディレクトリからの相対パス、通常UNC、明示的な拡張長ローカル・UNC、ファイルURI。 |
| 正規化 | `.`と`..`、末尾の区切り、既存の接頭辞、相対シンボリックリンク。通常表記と拡張長表記で意味が違う名前を誤変換しない。 |
| 失敗 | 存在しないパス、権限不足、名前の要素が長すぎる場合、OSの限界を超える長さ、不正UTF-8。切捨て、誤ったファイルへのアクセス、後片付け漏れがないこと。 |

長さは最終的な絶対パスから計算する。テストの作業ディレクトリ名が変わっても、狙った長さになっていることを実行時に確認する。相対パスは入力自体の長さと、解決後の長さを分けて記録する。

### 機能ごとの確認

| 機能 | 観測する結果 |
| --- | --- |
| `muon.fs` | `readFile`と`writeFile`の全体・範囲指定、`readTextFile`と`writeTextFile`、`appendFile`と`appendTextFile`、`stat`と`lstat`、`exists`と`access`、`mkdir`と`readdir`、`copyFile`と`rename`、`truncate`、`unlink`と`rmdir`と`rm`、`realpath`と`readlink`と`symlink`、`watch`の内容とイベント。[公開型定義](../../../ui/muon.d.ts)の操作とオプションを基にケースを追加する。 |
| APIの往復 | `realpath`やダイアログが返したパスを、次の`readFile`、`writeFile`、`stat`へそのまま渡して同じ対象を扱えること。 |
| 設定とアセット | 長い`-c`とJSON内のパス、ディレクトリ・ZIP両方のアセット、`app://`表示、配信範囲外を拒否する既存動作。 |
| builder | 深い入力元、アーカイブ、キャッシュ、展開先、準備状態JSON、fingerprint、再利用、強制更新、失敗後の再試行、旧runtimeの復元、削除。 |
| ランチャー | 配置元と実行先を分けた起動、引数、環境変数、再起動、更新。CEFを深い実行先へ置く試験は別枠で結果を記録する。 |
| プラグイン | 深い場所のDLLと隣接する依存DLL、署名一致・不一致、inspector、executorによるDLL利用。関数を実際に呼んで結果を確認する。 |
| 子プロセスとNode.js | 長い実行ファイルパス、コマンド名検索、長い引数、短いcwd、長いcwdの制約、Node.jsプロジェクト読込み、標準入出力、終了コード、キャンセル、子孫の終了。 |
| CEF | browser・renderer・GPU等の起動、アセット、ログ、プロファイルの永続化、再起動。runtime、profile、TEMPを一つずつ長くして原因を切り分ける。 |
| 配布とシェル | リソース更新後の起動、NSISのインストール・起動・アンインストール、ショートカット、ファイル選択・保存、Runキーの長さ上限。 |

watchの試験は監視の準備完了後に操作を実行し、期待したイベントを待つ。固定時間sleepで成否を決めない。ダイアログでは初期フォルダーと返却値を観測する。時系列のUI不具合が見つかった場合に、動画と状態遷移のアサーションを追加する。

ソースに`longPathAware`と書かれていることだけを確認するテストは採らない。生成PEのマニフェスト確認は補助検査とし、実際に通常表記の長いパスを扱うテストを合否の基準にする。テスト専用EXEだけが対応していて製品EXEが未対応、という状態も検出する。

## 実装段階と各段階の完了条件

### 段階1 マニフェストと基本的なファイル操作

短いruntime・profile・TEMPを使い、通常表記の長いパスで`muon.fs`の書込みと読込みが失敗することをWindows実機で確認する。フィクスチャは別の対応済みドライバーで作成し、API入力が既に`\\?\`付きになっていないことも確認する。

コア、ランチャー、builder、inspectorのマニフェストとリソースの依存関係を整え、同じ試験を成功させる。既存のOS互換性、Common Controls、バージョンリソースを維持する。CEFの実行場所、profile、TEMPを長くした小さな試験も行い、以降の配置条件を記録する。

完了条件は、両CPU対象の製品コアで通常表記の長いパスのファイル往復が成功し、4種類の製品EXEへ宣言が埋め込まれ、短いパスの既存試験も成功すること。CEF等の制約は、再現条件と実際に失敗した操作を記録する。この段階を全機能の対応完了とはしない。

コミットの目安は`feat: enable long path awareness for Windows executables`。

### 段階2 builderのパスと基本I/O

`muon_ensure_directory`、実行ファイルパス取得、コピー・列挙・削除・属性取得について、260文字超と非ASCII文字で失敗する振る舞いのテストを追加する。失敗を確認してから、動的バッファとUnicodeの入口、Windows用I/Oを実装する。必要なテストドライバーはMinGWとMakefileで実行可能にする。

UNCのルートと拡張長パスの加工、環境変数、引数、Windowsエラーの報告もこの段階で揃える。`prepare.c`などに残る直接の狭いファイル操作を置換する。Linux側は既存実装を利用し、使われなくなったWindows専用ヘルパーを残さない。

完了条件は、両CPU対象で長い階層の作成、内容を保ったコピー、列挙、改名、削除が成功し、UTF-8とUTF-16の変換による文字欠落がないこと。4096を超える入力でもmuon独自の固定バッファに妨げられず、割当て失敗やOS側の拒否を正しく返すこと。builderの既存テストも成功させる。

コミットの目安は`fix: handle Windows builder paths with Unicode APIs`。入口、列挙、更新処理が大きくなる場合は、テストが成功する単位で分割する。

### 段階3 取得と展開とリソース更新

CEFのtar.bz2とNode.jsのZIPについて、アーカイブ本体と展開先の両方を長くした失敗を再現する。libarchiveの公開W版、出力I/O、直接のrename、fingerprint、キャッシュの確定処理を対応させる。curlはローカルのテストHTTPサーバー等で外部ネットワークに依存せず確認する。

PEリソース更新にも長い入力・出力・JSON・アイコンを使い、engraverの公開I/O差し替えAPIへ接続する。読み書きに失敗したときの解放、部分出力の削除、既存PEリソースの維持を確認する。

完了条件は、長いパスで取得・展開・ハッシュ検査・再利用・更新・復元・後片付けが成功すること。更新したPEが起動し、マニフェストと要求したリソースが維持されること。外部コードを変更せず、壊れたアーカイブ等を成功扱いにしないこと。

コミットの目安は`fix: support long Windows paths during runtime preparation`と`fix: use Unicode I/O for Windows resource updates`。

### 段階4 コアのファイルAPIとプラグイン

既存の[runtime-api.ts](../../../core/cef-test/plugin-e2e/runtime-api.ts)にあるファイルAPIの振る舞いを長いパスでも検証し、失敗した操作を先にテストで固定する。read/writeだけでなく、`std::filesystem`、追記、watch、realpathの戻り値、相対リンクを含める。

設定、アセット、ログ、署名確認、DLL読込みの各入口へ同じ種類のパスを渡す。UTF-8の文字列変換が不足する箇所と、使用中のCRTで長さ制限が残る箇所を分けて修正する。アセットの配信範囲やDLL探索条件を維持する。

完了条件は、機能ごとの確認表にあるコアの対象操作が両CPU対象で成功し、返されたパスを再利用できること。署名不一致や配信範囲外の拒否など、既存の失敗条件も維持する。CEF内部の操作に関する制約は、muonのファイルAPIの不具合と混同しない。

コミットの目安は`fix: support long Windows paths across filesystem APIs`と、設定・プラグイン等の振る舞いごとの`fix:`。

### 段階5 子プロセスとランチャー

短いcwdから長いパスのテストEXEを起動し、executorの失敗を確認する。パス指定の実行ファイルを`application_name`へ明示し、コマンド名検索の既存ケースも成功させる。引数の引用とUnicode環境変数、標準入出力、Job Objectの管理を含めて確認する。

ランチャーの起動処理もUnicode化し、準備済みruntimeへの起動と再起動を実行する。Node.jsは既存の明示的な実行ファイル指定を維持し、長いプロジェクトやbridgeを扱えるか確認する。長いcwdは独立した試験とし、OSとCEFの制約を超えるために別のcwdへ黙って変更しない。

完了条件は、muonが制御する子プロセス起動で長い実行ファイルパスとUnicode引数を扱え、コマンド名検索、終了コード、キャンセル、子孫の終了が従来どおり機能すること。配布元と実行先を分けたランチャー起動と再起動も成功すること。外部EXE自身の未対応は具体的な対象と条件を記録する。

コミットの目安は`fix: launch Windows executables with explicit Unicode paths`。

### 段階6 配布とWindowsのUI連携

NSISのマニフェスト設定を追加し、生成したインストーラーとアンインストーラーを実行する。インストール先、ショートカット、起動時のruntime準備、終了後の削除を確認する。未対応の配置では、途中まで成功して見える状態を残さない。

ファイルダイアログは長い初期フォルダー、選択、保存、返却パスの再利用を確認する。対応できない条件がOS・シェル側にある場合は、根拠と実機結果を記録する。Runキーの上限は維持し、上限超過時のエラーと通常の自動起動を検証する。

完了条件は、サポート対象として記載する配置で、配布・起動・利用・アンインストールの一連の操作が成功すること。CEF、NSIS、シェル、自動起動の残る制限が、利用者から見て判別できること。宣言を追加しただけの配布形式を対応済みと記載しない。

コミットの目安は`fix: preserve Windows long path support in packaged applications`。

### 段階7 利用文書と全体検証

日本語の[制限事項](../limitation.md)、[組み込みAPI](../muon-built-in-plugin-reference.md)、[インストール先](../package-install-location-advanced-topics.md)など、利用者が条件を確認する箇所へ反映する。OS設定、通常表記で使える機能、CEF等に残る制約を分け、設定変更後の再起動条件も説明する。日本語を原本として既存の他言語文書へ反映する。一般文書から本計画書へのリンクは追加しない。

次の既存コマンドを基に全体を確認する。新設する個別テストはそれぞれの段階で適切な既存テスト入口へ組み込む。

```sh
npm run test --workspace muon-builder
npm run test:windows-e2e --workspace muon-core-tester
npm run test:windows-nsis-e2e --workspace muon-ui
npm run verify:package-runtime --workspace muon-ui
npm test
```

NSISの実機テストはルートの`npm test`だけに任せず明示的に実行する。パッケージ検証は対象別の配布物を生成・配置してから実行する。全体テストの所要時間に応じて十分なタイムアウトを設定し、実行中の進行を確認する。skipや環境不足を成功件数に含めない。

完了条件は、全体テストと両CPU対象のWindows実機試験、NSISの実機試験、配布物検査が成功し、実装結果と利用文書が一致すること。新たな警告は原因を確認して解消し、未検証項目を残したまま全体の対応完了とはしない。

コミットの目安は利用文書の`doc:`と、検証で必要になった修正の`fix:`。

## 作業方法と完了判定

実装は上記の段階に従い、それぞれで失敗を再現する振る舞いのテストを追加してREDを確認し、修正後にGREENを確認する。複数の対処を試して原因が特定できた場合は、不要になった変更を戻す。外部依存の制限が計画の前提と合わなければ、変更範囲と完了条件を更新してから進める。

マニフェスト、Unicode化、ファイル操作の置換は既存機能の削除を目的としない。機能削除が必要になった場合は、削除を確認するテストのRED、削除後のGREENとコミット、最後にそのテストを削除するプロジェクトの手順に従う。既存コメントは維持し、内容が変わる箇所だけ更新する。

外部APIは公式資料と使用するバージョンのヘッダーコメントを照合する。libarchive、engraver、CEF、Chromium、MinGW等の外部コードは変更しない。公開されていないWindows内部構造を書き換えてlong path aware状態を作る方法も採らない。

実装全体の完了判定は次のとおりとする。

1. 4種類のmuon製品EXEで`longPathAware`が有効であり、配布処理後も維持される。
2. OS設定が有効なWindowsで、muonの対象ファイルAPIとbuilderの準備処理が通常表記の長いパスを扱える。
3. Unicodeの引数、環境変数、設定、パス、列挙結果に文字欠落がなく、固定バッファで切り捨てない。
4. キャッシュ更新、アーカイブ検査、署名確認、DLL探索、アセットの配信範囲、子プロセスの管理を維持する。
5. CEFの実行場所・profile・TEMP、外部EXE、NSIS、シェル、Runキーの対応範囲が、根拠と実機結果に対応している。
6. OS設定が無効な環境でも短いパスの既存動作を保ち、未対応の長いパスで誤った対象を操作しない。
7. `windows-i686`と`windows-amd64`の実機試験、NSIS試験、配布物検査、全体テストが完了している。
8. 利用文書に、通常のファイル操作への対応と、CEFやシェル等の配置制約が区別して記載されている。

今回の計画作成の完了条件は、現在のコードを根拠とする変更箇所、外部仕様への参照、対応範囲、各段階の成果物と検証方法、完了条件を`plan12.md`へ保存することである。実装段階の完了とは区別する。文書だけの変更でビルド工程へ影響しないため、今回の保存ではビルドと全体テストを実行しない。
