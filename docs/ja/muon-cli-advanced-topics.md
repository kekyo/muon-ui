# muon CLI (Advanced topics)

muonは基本的にViteプラグインと共に使用することを想定していますが、Viteプラグインを使用しないで開発することも出来ます。
その場合は、 `muon` CLIコマンドを使用します。

## muon runで直接起動

Viteの開発サーバーを使わず、ローカルに生成済みのアセットディレクトリをそのままmuonで開きたい場合は、`muon run` を使用出来ます:

```bash
npx muon run
```

- `muon run` はViteの開発サーバーを起動しません。そのため、`muon run`ではHMRは動作しません。
- Viteの `server.proxy` に1件以上の定義がある場合、proxyはVite開発サーバー経由でのみ使用でき、起動したアプリでは機能しないため、`muon run` はstderrへ警告を出力します。
- `--assets` を明示した場合は、従来どおり指定されたローカルアセットを直接起動します。
- `--assets` を省略し、`vite.config.*` にmuon Viteプラグインが1つだけ含まれている場合、`muon run` は起動前に `vite build` を実行します。
  Viteの `build.outDir` と `base` を読み取り、ビルド出力を `.muon/run/assets/main/` 配下に配置してから、その `.muon/run/assets` を `asset.sourcePath` としてmuonを起動します。
  例えば `base: "/foo/"` の場合、出力は `.muon/run/assets/main/foo/` に配置され、`muon.json` に `browser.startPage` が無ければ `asset://main/foo/index.html` を開きます。
- Viteやmuon Viteプラグインが無い場合は、`--assets` オプション、 `muon.json` の `asset.sourcePath`、 `assets/` の順に解決されます。
  `asset://main/index.html` の場合は、 `assets/main/index.html` を参照することに注意して下さい。URLのホスト名部分がサブディレクトリとして扱われます。
- `vite.config.*` にmuon Viteプラグインが1つだけ含まれている場合、`muon run` は `muonPath`, `cefPath`, `stagePath`, `enableDebugger`, `allowInsecureLocalhost` と `build.configPath` を読み取ります。
- CLIオプションで同じ項目を指定した場合はCLI側が優先され、`open` は `muon run` では無視されます。
  `build: false` は `--assets` を省略したVite-backed起動ではエラーになりますが、`--assets` を明示した場合は従来どおり指定アセットを起動します。
- `allowInsecureLocalhost` の優先順位は、`--allow-insecure-localhost`、明示したViteプラグインオプション、`muon.json` の `network.localAccess.allowInsecureLocalhost`、`false` の順です。
  CLIフラグは設定を有効にするだけで、負方向のCLI形式はありません。
- muon DevTools、リサイクルキーバインド、CDPの開発用既定値を無効化するには `--no-debugger` を指定します。
- localhostの開発用HTTPS証明書エラーを無視するには `--allow-insecure-localhost` を指定します。
  このrawフラグを配布用のmuon-launcherへ直接渡した場合は、引き続き起動前に拒否されます。
  配布用ビルドでこの動作が必要な場合は、`muon.json` の `network.localAccess.allowInsecureLocalhost` を設定してください。
- `muon run` は起動前にmuon-builderを実行し、CEFと、`node.project` を指定した場合に必要な公式Node.js runtimeを準備します。Node.js runtimeの準備は `simple` と `validate` のどちらのモードでも行われます。
  両方のdownloadが必要な場合は並行して実行され、Node.jsはstage directoryの `runtimes/node/bin/node`（Windowsでは `runtimes/node/bin/node.exe`）へ配置されます。
  Node.js実行ファイルを指定するCLIオプションはなく、Node pluginはこの絶対pathだけを使用します。環境変数や `PATH` へのfallbackはありません。

Viteプラグインを使用する場合と異なり、アセットホスト名部分によるページ管理の分割を自然に行うことが出来ます。
例えば、 `asset://main/index.html` と `asset://sub/index.html` は、CEFが異なるオリジンとして扱います。
高度なセキュリティ分離を行いたい場合や、muonのホワイトリストによるフィルタの分離にも応用できます。
詳しくはローカルアセットの権限の章を参照して下さい。

> 注釈: Viteプラグインを使用した場合でもアセットホスト名分離は機能しますが、
> 既定でViteが出力したコンテンツファイル群はすべて `main/` 配下に配置されるため、
> 異なるアセットホスト名を使用する場合は、Viteのビルドプロセスまたは手動で適切な構成を行う必要があります。

## 配布用ビルド

