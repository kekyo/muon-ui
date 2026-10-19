# Windowsランタイムのlong path対応

## 目的と調査結果

Windows版muonで、従来の`MAX_PATH`を超えるパスを扱うための変更と検証方法を整理する。本書でいう長いパスはディレクトリとファイル名を合わせたパス全体を指す。8.3形式より長いファイル名への対応とは区別する。

初回調査日は2026年10月19日。コードの基準コミットは`c9fda0402ec1f27ddec03e1e7f74e9bc17dd7bd2`である。対象は`windows-i686`と`windows-amd64`。参照CEFは`147.0.14+g76d2442+chromium-147.0.7727.138`、builderのlibarchiveは`3.8.7`である。同日の追加調査では、`e1cd2ca7da472db4219583bbd3a91eab558293eb`のmuonと.NETの公式資料・ソース・テストを照合した。

`longPathAware`の宣言は補助的に追加する。長いパスを扱うための主な変更は、muon内部での正規化と、ファイルの入出力（I/O）へ渡す直前の拡張長パスへの変換である。builderには実際に260文字となる固定バッファ、ANSI版のファイル操作、狭い文字列で受け取る起動引数が残っている。コアにも子プロセス起動時の実行ファイル名の渡し方や、パスを文字列へ変換する箇所の確認が必要である。

さらに、参照Chromiumには`MAX_PATH`を前提とする独自処理が残っている。アプリが扱うファイルのパス、配布物の配置元、CEFを実際に起動する場所、プロファイルの保存先を分けて対応する必要がある。muon側の変更だけで、これらすべてを約32,767文字まで使用できるとは判断しない。

本計画では.NETの手法を取り入れ、`LongPathsEnabled=0`でも通常のパス表記でmuonのファイル操作と準備処理を利用できるようにする。長いパス対応のために、利用者へ管理者操作、レジストリ変更、再起動を要求しない。この方針は初版の`LongPathsEnabled=1`を前提とする基本案を置き換える。CEF、シェル、自動起動、外部プロセスの制約は個別に検証し、対応できる配置と利用方法を明示する。

実装着手時の予備試験では、既存のWindows接続先は`LongPathsEnabled=0`だった。独立したドライバーによる4096文字超の拡張長パスの作成・削除は両CPU対象で成功した。一方、amd64の製品コアへ通常表記の長いパスを渡す試験は`CreateFileW`で失敗した。作業ツリーの試作テストにはOS設定が無効ならskipする条件が残っているため、段階1で新しい成功条件に合わせる。この予備試験を製品の対応完了とは扱わない。今回の追加は分析と計画の更新であり、.NET自体の実行比較や製品実装は行っていない。

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

