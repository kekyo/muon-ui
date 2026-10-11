# 外部プラグインの開発とNPM配布

`muon-plugin-sdk`を開発用依存へ追加すると、muon-uiのリポジトリを取得せずにC/C++のプラグインを作れます。CEFのLinux・WindowsとAndroidは同じC ABIを使用します。アプリへの登録とアクセス権限は、アプリ側の`muon.json`で指定します。

## 最初のプラグイン

アプリのディレクトリで実行します。標準はC++20とcardio、Makeとpkg-configです。SDKはmuon-uiと同じリリースを使用してください。

```bash
npm install -D muon-plugin-sdk
npx muon-plugin init native/counter --name counter
npx muon-plugin doctor --config native/counter/muon-plugin.build.json --target linux-amd64
npx muon-plugin build native/counter/muon-plugin.build.json --target linux-amd64
```

生成された`plugin.cpp`は`muon.counter.increment`を公開します。JavaScriptには`Promise<number>`を返し、cardioの非同期完了と終了コールバックを実装しています。C99を選ぶ場合は`init`へ`--language c`を付けます。C版はcardioをリンクせず、完了コールバックを使用します。初期化・型メタデータ・値の所有権については[プラグインABIの説明](muon-plugin-develop.md)を参照してください。

## 前提環境の診断と導入

開発ホストの標準はLinuxです。Windows向けはMinGW、Android向けはNDKでビルドします。`doctor`は選択した言語とビルド方式に必要なツール、SDK内の対象別リンク入力、sysroot、pkg-configの対象ライブラリを確認します。不足する項目をまとめて表示し、導入コマンドまたは公式文書を案内します。ビルド開始時にも同じ検査を行います。

| 必要なもの | Debian・Ubuntuでの導入例、または設定方法 |
| --- | --- |
| Node.js | SDKのpackage.jsonに記載された対応版を使用します。Node.js 24を使用できます。 |
| Make・pkg-config・GCC | `sudo apt install build-essential pkg-config` |
| C++とcardioのLinux開発ライブラリ | `sudo apt install libglib2.0-dev`。GIO 2.74以上が必要です。 |
| 独自CMakeビルド | `sudo apt install cmake`。CMake 3.24以上を使用します。 |
| Windows i686・amd64 | `sudo apt install mingw-w64` |
| Linux armhf・arm64のクロスコンパイラ | `sudo apt install g++-arm-linux-gnueabihf g++-aarch64-linux-gnu` |
| Android | Android command-line toolsから`sdkmanager "ndk;29.0.14206865"`を実行し、`ANDROID_HOME`を設定します。 |