配布用ビルド生成は、 `muon` CLIからも実行出来ます。

```bash
npx muon build
```

`vite.config.*` にmuon Viteプラグインが含まれている場合、 `muon build` は `muon({ ... })` の引数を読み取り、 `vite build` と同じビルド設定を使用します。
但し、CLIオプションを指定した場合は、そのパラメータはオーバーライドされます。

muon Viteプラグインが構成されていない場合、 `muon build` はコンテンツビルド用のnpm scriptや `vite build` を自動実行せず、既に存在するアセットを配布用ディレクトリにまとめます。
この場合のアセット元は、`--assets`、 `muon.json` の `asset.sourcePath`、 `assets/` の順に解決されます（`muon run` 同様）。

`asset.sourcePath` は設定ファイルが置かれているディレクトリからの相対パス、または絶対パスとして扱われます。
アセット元がディレクトリの場合は `assets.zip` にパッキングし、ZIPファイルの場合は配布先の `assets.zip` としてそのままコピーして署名します。
また、`package.json` の `files` に `README.md` や `LICENSE` などの通常ファイルを指定すると、条件を満たすものは配布先ディレクトリ直下へコピーされます。
muon Viteプラグインの `build.distributionFiles` が指定されている場合は、`package.json` の `files` ではなくそのリストを使用します。

`node.project` を設定したアプリでは、`muon build` と `muon pack` が対象platformの `node.so` または `node.dll`、`node-bridge.mjs`、Nodeプロジェクトを成果物へコピーし、正規化済みNode runtime requirementをlauncherへ埋め込みます。
Node.js実行ファイル、配布archive、download cacheはbuildまたはpackの成果物に含めません。
配布用 `muon-launcher` は起動時にCEFと必要なNode.js runtimeを準備します。通常のinstallではアプリケーションルートとは別のruntime directoryへ配置し、portable配布物では展開先へ直接配置するため、準備後のportable runtimeにも必要なNode.jsが含まれます。
実行中のNode.js runtimeは置き換えず、更新は次回のlauncher起動時に適用します。sidecarは準備済みの `runtimes/node/bin/node`（Windowsでは `.exe`）だけを起動し、環境変数や `PATH` へfallbackしません。

ターゲットを指定する場合は `--target linux-amd64` のように指定し、すべての同梱ターゲットを生成する場合は `--all` を使用します。
muon Viteプラグインが無い場合、 `muon build` の未指定ターゲットは実行中ホストのターゲットです。

ビルド時に生成されるmuonアプリバイナリに指定する名称やアイコンなどのオプション指定例を示します:

```bash
npx muon build --icon icons/app.png --windows-version 1.2.3
npx muon build --icon icons/app.png --linux-name "My App"
```

- `--icon` はmuonアプリの静的アイコン指定です。Windows PE/NSIS、Linux desktop、起動時タイトルバーアイコンの共通ソースになります。
- Windowsターゲットでは、`--windows-icon`, `--windows-product-name`, `--windows-file-description`, `--windows-company-name`, `--windows-version`, `--windows-copyright` でlauncherとNSIS installer用のWindows resource metadataを上書き出来ます。
  `--windows-icon` はWindowsターゲット専用のアイコンoverrideです。
  同じ値は `muon.json` の `windows.resource` でも指定出来ます。
- Windowsコードサイニングを行う場合は、`--windows-sign-command`, 繰り返し指定可能な `--windows-sign-arg`, `--windows-sign-target` を指定します。
  `--windows-sign-arg` には署名対象ファイルに置換される `{path}` が必要で、`{target}` と `{kind}` も使用出来ます。
  `--no-windows-code-signing` を指定すると、`muon.json` の `windows.codeSigning` を無効化します。
- Linuxターゲットでは、`--linux-desktop-id`, `--linux-name`, `--linux-comment`, `--linux-icon`, `--linux-categories`, `--linux-startup-notify` でdesktop entry metadataを上書き出来ます。
  `--linux-icon` はLinuxターゲット専用のアイコンoverrideです。
  同じ値は `muon.json` の `linux.desktop` でも指定出来ます。

> 注釈: Viteプラグインを使わず、生成済みアセットをmuon CLIで直接収録する場合、CLI自身はvirtual moduleの解決（`import`によるmuonプラグインの参照）を行いません。
> `validate` モードの `muon:node` を使用するコードはViteで事前にbundleしてください。Viteを介さないアセットからNode.js機能を参照する場合は `simple` モードの `window.muon.node.createNode()`を使用します。