OS設定はプロセス内でキャッシュされる。これは通常表記をWin32 APIへ直接渡す方式の条件であり、対応APIへ拡張長パスを渡す方式の前提ではない。muonはOS設定を変更せず、内部のパス変換も設定値によって切り替えない。`longPathAware`の宣言自体は権限昇格を要求しない。[Microsoftの長いパスの説明](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation)、[マニフェストの仕様](https://learn.microsoft.com/en-us/windows/win32/sbscs/application-manifests#longpathaware)

この宣言は各プロセスの実行ファイルに必要である。ランチャーの宣言が、別に起動するbuilderやNode.jsへ引き継がれるとは扱わない。DLLへ宣言を追加してホストの設定を代替する方式も採らない。

### 通常のパスと拡張長パス

UNCは`\\server\share\...`で表すネットワーク共有へのパスである。`\\?\C:\...`や`\\?\UNC\server\share\...`は拡張長パスの表記である。対応するUnicode APIでは通常のパスと異なる解析規則で扱われる。相対パスへそのまま接頭辞を付けることはできず、区切りの`/`や`.`、`..`の扱いにも注意が必要である。[Microsoftのパス解析の説明](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation)

| 条件 | 本計画での扱い |
| --- | --- |
| OS設定が無効または有効で、通常表記の長いローカル絶対パス | muonが管理するファイル操作とbuilderの準備処理の基本的な対応対象とする。対応APIの入口で拡張長パスへ変換する。 |
| 通常の相対パスを解決した結果が長くなる | 元の基準ディレクトリで一度だけ絶対化し、その結果をI/Oへ渡す。長い作業ディレクトリからのプロセス起動とは分けて検証する。 |
| 通常表記のUNCパス | ファイル操作の対象に含める。今回の試験ではネットワーク環境の準備が必要なI/Oを除外し、文字列変換を検証する。共有先の権限やファイルシステム固有の制約は別に扱う。 |
| 呼出し元が明示した拡張長パス | 対応するファイルAPIへ渡す際に接頭辞を破損させない。DLL、シェル、URIなどへ一律に流用しない。 |
| シンボリックリンクに保存する相対ターゲット | 相対のまま保つ。リンクの作成場所・読取り対象のパスと、リンク内部へ保存する文字列を区別する。 |
| CEF設定、シェル、子プロセスの引数、URI | ファイルI/O用の拡張長パスへ一律に変換しない。受渡し先ごとの公開仕様と実機結果で判断する。 |

変換対象はファイルI/Oへ渡すパスである。設定、JSON、アーカイブ内のエントリー名、公開APIの戻り値まで拡張長表記に統一しない。Cランタイム（CRT）や`std::filesystem`にも操作に応じて変換したネイティブパスを渡す。接頭辞を渡しても動かない呼出しは、失敗を再現して公開Win32 APIへ置換する。

### 長さの数え方と残る制約

Windowsへ渡す長さはUTF-16のコード単位で確認する。UTF-8のバイト数、JavaScript上の見た目の文字数、終端NULを含むバッファ長を混同しない。拡張長パスの最大長は概ね32,767文字であり、各パス要素の長さはファイルシステムの制約にも従う。各要素を長くする試験と、多段のディレクトリによってパス全体を長くする試験を分ける。[Microsoftの最大パス長の説明](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation)

`CreateProcessW`では、`lpApplicationName`を省略した場合、コマンドライン中の実行ファイル名部分に`MAX_PATH`制限がある。コマンドライン全体の上限は終端NULを含め32,767文字であり、ファイルパスの上限とは別である。[CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)

`SetCurrentDirectory`の公式資料には、`MAX_PATH`より長いカレントディレクトリを設定すると`CreateProcessW`が失敗するとの注意がある。長いパスにあるデータへアクセスできても、同じディレクトリをプロセスの作業ディレクトリにできるとは限らない。[SetCurrentDirectory](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setcurrentdirectory)

## .NETの手法の分析

### 参照範囲とOS設定への依存

参照実装は`dotnet/runtime`の`v10.0.0`に固定する。対象は現代の.NETの`System.IO`であり、.NET Frameworkの互換性スイッチをmuonへ移植する計画ではない。Microsoftの説明でも、.NET Coreと.NET 5以降は長いパスを暗黙に扱い、従来の`MAX_PATH`検査を行わないとされている。[.NETのパス形式と正規化](https://learn.microsoft.com/en-us/dotnet/standard/io/file-path-formats#skip-normalization)

この性質はテスト側でも区別されている。`PathFeatures.AreAllLongPathsAvailable`は`System.Private.CoreLib`上ではOSの設定値を調べずに`true`を返す。従来の.NET Framework用の分岐だけが互換性スイッチとOS側の状態を確認する。muonでは前者を参考にし、設定値に依存しないファイルI/Oを実装する。テスト補助コードにある非公開の`RtlAreLongPathsEnabled`呼出しは採用しない。[PathFeaturesの実装とコメント](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/Common/tests/System/IO/PathFeatures.cs)

### 通常のパスの正規化

.NETの処理はパスの意味を決める正規化と、Windows APIに渡す表記の変換に分かれている。`Path.GetFullPath`は空の入力と埋込みNULを検査する。通常表記では`PathHelper.Normalize`が`GetFullPathNameW`を呼び、絶対パス化、区切り、`.`や`..`などを処理する。既に厳密な拡張長形式の入力なら、`GetFullPathInternal`はその表記を保つ。[Path.Windows.cs](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Private.CoreLib/src/System/IO/Path.Windows.cs)

`PathHelper`は最初に260要素の領域を使うが、APIが返す必要量に合わせて拡張する。したがって260は最大長ではない。正規化結果に`~`があれば、8.3形式の短い名前を展開する別処理も試す。この処理までmuonへ持ち込む必要はなく、長いパス対応と名前の展開を分ける。[PathHelper.Windows.cs](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Private.CoreLib/src/System/IO/PathHelper.Windows.cs)

正規化はファイルの存在確認やリンクの解決とは異なる。`GetFullPathNameW`は結果が実在することを検査しないため、新規作成先にも使える。一方、相対パスはプロセスのカレントディレクトリに依存する。同じ入力を非同期処理の途中で繰り返し解決せず、一度決めた絶対パスを使う。[GetFullPathNameWの仕様](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfullpathnamew)

.NETには基準ディレクトリを指定する`GetFullPath(path, basePath)`もある。同じドライブの`C:foo`と別ドライブの指定を分け、後者ではドライブルートから解決する分岐がある。muonはこのオーバーロードをそのまま再実装せず、`muon.fs`、設定ファイル、プラグインなどの既存の基準を保つ。`C:foo`を`C:\foo`と同じ入力にはしない。[基準ディレクトリ付きの実装](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Private.CoreLib/src/System/IO/Path.Windows.cs#L61)

### 拡張長表記への変換条件

`PathInternal.EnsureExtendedPrefixIfNeeded`と`EnsureExtendedPrefix`は次の条件で動く。ここでいう長さはC#の文字列長、すなわち終端NULを含まないUTF-16コード単位数である。[PathInternal.Windows.csの実装とコメント](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/Common/src/System/IO/PathInternal.Windows.cs)

| 入力の状態 | .NETの処理 |
| --- | --- |
| 正規化済みの通常パスが260以上 | 条件付き変換から接頭辞付加へ進む。 |
| 正規化後にも末尾のピリオドまたは空白が残る | 長さにかかわらず接頭辞付加へ進み、名前の文字を保持する。列挙等で得た名前も想定されている。 |
| ローカル絶対パス`C:\dir\file` | `\\?\C:\dir\file`へ変換する。 |
| UNC絶対パス`\\server\share\file` | `\\?\UNC\server\share\file`へ変換する。元の文字列の先頭へ単純に4文字を追加する処理とは異なる。 |
| 相対パス、ドライブ相対パス等の未解決パス | 接頭辞付加関数自身は絶対化せず、そのまま返す。通常のファイル操作では上位の正規化が先に行われる。 |
| 既にデバイス形式の接頭辞がある | 二重に接頭辞を付けない。既存の解析規則を変えない。 |

条件付き変換は正規化の代わりにはならない。利用者が通常表記で渡した末尾空白を、そのまま接頭辞付きの別名ファイルとして作成しない。通常入力を先に正規化する処理と、既に正確な名前が分かっているパスを保持する処理の区別が必要である。.NETのテストにも通常表記の末尾空白の除去と、明示的な拡張長表記の空白保持が別々にある。[Pathのテスト](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.Runtime.Extensions.Tests/System/IO/PathTests_Windows.cs)、[ディレクトリ作成のテスト](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/Directory/CreateDirectory.cs)

### 操作ごとの例外

接頭辞の付加は`CreateFileW`だけに限定されない。.NETは操作ごとのラッパーに処理を置き、Windows APIを直接呼ぶP/Invoke宣言には長いパスを暗黙に処理しない旨のコメントを付けている。

| 操作 | 確認した処理 | muonへ取り込む点 |
| --- | --- | --- |
| ファイルを開く | [CreateFile](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/Common/src/Interop/Windows/Kernel32/Interop.CreateFile.cs)で条件付き変換する。 | 読書き、属性取得用ハンドル、署名確認にも同じ規則を通す。 |
| 属性、削除、列挙 | [GetFileAttributesEx](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/Common/src/Interop/Windows/Kernel32/Interop.GetFileAttributesEx.cs)、[DeleteFile](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/Common/src/Interop/Windows/Kernel32/Interop.DeleteFile.cs)、[FindFirstFile](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/Common/src/Interop/Windows/Kernel32/Interop.FindFirstFileEx.cs)にも条件付き変換がある。 | 存在確認や後片付けだけが通常表記のまま残ることを防ぐ。列挙用の末尾`\*`も長さに含める。 |
| コピーと改名 | [CopyFileEx](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/Common/src/Interop/Windows/Kernel32/Interop.CopyFileEx.cs)と[MoveFile](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/Common/src/Interop/Windows/Kernel32/Interop.MoveFileEx.cs)で入力と出力を両方変換する。 | 移動元だけでなく移動先、一時ファイル、バックアップにも適用する。 |
| ディレクトリ作成 | [CreateDirectory](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/Common/src/Interop/Windows/Kernel32/Interop.CreateDirectory.cs)は従来の248文字制限を避けるため、条件なしの接頭辞付加を選ぶ。 | 正規化済みの作成先を常に拡張長表記にする。259・260の境界試験だけでは不足する。 |
| 再帰削除 | [FileSystem.Windows.cs](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Private.CoreLib/src/System/IO/FileSystem.Windows.cs#L250)は再帰に入る前に拡張長表記へ変換する。末尾空白・ピリオドのある子を扱い、別の場所を指す再解析ポイントへは再帰しない。 | 列挙で得た子の名前を通常入力として再正規化しない。リンク先を削除しない既存方針も保つ。 |
| シンボリックリンク | [CreateSymbolicLink](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/Common/src/Interop/Windows/Kernel32/Interop.CreateSymbolicLink.cs)で作成先とターゲットを条件付き変換する。未解決の相対ターゲットには接頭辞が付かない。 | ターゲットを通常のファイルアクセス先として絶対化しない。 |
| 監視と列挙のハンドル | [FileSystemWatcher](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.IO.FileSystem.Watcher/src/System/IO/FileSystemWatcher.Win32.cs)と[列挙処理](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Private.CoreLib/src/System/IO/Enumeration/FileSystemEnumerator.Windows.cs)も変換付きのファイルハンドル取得を使う。 | muonの既存watch方式の入口へ適用する。.NETの監視方式や内部のNT APIまで移植しない。 |

ディレクトリを段階的に作る処理では、.NETはルートの長さを求めて親をたどる。ドライブルートやUNC共有そのものを作成対象にしない設計は、builderの再帰mkdirにも適用できる。[FileSystem.DirectoryCreation.Windows.cs](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/Common/src/System/IO/FileSystem.DirectoryCreation.Windows.cs)

### 戻り値とエラー

.NETが内部で接頭辞を付けることと、公開APIが常に接頭辞付き文字列を返すことは同じではない。例えばリンクの最終ターゲットを取得する処理は、入力が拡張長表記かどうかで返す表記を変える。muonの`realpath`と`readlink`には独自の公開仕様があるため、この返却処理を機械的にコピーしない。[GetFinalLinkTargetの実装](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Private.CoreLib/src/System/IO/FileSystem.Windows.cs#L665)

Win32呼出しの失敗は保存したエラー番号から例外へ変換される。muonでも元の操作名、利用者に分かるパス、Windowsエラーを対応づける。拡張長パス化は長さや表記の問題を扱うだけであり、権限不足や共有違反を別のパスで再試行する理由にはしない。[.NETのファイル操作とエラー処理](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Private.CoreLib/src/System/IO/FileSystem.Windows.cs)

### 採用する範囲

muonへ採用するのは、正規化とI/O用表記の分離、Unicode APIの使用、操作ごとの接頭辞付加、動的バッファ、名前の意味を保つ往復である。.NETの`PathInternal`等は実装の参考であり、muonから呼ぶ公開APIではない。.NETランタイムへの依存や外部ソースのコピーは追加しない。

.NETの8.3名の展開、.NET Frameworkの互換性スイッチ、NT APIによる列挙、監視方式は対象外とする。また、.NETの`MoveFile`が選ぶコピー許可フラグをmuonの原子的な更新へ流用しない。各操作の失敗条件、リンクの扱い、置換の原子性はmuonの既存仕様に従う。

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

[muon_builtin_fs_helpers.cpp](../../../core/cef/src/plugins/builtin/muon_builtin_fs_helpers.cpp)の`CreateLocalFilesystemPath`は、ファイルURIを処理した後、WindowsではUTF-8をUTF-16へ変換する。絶対化や拡張長表記への変換は行わない。[muon_builtin_fs.cpp](../../../core/cef/src/plugins/builtin/muon_builtin_fs.cpp)の通常の読書きは`CreateFileW`を使うため、その直前にI/O用パスの変換を追加できる。

ただし、すべての操作が同じ実装ではない。`exists`、ディレクトリ列挙、作成、削除、改名、コピー、truncateなどは`std::filesystem`、追記は`std::ofstream`を使用する。`watch`のWindows側はディレクトリのスナップショットを作る方式であり、`std::filesystem`と属性取得を使用する。`CreateFileW`の成功だけを全APIの対応確認にしない。

`RealpathAsync`と`ReadWindowsSymbolicLinkTarget`は結果から`\\?\`等を除去する。通常表記で同じ対象を表せる結果は従来の表記へ戻し、次の操作で再び内部変換する。接頭辞を外すと意味が変わる名前は後述の返却規則で扱う。`SymlinkAsync`は`CreateLocalFilesystemPath`を作成先とターゲットの両方に使っているため、このヘルパーで一律に絶対化してはならない。

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

CEF側の`NormalizePath`も確認したが、`MakeAbsoluteFilePath`を呼ぶ部分はPOSIX条件内だった。「Windowsのプロファイル設定が、この関数だけで必ず拒否される」とは判断しない。プロファイルについては起動だけでなくCookieやlocalStorageの保存、再起動、キャッシュの更新と削除まで確認する。[参照CEFのcontext.cc](https://github.com/chromiumembedded/cef/blob/76d2442/libcef/browser/context.cc)

### シェル、ダイアログ、自動起動、NSIS

[muon_builtin_fs_dialogs.cpp](../../../core/cef/src/plugins/builtin/muon_builtin_fs_dialogs.cpp)は`IFileDialog`と`SHCreateItemFromParsingName`を使用する。初期フォルダーの解析に失敗しても、その場ではエラーを通知せずに表示を続ける経路がある。長い初期パス、選択結果、保存先と、その後の`muon.fs`操作を一続きで確認する。ファイルシステムAPIとシェルの対応範囲は同一ではない。[Microsoftのシェルに関する注意](https://learn.microsoft.com/en-us/windows/win32/fileio/maximum-file-path-limitation)

[muon_autostart.cpp](../../../core/cef/src/config/muon_autostart.cpp)の`kWindowsRunCommandMaxLength=260`は、Runキーの仕様に対応した制限である。長いパス対応のためにこの定数を拡大してはならない。引用符などを含む登録コマンドが制限内なら登録し、超える場合の既存エラーを確認する。別の自動起動方式の追加は本計画に含めない。[RunとRunOnceの公式説明](https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys)

NSISには`ManifestLongPathAware true`があるが、公式資料は`CopyFiles`と`CreateShortcut`などが長いパスに対応しないと明記している。変数の既定サイズも1024文字である。現在の生成スクリプトはショートカット作成、起動、アンインストーラー生成、状態ディレクトリの再帰削除を含むため、宣言の追加だけで任意長のインストール先を保証できない。[NSISのマニフェスト設定](https://nsis.sourceforge.io/Docs/Chapter4.html#longpathaware)、[NSISの変数](https://nsis.sourceforge.io/Docs/Chapter4.html#variables)

## 実装方針

### パス表現と所有権

公開JavaScript APIではパスをUnicode文字列として扱い、ネイティブ側へ渡す文字列とJSONファイルの符号化にはUTF-8を使用する。Windowsのファイル操作へ渡すところでUTF-16へ変換する。既に`std::filesystem::path`で保持しているWindowsパスは、そのネイティブ表現を利用する。UTF-8とANSIコードページの文字列を同じ型のまま往復させない。

Cのbuilderでは動的に確保した変換結果と列挙結果の解放元を明示する。C++側は同じパス変換を薄いラッパーから利用する。共通化するのはパスの分類・正規化・表記変換であり、CとC++のファイル操作全体を新しい抽象化層へ移さない。

起動引数はWindows用のUnicode入口で取得する。`wmain`を採る場合は、MinGWのUnicodeエントリーポイント設定とGUIランチャーのリンク条件を合わせて変更する。環境変数もwide版で取得してからUTF-8へ変換する。OS全体のコードページ変更を前提にしない。[wmainとUnicode環境変数の説明](https://learn.microsoft.com/en-us/cpp/c-language/using-wmain?view=msvc-170)

パス取得のバッファは、APIが返す必要サイズと終端NULの規則に従って確保する。`PATH_MAX`を別の大きい定数へ置き換えるだけでは完了としない。絶対化が必要な場合は元の基準ディレクトリを保ち、他スレッドのためにプロセス全体の作業ディレクトリを変更しない。[GetFullPathNameWのサイズと相対パスの注意](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfullpathnamew)

拡張長パスの変換を導入する箇所では、ローカル絶対パス、UNC、既に接頭辞があるパスを区別する。デバイス名や名前付きパイプをファイルパスとして加工しない。シンボリックリンクの相対ターゲット、ファイルURI、公開APIの戻り値も、用途に合わせて扱う。

### 共通の変換処理

`core/common`にWindows専用の小さなC99モジュールを追加する。配置案は`include/muon_windows_path.h`と`src/muon_windows_path.c`とする。builderのMakefileとコアのCMakeから同じソースをビルドし、C++側はスコープ終了時に変換結果を解放する薄いラッパーを使う。新しい配布DLLや.NETへの依存は設けない。

変換処理では次の情報を区別する。

| 情報・処理 | 規則 |
| --- | --- |
| 利用者の入力 | UTF-8の文字列と長さを受け取る。埋込みNULと不正UTF-8を拒否する。空入力の扱いは各公開APIで維持し、カレントディレクトリへ勝手に置き換えない。 |
| 正規化したパス | 通常入力を元の基準で絶対化する。設定の保存、パスの比較、診断に使う表記と、I/O用の表記を区別する。存在確認や`canonical`によるリンク解決は正規化に混ぜない。 |
| I/O用パス | 対応するWindows APIやCRTへ渡すUTF-16文字列とする。ファイル、ディレクトリ作成、列挙から得た名前など、用途に応じて接頭辞を付ける。 |
| 明示的な拡張長パス | 入力時点で識別し、通常入力向けの区切り置換や`.`・`..`の除去を行わない。接頭辞を二重に付けない。 |
| 列挙で得た名前 | 既に正確な名前として扱い、拡張長表記の親と結合する。末尾空白・ピリオドを消す再正規化を避ける。 |
| リンクの相対ターゲット | URIの処理と文字列変換を済ませた後も相対のまま保つ。ファイルI/O用の絶対化を呼ばない。 |

Cの結果は所有するバッファと長さ、元の入力が明示的な拡張長形式だったかを必要な範囲で保持する。解放関数と失敗時の空状態をヘッダーのdoxygenコメントに明記する。UTF-16の文字数からバイト数や`DWORD`へ変換する前にオーバーフローを確認する。バッファ不足時はAPIごとの終端NULの規則に従って拡張し、失敗時のエラーは解放処理より先に保存する。

`GetFullPathNameW`は公開仕様とMinGWヘッダーを照合して使い、`LongPathsEnabled=0`で4096文字超を含む正規化とファイル往復を実機確認する。.NETの説明には接頭辞なしでも長い入力を解決できる旨があるが、muonの対象環境でも同じ結果になることを段階1で検証する。失敗した場合は、単に接頭辞を先に付けて`.`や`..`の意味を変えず、正規化の設計を再検討する。[.NETの正規化の説明](https://learn.microsoft.com/en-us/dotnet/standard/io/file-path-formats#skip-normalization)

### 入力の分類と変換順序

通常の入力はURIのデコード、厳密なUnicode変換、入力形式の分類、基準ディレクトリでの正規化、操作に応じた接頭辞付加の順で処理する。URIは既存の許可条件を保ち、デコードで生じたNULも拒否する。Windows固有の表記へ変換した文字列を、もう一度URIとして解釈しない。

| 入力 | muonでの方針 |
| --- | --- |
| `C:\base\..\data\file`、`C:/data/file` | 通常表記として正規化してから、必要な操作へ拡張長表記を渡す。入力の長さではなく、解決後の絶対パスで判定する。 |
| `sub\file` | APIで決まる基準から一度だけ解決する。`muon.fs`では対象操作の開始時、設定では既存の設定ファイル基準を使う。再試行でも同じ結果を利用する。 |
| `C:sub\file`、`\sub\file` | ドライブ相対と現在ドライブのルート相対を区別する。既存の基準とWindowsの解決規則を確認し、単純な文字列結合でドライブルートに置き換えない。 |
| `\\server\share\dir\file` | UNCのサーバー名・共有名をルートとして保ち、`\\?\UNC\server\share\dir\file`へ変換する。 |
| `\\?\C:\...`、`\\?\UNC\...` | 明示された表記を保つ。`/`への区切り統一や`lexically_normal()`を通す前に分類する。 |
| `\\.\...`、`\??\...`、ボリュームGUIDなど | 通常のドライブ・UNCパス用変換から除外する。受渡し先の既存仕様に従い、未対応なら明示的に失敗させる。今回の変更で扱えるデバイスを増やさない。 |

相対入力の基準を確定するためにプロセス全体のcwdを変更しない。設定読込み、非同期ファイル操作、builderの再帰処理ごとに、決めた基準を最後まで使う。`..`の処理をファイルの存在に依存させる`canonical`で代用すると、新規作成先やリンクを含むパスの挙動が変わるため、正規化と対象確認は別の処理にする。

通常表記と拡張長表記では、末尾空白・ピリオドや予約デバイス名の意味が変わり得る。通常入力の`NUL`や`CON`等を、接頭辞付加によって同名の通常ファイルへ変えることは認めない。既存の扱いを保てない変換はI/O前にエラーとする。明示的な拡張長表記で指定したファイル名はその表記の意味を保つ。この判定はmuonで誤操作を防ぐための方針であり、.NETの条件付き付加だけで同じ保証が得られるとは扱わない。[Windowsの命名規則](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file)

### I/Oの用途と接頭辞

通常のファイル操作は.NETと同様に、正規化後の長さが260以上なら拡張長表記を使う。既に正確な名前として受け取ったパスに末尾空白・ピリオドが残る場合も、その意味を維持する表記を使う。ディレクトリ作成と再帰走査では、正規化済みの開始パスに常に接頭辞を付ける。短い親から長い子へ進む場合や、列挙で得た名前を再利用する場合も同じ規則で扱う。

`CreateLocalFilesystemPath`にはURIとUnicodeの処理を残し、通常のファイルI/O用変換を別に設ける。各操作は用途を明示して新しい変換を呼ぶ。`SymlinkAsync`では作成先にI/O用変換を適用し、ターゲットは絶対・相対を分けて扱う。全呼出し元を調べずに既存ヘルパーの返却値を変更しない。

列挙用の検索パターンはディレクトリの正規化後に組み立て、最後にAPIへ渡す文字列の長さを確認する。ドライブルート、UNC共有ルート、末尾区切りを区別し、ルートをmkdirや再帰削除の中間要素として扱わない。再解析ポイントの追跡条件は現在のAPI仕様を保つ。

### 公開APIの戻り値

`realpath`は、通常表記で同じ対象を表せるなら通常の絶対パスを返す。内部で追加した`\\?\`だけを除き、UNCは`\\?\UNC\server\share\...`から`\\server\share\...`へ戻す。戻した結果はOS設定が無効でも、次のmuonの操作で再変換できることを確認する。

明示的な拡張長入力の接頭辞と、除去すると名前が変わる結果は保持する。例えば末尾ピリオドや空白を含む名前、通常表記ではデバイスとして扱われる名前、通常のドライブ・UNC表記へ戻せないボリューム名が該当する。通常表記へ戻して正規化した文字列が同じ名前を表すと確認できない場合も、拡張長表記を残す。短い別名ファイルを同時に用意し、返却パスが別の対象へ向かわないことをテストする。

`readlink`は相対ターゲットをそのまま返す。絶対ターゲットの名前空間を変換する場合も、同じ対象を保つ上記の規則に従う。`readdir`はファイル名をそのまま返し、watchのイベントパス、JSON、エラー表示には内部都合で接頭辞を混入させない。正確な名前を表すために必要な接頭辞は残す。

### 適用箇所と責務

| muonの箇所 | 適用内容 | 維持する条件 |
| --- | --- | --- |
| コアの`CreateFileW`、属性、`std::filesystem`、追記、watch | ファイルI/O用の変換を通す。CRTで失敗する操作だけW版APIへ置換する。 | 非同期処理、キャンセル、範囲指定、上書き条件。監視方式を変更しない。 |
| builderの`common.c`と`prepare*.c` | 共通のC99変換を使い、直接の狭いI/Oも置換する。`muon_normalize_path_separators`へ渡す前に明示的な拡張長入力を識別する。 | コピー内容、fingerprint、更新・復元・削除、再解析ポイントの扱い。 |
| 設定、アセット、ログ、ハッシュ | 通常の設定表記を保ち、実際に開く側で変換する。ZIP本体の読込みも含める。 | 設定の相対基準、アセットの配信範囲、署名検査。 |
| `canonical`を使うアセットの配信範囲検査 | ルートと対象の両方を同じ規則で開き、リンク解決後のパスを同じ表記・要素の単位で比較する。 | 接頭辞を外した文字列の前方一致だけで許可しない。範囲外のリンクは従来どおり拒否する。 |
| libarchiveとengraver | 公開W版または公開I/O差替えへI/O用パスを渡す。 | エントリー名の検査、所有権、既存PEリソース。外部コードを編集しない。 |
| プラグインとinspector | 検査用I/Oを先に対応し、DLLパスのUTF-8往復を揃える。ロード用表記は`LoadLibraryExW`等の公開仕様と実機で確認する。 | 隣接依存DLLの探索、署名、エラーメッセージ。 |
| launcher、executor、Node.js | 引数と環境変数をUnicodeで扱う。実行ファイルを指定する引数はファイルI/Oとは別に検証する。 | コマンド名検索、cwd、引数の引用、子孫の終了。 |
| CEF、ダイアログ、NSIS、自動起動 | 内部I/O用パスを無条件で渡さず、受渡し先の制約を別途検証する。 | CEFの配置条件、シェルの対応範囲、Runキーの上限。 |

### ファイル操作の置換

Windows用の`open`、属性取得、列挙、作成、削除、移動をmuon側の共通処理へ集約し、`prepare.c`等の直接呼出しも通す。W版CRTで長いパスを扱えることを実際のビルドで確認する。失敗する操作は`CreateFileW`、`GetFileAttributesExW`、`FindFirstFileW`などの公開APIで扱う。CRTやlibstdc++の全面的な入替えは前提にしない。

`std::filesystem`を使うコア側は操作ごとの試験で不足を特定する。正常な実装まで一括でWin32へ書き換えず、失敗を再現した操作だけを修正する。改名・更新の原子性、上書きの条件、リンクをたどるかどうか、ファイル時刻なども元の用途に合わせて保つ。

Win32 API失敗時は直後の`GetLastError`を保存して報告する。例えば現在の`muon_atomic_replace`はWin32 API失敗後に`errno`を表示するため、この置換に合わせて診断も修正する。文字列の切捨てや、不足したパスで処理を続行することは許容しない。

### 起動場所とCEFの制約

基本的な利用形態はCEF本体と子プロセスを起動できる場所にruntimeを置き、長い場所にあるデータ、設定、アセット、プラグイン等へアクセスする構成とする。配布元を長い場所に置くことと、CEFの実行場所を長くすることは別の試験項目にする。

既存の`user-state`方式は配布元とruntimeの配置を分けられる。ただし、`LOCALAPPDATA`自体が長い場合もあるため、この方式だけで短い実行場所が保証されるとは扱わない。`in-place`方式は実行場所の制約を受ける。[現在のインストール先の説明](../package-install-location-advanced-topics.md)

CEFが使うruntime、プロファイル、TEMPに制約が残る場合は、実機結果に基づいて対応範囲を記載する。明示されたプロファイルを別の場所へ黙って移したり、保存失敗をメモリー内の動作へ置き換えたりしない。短い場所への配置が必要な場合は、その条件と設定方法を利用者へ示す。

CEFの実行場所やプロファイル自体を無制限に長くすることが必須になった場合は、外部コードを変更しない条件で成立するかを先に確認する。新しい配置方式、パスの別名、CEFの変更などを暗黙に追加しない。既存の公開設定で解決しなければ、その対応を追加計画として確定する。基本案のファイル操作への対応と、この追加範囲の未達は分けて記録する。

### 配布と外部ツール

PEのリソース更新後、配布用コピー後、署名後の実行ファイルで、マニフェストと動作が維持されることを確認する。リソース更新は署名前に完了させ、署名済みの成果物を後から編集しない。

curlは別プロセスなので、muonのマニフェストだけで長い取得先へ書けるとは限らない。実際のWindows用curlで確認する。失敗する場合は、短い一時領域へ取得した後、muonのファイルI/Oで最終キャッシュと同じディレクトリの一時ファイルへコピーし、検証後に確定する方式を検討する。異なるボリュームからの移動を原子的な更新とみなさない。

NSISでは通常のインストール、ショートカットからの起動、終了後のアンインストールまで成功する配置を確認する。未対応の長さはインストール前に検出し、途中まで展開した状態を成功として返さない。NSIS自体の改造、外部バイナリのマニフェスト書換え、対応していない配布形式の追加は行わない。

## 検証方法

### テスト環境

必須の検証環境は`LongPathsEnabled=0`で通常権限のプロセスを実行するWindows実機とする。既存の接続先を利用し、管理者操作を実装開始の前提にしない。OSのバージョン、ポリシー値、プロセスの権限、CPU対象、CRT、CEF、Node.js、NSIS、curlのバージョンと成果物を記録する。

`LongPathsEnabled=1`の環境が利用できる場合は、同じ試験を追加の互換性確認として行う。共有PCの設定を変更したり、試験用に権限昇格したりしない。有効な環境が未提供であることは、設定が無効な環境での実装・必須検証を止める理由にしない。未実施の追加試験は別に記録し、両設定で実機確認したとは記載しない。

両CPU対象を実行して確認する。WineはbuilderやDLLの早期確認に利用できるが、WindowsのマニフェストとOS設定の組合せを検証した代わりにはしない。Wine固有の不具合と特定できた場合は、Wine向けの変更を追加せず実機へ切り替える。

実機試験は既存の`agent-rover`接続と[plugin-e2e-windows.test.ts](../../../core/cef-test/plugin-e2e-windows.test.ts)を利用する。ビジーなら接続できるまで待ち、実機が必要な試験が終わったら解放する。現在の接続処理は接続失敗時にskipを返すため、テストコマンドの終了コードだけで実機確認済みと判断しない。接続と対象テストの実行件数まで確認する。長いパスの必須実行では設定値が`0`であることを確認し、値が異なる・取得できない場合は前提不成立として報告する。設定が`0`であることを理由とするskipは廃止する。設定値の前提確認と動作のアサーションを分け、追加の`1`の試験でも同じ本体を使う。

試験用ファイルの準備や後片付けが先に長さ制限へ当たらないようにする。短い制御ディレクトリから、Unicodeと拡張長パスを扱えるテストドライバーで対象を作成する。agent-roverのファイル転送、PowerShell、`cmd.exe`の成功を、muonの成功条件に混ぜない。ブラウザーを介した操作と観測にはPlaywright MCPを使う。

### パスの組合せ

| 観点 | ケース |
| --- | --- |
| パス全体の長さ | 終端NULを除くUTF-16長が259、260、261となるファイルの境界と、247、248、249となるディレクトリ作成の境界。300程度、1024超、4096超の多段パスも使う。各要素の長さはファイルシステムの上限内にする。 |
| 文字 | ASCII、空白、日本語、使用中のANSIコードページでは表せない文字、サロゲートペアを含む名前。UTF-8では260バイト超でもUTF-16では短いケースを含める。 |
| 表記 | ドライブ絶対パス、短い基準ディレクトリからの相対パス、ドライブ相対、現在ドライブのルート相対、通常UNC、明示的な拡張長ローカル・UNC、ファイルURI。 |
| 正規化 | `.`と`..`、`/`と`\`、末尾の区切り、既存の接頭辞、相対シンボリックリンク。正規化で短くなる入力と、基準の付加で長くなる入力を含める。 |
| 正確な名前 | 通常名と、末尾空白・ピリオド付きの別名を同時に作る。通常入力、明示的な拡張長入力、列挙結果の再利用を区別し、操作した対象の内容で確認する。 |
| 失敗 | 存在しないパス、権限不足、共有違反、名前の要素が長すぎる場合、接頭辞付加後にOSの限界を超える長さ、不正UTF-8、埋込みNUL、予約デバイス名の誤変換。切捨て、別対象へのアクセス、後片付け漏れがないこと。 |

長さは最終的な絶対パスから計算する。テストの作業ディレクトリ名が変わっても、狙った長さになっていることを実行時に確認する。相対パスは入力自体の長さと、解決後の長さを分けて記録する。

### .NETの分析から追加する確認

.NETのテストを移植してソースの形を確認するのではなく、muonの公開APIを動かした結果を確認する。共通変換のテストでは実際の関数の返却値とエラーを確認する。ネイティブのテストドライバーはその変換結果でファイルを開き、内容と対象を確認する。製品コアのE2Eは別に行う。[.NETの接頭辞・デバイス判定のテスト](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/Common/tests/Tests/System/IO/PathInternal.Windows.Tests.cs)

| ケース | 確認する結果 |
| --- | --- |
| OS設定が`0`の通常入力 | 接頭辞のない入力から、作成、読書き、属性、列挙、コピー、改名、削除まで成功する。製品が内部で変換する前にテスト側で接頭辞を付けない。 |
| ディレクトリの247〜249と長い検索パターン | ディレクトリを実際に作成でき、ファイル名を列挙できる。末尾`\*`を加えた長さによる漏れも検出する。 |
| 長い相対入力と`.`・`..` | 意図した基準のファイルを読み書きする。別の基準に置いた同名ファイルを変更しない。ファイル未作成でも親の解決ができる。 |
| 明示的な接頭辞 | 二重付加せず同じ対象を操作する。一般的な区切り置換や正規化によって別のパスへ変わらない。 |
| `realpath`・`readlink`・ダイアログの結果 | 返却値をそのまま次のAPIへ渡し、内容またはファイルの識別情報が一致する。接頭辞の有無だけを合否にしない。 |
| 列挙と再帰削除 | 末尾空白・ピリオドを含む子を正確に扱い、隣の通常名ファイルやツリー外のリンク先を変更しない。 |
| 相対リンク | `..\target`等を保存し、`readlink`でも相対のまま返す。親ディレクトリを移動しても相対リンクとして解決する。リンク作成権限の可否は長いパス対応とは分けて記録する。 |
| コピー・更新・復元 | 長い入力と出力の両方を通す。出力側の失敗でも旧runtime、元ファイル、署名検査の結果を維持する。 |
| エラーと上限 | NUL以降を切り捨てて成功しない。無効入力やOS上限超過で別名のファイルを作らない。割当ての解放と元のエラーが保たれる。 |

UNCは共有名を含めた文字列長とルートの変換を確認する。共有先への実際のI/Oは、以下の追加指示に従い、ネットワーク環境の準備が必要なものを今回の対象から除く。文字列変換の成功と共有先へのI/Oの成功を区別して記録する。

### .NET BCLに対応するローカル試験

.NET 10.0.0のBCL（基本クラスライブラリ）のテストを調査し、以下の振る舞いをmuonへ移す。参照するタグを固定し、元のテスト名とmuon側の検証内容を対応づける。C#の例外型や.NET専用のAPIを再現するのではなく、muonの公開APIで操作結果・内容・対象の同一性を確認する。

| .NETの試験と出典 | muonで確認する振る舞い |
| --- | --- |
| [IOInputs.GetPathsLongerThanMaxPath / GetPathsLongerThanMaxDirectory](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/PortedCommon/IOInputs.cs) | 終端NULを除くUTF-16長でファイル260・261・262、ディレクトリ248・249・250を生成する。既存の259・247と300・1024超・4096超も残す。 |
| [Directory_CreateDirectory.DirectoryLongerThanMaxPath_Succeeds / ExtendedDirectoryLongerThanLegacyMaxPath_Succeeds](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/Directory/CreateDirectory.cs) | 通常入力と明示的な拡張長入力から階層を作成し、存在と内容を確認する。末尾区切り、既存ディレクトリ、途中にファイルがある失敗も含める。 |
| [File_Create.ValidCreation_LongPathExtendedSyntax / CreateInParentDirectory](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/File/Create.cs) | 500文字の拡張長ディレクトリ内のファイル作成と読書き、存在しない要素を含む `missing/../file` の解決を確認する。 |
| [Directory_GetFiles.EnumerateFilesOverLegacyMaxPath / EnumerateFilesDirectoryOverLegacyMaxPath](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/Directory/GetFiles.cs) | 親が250文字で子を含めると260前後になる場合と、親自体が270文字の場合に、7〜12文字の異なる6個の名前を列挙し、各ファイルの内容を照合する。 |
| [File_Move.OverMaxPathWorks_Windows](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/File/Move.cs) | 短いファイルを260・261・262文字の移動先へ改名し、元の場所へ戻す。移動元の消失と内容の保持を往復で確認する。 |
| [File_Copy.ShortenLongPath](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/File/Copy.cs) | 多数の `/.` を含む長い入力がコピー元自身に解決される場合、コピーを拒否して元の内容を保持する。正常なコピーでは入力と出力の両方を長くする。 |
| [PathTests.GetFullPath_NormalizedLongPathTooLong / GetFullPath_PathTooLong](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.Runtime.Extensions.Tests/System/IO/PathTests_Windows.cs) | 正規化後も260を超える入力、正規化で短くなる入力、絶対化により長くなる入力を分ける。32767・32768文字の拒否は試験プロセス内で生成し、プロセス起動の引数上限と混同しない。 |
| [Directory_CreateDirectory.DirectoryWithComponentLongerThanMaxComponentAsPath_ThrowsException](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/Directory/CreateDirectory.cs) | 全体長と要素長を区別し、255文字の名前を扱えても256文字の要素は拒否すること、失敗時に短く切った別名を作成・更新しないことを確認する。 |
| [Directory_Delete.LongPathExtendedDirectory / RecursiveDeletingDoesntFollowLinks](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/Directory/Delete.cs) | 長い階層を削除し、再解析ポイントの先にあるツリー外のディレクトリと内容を残す。リンク作成権限の制約は長いパスの成否と分ける。 |
| [EnumerableTests.EnumerateDirectories_TrailingDot](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/System.Runtime/tests/System.IO.FileSystem.Tests/Directory/EnumerableTests.cs) | 通常名と末尾ピリオド・空白を持つ名前を同時に用意し、列挙後のコピー・削除で同名に正規化しない。muonではファイルにも同じ確認を加える。 |
| [PathInternalTests_Windows.EnsureExtendedPrefixTest / IsExtendedTest / GetRootLength_Data](https://github.com/dotnet/runtime/blob/v10.0.0/src/libraries/Common/tests/Tests/System/IO/PathInternal.Windows.Tests.cs) | 二重の接頭辞、区切り、ドライブルート、UNCルート、予約名を共通変換の契約に照らして確認する。muonで対象外としたデバイス名前空間は、.NETの許容範囲へ広げない。 |

共通変換の試験は段階1のドライバー、builderの試験は段階2、製品のファイルAPIの試験は段階4へ追加する。builderのドライバーだけで公開APIの確認を済ませず、製品E2Eでも対応する操作を実行する。各段階で不足する動作のREDを確認してから修正し、両CPU対象の実機でGREENを確認する。

利用者の追加指示により、共有フォルダーの作成、共有先の権限設定、ネットワーク接続などの環境準備が必要な試験は今回の対象から除く。UNCの文字列変換はネットワークI/Oなしで確認する。除外した共有先へのI/Oを成功扱いにはせず、製品ドキュメントで実機確認済みの範囲を超えて説明しない。


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

### 段階1 共通変換と基本的なファイル操作

この段階は単独で動かせるネイティブドライバーと、製品コアでの往復の2つに分ける。どちらも`LongPathsEnabled=0`の実機でREDからGREENを確認する。未実装関数が存在しないだけのコンパイル失敗は、振る舞いのREDには数えない。

段階1aでは、既存のUTF-16変換と通常表記のI/Oで長いパスの操作が失敗するドライバーを用意する。Makefileから両CPU対象でビルドし、独立したフィクスチャに対して実行する。その後、共通C99モジュールを追加し、正規化、接頭辞付加、動的バッファ、NUL検査、UNC分類を実装する。ファイルの259〜261とディレクトリの247〜249、4096文字超、相対入力の往復を成功させる。段階1aの成果物は変換後の実際のファイル操作を観測できるドライバーと、コア・builderへ組み込める共通ソースである。

段階1bでは作業ツリーの試作テストにある`LongPathsEnabled=1`を要求するskipを新しい前提確認へ変更する。短いruntime・profile・TEMPから、製品コアへ通常表記の長いパスを渡し、読書きのREDを確認する。コアの基本読書きだけを共通変換へ接続してGREENにし、相対ターゲット等の未移行経路へ一律の絶対化が及ばないようにする。

コア、ランチャー、builder、inspectorのマニフェストとリソースの依存関係も整える。既存のOS互換性、Common Controls、バージョンリソースを維持する。マニフェストを追加しても設定が`0`のまま試験する。CEFの実行場所、profile、TEMPを一つずつ長くした試験は、以降の配置条件を得るための別枠として記録する。

段階1全体の完了条件は、両CPU対象の製品コアで接頭辞なしの長い入力からファイル往復が成功し、短いパスの既存試験も成功すること。共通ドライバーでディレクトリの境界と正規化を確認し、4種類の製品EXEへ宣言が埋め込まれていることも補助検査する。設定が`0`のために必須試験がskipされる状態は未完了とする。

コミットの目安は`feat: add Windows filesystem path conversion`、`fix: handle long file paths without Windows policy changes`、`feat: enable long path awareness for Windows executables`。段階1aと1bのそれぞれで動作する成果物を保つ。

### 段階2 builderのパスと基本I/O

`muon_ensure_directory`、実行ファイルパス取得、コピー・列挙・削除・属性取得について、260文字超と非ASCII文字で失敗する振る舞いのテストを追加する。失敗を確認してから、動的バッファとUnicodeの入口、Windows用I/Oを実装する。必要なテストドライバーはMinGWとMakefileで実行可能にする。

段階1の共通変換へ接続し、UNCのルート、明示的な拡張長パス、環境変数、引数、Windowsエラーの報告を揃える。再帰mkdirはルートを除く正規化済みの作成先へ接頭辞を付け、列挙・再帰削除は正確な子の名前を保つ。`prepare.c`などに残る直接の狭いファイル操作を置換する。Linux側は既存実装を利用し、使われなくなったWindows専用ヘルパーを残さない。

完了条件は設定が`0`の両CPU対象で長い階層の作成、内容を保ったコピー、列挙、改名、削除が成功し、UTF-8とUTF-16の変換による文字欠落がないこと。末尾空白等の異なる名前とリンク先を誤操作しないことも確認する。4096を超える入力でもmuon独自の固定バッファに妨げられず、割当て失敗やOS側の拒否を正しく返すこと。builderの既存テストも成功させる。

コミットの目安は`fix: handle Windows builder paths with Unicode APIs`。入口、列挙、更新処理が大きくなる場合は、テストが成功する単位で分割する。

### 段階3 取得と展開とリソース更新

CEFのtar.bz2とNode.jsのZIPについて、アーカイブ本体と展開先の両方を長くした失敗を再現する。libarchiveの公開W版、出力I/O、直接のrename、fingerprint、キャッシュの確定処理を対応させる。curlはローカルのテストHTTPサーバー等で外部ネットワークに依存せず確認する。

PEリソース更新にも長い入力・出力・JSON・アイコンを使い、engraverの公開I/O差し替えAPIへ接続する。読み書きに失敗したときの解放、部分出力の削除、既存PEリソースの維持を確認する。

完了条件は設定が`0`の実機で、長いパスによる取得・展開・ハッシュ検査・再利用・更新・復元・後片付けが成功すること。入力と出力、バックアップの全経路を共通の規則へ接続する。更新したPEが起動し、マニフェストと要求したリソースが維持されること。外部コードを変更せず、壊れたアーカイブ等を成功扱いにしないこと。

コミットの目安は`fix: support long Windows paths during runtime preparation`と`fix: use Unicode I/O for Windows resource updates`。

### 段階4 コアのファイルAPIとプラグイン

既存の[runtime-api.ts](../../../core/cef-test/plugin-e2e/runtime-api.ts)にあるファイルAPIの振る舞いを長いパスでも検証し、失敗した操作を先にテストで固定する。read/writeだけでなく、`std::filesystem`、追記、watch、realpathの戻り値、相対リンクを含める。

設定、アセット、ログ、署名確認、DLL読込みの各入口へ同じ種類のパスを渡す。UTF-8の文字列変換が不足する箇所と、使用中のCRTで長さ制限が残る箇所を分けて修正する。通常表記とI/O用表記の比較によってアセットの許可判定が変わらないようにし、配信範囲やDLL探索条件を維持する。

戻り値の接頭辞を一律に外す実装を見直し、通常表記で同じ対象を扱える場合と、拡張長表記を残す必要がある場合を分ける。`SymlinkAsync`のターゲット、`readlink`の相対結果、列挙された正確な名前、watchの返却パスを個別に移行する。各移行を公開APIの往復テストで確認してからコミットする。

完了条件は設定が`0`の両CPU対象で、機能ごとの確認表にあるコアの対象操作と返却パスの再利用が成功すること。相対リンクの意味と通常名・末尾空白等の名前の区別が保たれ、署名不一致や配信範囲外の拒否も維持されること。CEF内部の操作に関する制約はmuonのファイルAPIの不具合と混同しない。

コミットの目安は`fix: support long Windows paths across filesystem APIs`と、設定・プラグイン等の振る舞いごとの`fix:`。

### 段階5 子プロセスとランチャー

設定が`0`の実機で短いcwdから長いパスのテストEXEを起動し、executorの失敗を確認する。パス指定の実行ファイルを`application_name`へ明示し、コマンド名検索の既存ケースも成功させる。プロセス起動で受け付ける表記を検証し、ファイルI/O用の接頭辞付加をコマンドライン全体や一般の引数へ適用しない。引数の引用とUnicode環境変数、標準入出力、Job Objectの管理を含めて確認する。

ランチャーの起動処理もUnicode化し、準備済みruntimeへの起動と再起動を実行する。Node.jsは既存の明示的な実行ファイル指定を維持し、長いプロジェクトやbridgeを扱えるか確認する。長いcwdは独立した試験とし、OSとCEFの制約を超えるために別のcwdへ黙って変更しない。

完了条件は、muonが制御する子プロセス起動で長い実行ファイルパスとUnicode引数を扱え、コマンド名検索、終了コード、キャンセル、子孫の終了が従来どおり機能すること。配布元と実行先を分けたランチャー起動と再起動も成功すること。外部EXE自身の未対応は具体的な対象と条件を記録する。

コミットの目安は`fix: launch Windows executables with explicit Unicode paths`。

### 段階6 配布とWindowsのUI連携

NSISのマニフェスト設定を追加し、生成したインストーラーとアンインストーラーを実行する。インストール先、ショートカット、起動時のruntime準備、終了後の削除を確認する。未対応の配置では途中まで成功して見える状態を残さない。

ファイルダイアログは長い初期フォルダー、選択、保存、返却パスの再利用を確認する。対応できない条件がOS・シェル側にある場合は、根拠と実機結果を記録する。Runキーの上限は維持し、上限超過時のエラーと通常の自動起動を検証する。

完了条件はサポート対象として記載する配置で、配布・起動・利用・アンインストールの一連の操作が成功すること。CEF、NSIS、シェル、自動起動の残る制限が、利用者から見て判別できること。宣言を追加しただけの配布形式を対応済みと記載しない。

コミットの目安は`fix: preserve Windows long path support in packaged applications`。

### 段階7 利用文書と全体検証

日本語の[制限事項](../limitation.md)、[組み込みAPI](../muon-built-in-plugin-reference.md)、[インストール先](../package-install-location-advanced-topics.md)など、利用者が条件を確認する箇所へ反映する。muon自身の対象ファイル操作には管理者操作やOS設定変更が不要であること、通常表記で使える機能、返却パスの規則、CEF等に残る制約を分けて説明する。日本語を原本として既存の他言語文書へ反映する。一般文書から本計画書へのリンクは追加しない。

次の既存コマンドを基に全体を確認する。新設する個別テストはそれぞれの段階で適切な既存テスト入口へ組み込む。

```sh
npm run test --workspace muon-builder
npm run test:windows-e2e --workspace muon-core-tester
npm run test:windows-nsis-e2e --workspace muon-ui
npm run verify:package-runtime --workspace muon-ui
npm test
```

NSISの実機テストはルートの`npm test`だけに任せず明示的に実行する。パッケージ検証は対象別の配布物を生成・配置してから実行する。全体テストの所要時間に応じて十分なタイムアウトを設定し、実行中の進行を確認する。skipや環境不足を成功件数に含めない。

完了条件は、全体テストと設定が`0`の両CPU対象のWindows実機試験、NSISの実機試験、配布物検査が成功し、実装結果と利用文書が一致すること。新たな警告は原因を確認して解消する。必須項目を未検証のまま対応完了とはせず、設定が`1`の場合の追加試験も実施状況を明記する。

コミットの目安は利用文書の`doc:`と、検証で必要になった修正の`fix:`。

## 作業方法と完了判定

実装は上記の段階に従い、それぞれで失敗を再現する振る舞いのテストを追加してREDを確認し、修正後にGREENを確認する。複数の対処を試して原因が特定できた場合は、不要になった変更を戻す。外部依存の制限が計画の前提と合わなければ、変更範囲と完了条件を更新してから進める。

マニフェスト、Unicode化、ファイル操作の置換は既存機能の削除を目的としない。機能削除が必要になった場合は、削除を確認するテストのRED、削除後のGREENとコミット、最後にそのテストを削除するプロジェクトの手順に従う。既存コメントは維持し、内容が変わる箇所だけ更新する。

外部APIは公式資料と使用するバージョンのヘッダーコメントを照合する。libarchive、engraver、CEF、Chromium、MinGW等の外部コードは変更しない。公開されていないWindows内部構造を書き換えてlong path aware状態を作る方法も採らない。

実装全体の完了判定は次のとおりとする。

1. 4種類のmuon製品EXEで`longPathAware`が有効であり、配布処理後も維持される。
2. `LongPathsEnabled=0`のWindowsで、通常権限のmuonの対象ファイルAPIとbuilderの準備処理が通常表記の長いパスを扱える。レジストリ変更、権限昇格、再起動を要求しない。
3. Unicodeの引数、環境変数、設定、パス、列挙結果に文字欠落がなく、固定バッファで切り捨てない。
4. キャッシュ更新、アーカイブ検査、署名確認、DLL探索、アセットの配信範囲、子プロセスの管理を維持する。
5. CEFの実行場所・profile・TEMP、外部EXE、NSIS、シェル、Runキーの対応範囲が、根拠と実機結果に対応している。
6. 相対パスの基準、相対リンク、UNCルート、明示的な拡張長入力、列挙で得た名前、返却パスの意味を保つ。短いパスの既存動作を保ち、無効入力や上限超過時に別の対象を操作しない。
7. `windows-i686`と`windows-amd64`の実機試験、NSIS試験、配布物検査、全体テストが完了している。
8. 利用文書に、通常のファイル操作への対応と、CEFやシェル等の配置制約が区別して記載されている。

今回の計画追加の完了条件は.NETの公式資料・実装コメント・テストを根拠とする分析、muonへの適用箇所、パスの変換と返却の規則、各段階の成果物と検証方法を`plan12.md`へ保存することである。初版のOS設定依存の前提を本文、検証方法、各段階、全体の完了条件まで更新し、矛盾を残さない。実装段階の完了とは区別する。文書だけの変更でビルド工程へ影響しないため、今回の保存ではビルドと全体テストを実行しない。