[Node.jsの導入](https://nodejs.org/en/download)、[GLibのビルド条件](https://docs.gtk.org/glib/building.html)、[CMake](https://cmake.org/download/)、[MinGW-w64](https://www.mingw-w64.org/getting-started/)、[NDKの導入](https://developer.android.com/studio/projects/install-ndk)に詳細があります。診断コマンドはインストールやライセンスへの同意を行いません。

```bash
npx muon-plugin doctor --target windows-amd64 --language c
npx muon-plugin doctor --target android-arm64-v8a --android-sdk-path "$ANDROID_HOME"
npx muon-plugin doctor --config native/counter/muon-plugin.build.json --target linux-amd64 --json
```

クロスビルドでは、ホスト用のGLibなどを対象用ライブラリとして扱いません。対象CPU向けの開発ライブラリを用意し、`--sysroot`または`--pkg-config-directories`で場所を指定します。必要なら`--cc`と`--cxx`でコンパイラを指定できます。コンパイラ引数はこれらのオプションに含めません。

```bash
npx muon-plugin doctor --target linux-arm64 --sysroot /opt/sysroots/arm64
npx muon-plugin build native/counter/muon-plugin.build.json --target linux-arm64 --sysroot /opt/sysroots/arm64
```

外部ライブラリも診断に含める場合は、ビルド設定の`packages`へpkg-config式と導入方法を記述します。ライブラリ作者が対象環境に合う導入方法を指定してください。これは存在確認の設定であり、実際のリンク設定はMake・CMakeなどへ別途記述します。

```json
{
  "packages": [
    {
      "name": "libexample >= 1.0",
      "install": "Install libexample-dev for the selected target architecture.",
      "documentation": "https://example.org/libexample/install"
    }
  ]
}
```

ビルド済みNPMプラグインだけを使うアプリには、プラグイン用SDK・コンパイラ・NDKは不要です。Android APKの作成には、muon-uiが指定するJDKとAndroid SDKなどの[アプリ用環境](android.md)が別途必要です。

## ターゲットの選択

| 対象 | SDKのターゲット名 |
| --- | --- |
| CEF / Linux | `linux-amd64`, `linux-armhf`, `linux-arm64` |
| CEF / Windows | `windows-i686`, `windows-amd64` |
| Android | `android-arm64-v8a`, `android-x86_64` |

プラグインの`targets`には提供する対象を記述します。アプリのビルドで要求した対象が成果物になければエラーになります。同じ出力ディレクトリに対して対象ごとに`build`すると、共通の`muon-plugin.json`へ成果物を集められます。異なる名前や関数カタログへ変更した場合は、新しい出力先を使用してください。

C++では対象と同じcardioヘッダー・定義・共有ライブラリを使用します。SDKに同梱されたcardioはリンク用で、アプリではmuon側のランタイムを使います。異なるcardioやC++ランタイムをプラグインに同梱しないでください。SDKはABIとcardioのヘッダー識別子を照合しますが、任意のコンパイラ間や将来のSDKとのバイナリ互換性は保証しません。

Androidの下限はAPI 24で、C++ランタイムは`c++_shared`です。依存.soも16 KiBページへ対応させます。[AndroidのC++ランタイム](https://developer.android.com/ndk/guides/cpp-support)と[ページサイズへの対応](https://developer.android.com/guide/practices/page-sizes)を参照してください。

## アプリ内だけで使う

プラグインをNPMパッケージにする必要はありません。アプリの`muon.json`にソース設定への相対パスを記述します。CLIとViteは、ソースをビルドしてから関数カタログを読み込みます。生成物はアプリの`.muon/plugins/`へ置きます。

```json
{
  "plugin": {
    "mode": "validate",
    "plugins": [
      {
        "name": "counter",
        "native": { "source": "native/counter/muon-plugin.build.json" },
        "imports": [{ "sources": ["src/**"], "allow": ["muon.counter.*"] }]
      }
    ]
  }
}
```

```typescript
import { increment } from 'muon:counter';
const value = await increment(41);
```

`native.source`にはビルド設定、`native.manifest`には既成の`muon-plugin.json`、`native.package`にはNPMパッケージ名を指定します。この三つのうち一つを選びます。`name`は成果物の登録名と一致させます。登録名、NPMパッケージ名、ライブラリのファイル名は別のものです。

アプリからソースをビルドする場合は、`native.sysroot`と`native.androidSdkPath`も指定できます。ソースごとに複雑なクロスコンパイル条件が異なる場合は、作者側で`build`を実行し、アプリはその`native.manifest`を参照する方法も使えます。`signature`と`salt`は成果物からターゲットごとに設定するため、`native`を指定するエントリーには記述しません。

## Make、CMake、独自ビルド

Makeでは`muon-plugin.build.json`の`sources`をコンパイルします。`build.args`で`CPPFLAGS`や`LDLIBS`などを渡せます。SDKの最小ビルドは呼出しごとに再コンパイルするため、ヘッダーやフラグの変更も反映されます。独自のオブジェクト単位の増分ビルドが必要なら、MakefileやCMakeを自分で用意してください。標準Makeのパスには英数字と`_ . / + -`を使用できます。空白などを含むパスにはCMakeか独自ビルドを使用します。

CMakeの雛形は次のように作成します。`find_package(MuonPlugin CONFIG REQUIRED COMPONENTS Cardio)`と`Muon::Cardio`を使用します。C版では`find_package(MuonPlugin CONFIG REQUIRED)`と`Muon::Plugin`です。

```bash
npx muon-plugin init native/counter --name counter --build-system cmake
npx muon-plugin build native/counter/muon-plugin.build.json --target linux-amd64
```

既存プロジェクトへSDKだけを接続する場合は、環境を取得してからビルドできます。`env --shell`はPOSIXシェル用です。JSONを返す`env`も使用できます。生成されたMakefileを直接実行する場合も、この環境を設定してください。

```bash
eval "$(npx muon-plugin env --target linux-amd64 --shell)"
pkg-config --cflags --libs muon-plugin-cardio
make -C native/counter
cmake -S native/counter -B .build/counter -DMuonPlugin_DIR="$MUON_SDK/cmake" -DMUON_PLUGIN_TARGET=linux-amd64 -DMUON_PLUGIN_OUTPUT_DIRECTORY="$PWD/.build/plugin" -DMUON_PLUGIN_OUTPUT_NAME=counter
```

任意のビルドは`build.system: "custom"`と`commands`で指定します。各コマンドはシェルを介さずに実行されます。引数と依存ファイルのパスには、funcityの`{{source}}`、`{{build}}`、`{{output}}`、`{{sdk}}`、`{{target}}`を使用できます。`build.directory`はソース設定からの相対パスです。生成物は`{{build}}`へ置き、完成したプラグインを`{{output}}`へ出力します。追加ツールは`build.tools`で診断対象へ加えられます。

```json
{
  "build": {
    "system": "custom",
    "tools": ["ninja"],
    "commands": [
      { "command": "cmake", "args": ["-S", "{{source}}", "-B", "{{build}}", "-G", "Ninja", "-DMuonPlugin_DIR={{sdk}}/cmake", "-DMUON_PLUGIN_TARGET={{target}}"] },
      { "command": "cmake", "args": ["--build", "{{build}}"] }
    ]
  }
}
```

この例では、独自のCMakeLists.txtが環境変数`MUON_OUTPUT`を使って最終出力先を決定するものとします。`commands[].command`と`tools`の`./compile`などの相対パスは、`build.directory`を基準に診断・実行します。独自ビルドでもSDKの公開ヘッダーとコンパイル定義を使用し、最後に同じ成果物検査を通します。

## 外部ソースと通常の共有ライブラリ

vendorやsubmoduleのソースは、そのプロジェクトのビルド方式を使用できます。muon用の接続コードとビルド設定は自分の管理領域に置き、外部ソースやnode_modulesへ生成物を書き込みません。[独立サンプル](../../sdk/examples/README_ja.md)では、別ディレクトリのCライブラリをCMakeの`add_subdirectory`で取り込み、Cプラグインから呼び出します。

外部ソースやnode_modules内のビルド設定をCLIから直接指定する場合は、`--out`で自分の管理する出力先を指定してください。省略時は設定ファイルの隣の`.muon-plugin/`へ出力します。アプリの`native.source`では、アプリ側が出力先を指定します。

共有ライブラリを残す場合は、`dependencies`へ対象別のパスを記述します。通常の依存ライブラリに`muon_init_plugin`は不要です。Windowsのインポートライブラリなど、開発時だけ必要なファイルは`linkLibraries`へ分けます。

```json
{
  "dependencies": {
    "linux-amd64": ["{{build}}/libexample.so"],
    "windows-amd64": ["{{build}}/example.dll"],
    "android-arm64-v8a": ["{{build}}/libexample.so"]
  },
  "linkLibraries": { "windows-amd64": ["{{build}}/libexample.dll.a"] }
}
```

成果物の検査ではELFのDT_NEEDEDとPEのインポートを読み、通常の依存先がすべて揃っているか確認します。同名で内容の異なるライブラリや、予約されたランタイムの差し替えは拒否します。Linuxでは、隣に置いた依存を参照するライブラリへ`$ORIGIN`のRPATHまたはRUNPATHを指定します。SDKのMakeとCMakeの雛形には設定済みです。[動的リンカーの仕様](https://man7.org/linux/man-pages/man8/ld.so.8.html)と[WindowsのDLL検索](https://learn.microsoft.com/en-us/windows/win32/api/libloaderapi/nf-libloaderapi-loadlibraryexw)に基づく配置です。

## NPMパッケージとして提供する

ソースのビルド設定へ`headers`、`types`、`licenses`を記述すると、成果物の`include/`、`types/`、`licenses/`へ収録されます。各一覧のファイルはファイル名を維持してそのディレクトリ直下へコピーします。重複するファイル名を避け、公開ヘッダー内のincludeもこの配置に合わせてください。型宣言と関数カタログは作者が管理します。

```bash
npx muon-plugin build native/counter/muon-plugin.build.json --target linux-amd64 --out packages/counter
npx muon-plugin build native/counter/muon-plugin.build.json --target windows-amd64 --out packages/counter
npx muon-plugin verify packages/counter/muon-plugin.json --target linux-amd64
```

完成したディレクトリへ次のpackage.jsonを置き、`npm pack`で確認します。`native`以下にビルド済み.so/.dllと依存物を含めます。`postinstall`でのコンパイルは不要です。

```json
{
  "name": "my-muon-counter",
  "version": "1.0.0",
  "types": "./types/index.d.ts",
  "exports": {
    ".": { "types": "./types/index.d.ts" },
    "./muon-plugin.json": "./muon-plugin.json"
  },
  "files": ["muon-plugin.json", "native", "include", "types", "licenses"]
}
```

```bash
cd packages/counter
npm pack
```

自分のビルドが既に.so/.dllを生成している場合は、Node.js APIの`packagePlugin`または`muon-plugin package <設定JSON>`へ渡せます。設定は`PackagePluginOptions`型と同じで、`outputDirectory`、`name`、`functions`、`targets`と任意の公開ファイルを指定します。バイナリのstripや署名は包装前に終えてください。包装時にSHA-256を生成するため、その後の加工では検証に失敗します。

## NPMパッケージを使う

```bash
npm install my-muon-counter
```

先ほどのアプリ設定の`native`を`{ "package": "my-muon-counter" }`へ変更します。アプリはビルド対象の成果物を選び、関数のカタログからViteの仮想モジュールを生成します。ビルドPC上でWindows DLLなどを実行する必要はありません。実行時にも実際の登録結果とカタログを照合します。

型宣言は、作者が仮想モジュール用の宣言を提供するか、アプリで次のようにつなぎます。SDKの雛形は`counterPlugin`インターフェイスを生成します。

```typescript
// src/plugins.d.ts
// Keep this file free of top-level imports so this declares an ambient module.
declare module 'muon:counter' {
  export const increment: import('my-muon-counter').counterPlugin['increment'];
}
```

`native`で配布物を参照しても、JavaScriptのアクセス権限は増えません。`mode`、`imports`、`allow`、プラグイン固有の`config`はアプリが指定します。[プラグインの利用方法](muon-plugins.md)を参照してください。
