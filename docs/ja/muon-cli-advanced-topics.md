# muon CLI (Advanced topics)

Android向けの`prepare`、`build`、`pack --type apk`は[Androidアプリのビルド・配布](./android.md)を参照してください。以下はdesktop版の手順です。`--all`の対象もdesktopに限ります。

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

## AppImageの作成と実行

AppImageは、インストーラーを使わずに起動できるLinux向けの単一ファイルです。CPUごとに次のコマンドで作成します。

```bash
npx muon pack --type appimage --target linux-amd64
npx muon pack --type appimage --target linux-armhf
npx muon pack --type appimage --target linux-arm64
```

生成先は`artifacts/<packageName>-<version>-<target>.AppImage`です。`--type`を省略した場合も、Linuxの生成形式にAppImageを含めます。WindowsとAndroid向けには選択できません。

作成環境にはLinux、Node.js、`binutils`の`readelf`、`file`、通常のシェルとcoreutilsが必要です。Debian・Ubuntuでは`apt-get install binutils file`で不足分を導入できます。amd64のホストからARM向けにも生成できます。固定バージョンのappimagetoolと対象CPU用の起動プログラムは、初回のpackで取得し、SHA-256を検証します。以後は`$XDG_CACHE_HOME/muon/appimage`、未指定なら`~/.cache/muon/appimage`を再利用します。`MUON_APPIMAGE_TOOLS_DIR`で別の保存先を指定できます。[appimagetool](https://github.com/AppImage/appimagetool/tree/1.9.1)、[起動プログラム](https://github.com/AppImage/type2-runtime/tree/20251108)

実行するCPUに合うファイルを配布し、実行権限を付けて起動してください。

```bash
chmod +x ./my-app-1.0.0-linux-amd64.AppImage
./my-app-1.0.0-linux-amd64.AppImage
```

CEFは同梱しません。初回起動では既存のlauncherがCEFを取得し、`node.project`を使うアプリでは必要なNode.jsも取得します。CEF選択ポリシーはdebと共通で、`tested`以外もそのまま使用できます。準備済みランタイムや有効な取得キャッシュは再利用しますが、未取得のバージョンを選ぶ場合や必要なカタログがない場合には通信が必要です。初回からのオフライン起動を保証する形式ではありません。

取得アーカイブの共有キャッシュは`$XDG_CACHE_HOME/muon/`、未指定なら`~/.cache/muon/`に保存します。`MUON_CACHE_DIR`で別の保存先を指定できます。

AppImageの内容は読み取り専用です。CEF、Node.js、起動状態は`$XDG_STATE_HOME/<appId>/runtime/`へ準備し、未指定なら`~/.local/state/<appId>/runtime/`を使います。プロファイルはアプリの`browser.profilePath`設定に従います。AppImageを移動した後は新しい場所から一度直接起動すると、メニューと有効な自動起動の登録先を更新します。自動起動を新たに有効にすることはありません。[保存先と登録の削除](./package-install-location-advanced-topics.md)

通常の起動にはカーネルのFUSE機能と利用権限が必要です。muonが使用する起動プログラムはFUSEのユーザー空間ライブラリを静的に含むため、`libfuse2`を別途導入する必要はありません。FUSEを利用できない環境では展開実行を使ってください。[type2-runtimeの構成](https://github.com/AppImage/type2-runtime/blob/20251108/README.md)、[展開実行](https://docs.appimage.org/user-guide/troubleshooting/fuse.html)

```bash
./my-app-1.0.0-linux-amd64.AppImage --appimage-extract-and-run
```

メニューや自動起動には元のAppImageのパスを登録し、この引数は保存しません。FUSEを使えないセッションから登録経由で起動する場合は、セッションの環境変数`APPIMAGE_EXTRACT_AND_RUN=1`を設定してください。CEF sandboxのsetuid方式はdeb専用で、AppImageに`--linux-sandbox=setuid`を指定するとpack時にエラーになります。[sandboxの制約](./limitation.md#linuxにおけるcef-sandboxの有効化制限)

### Linuxの実行環境

配布バイナリの基準環境はUbuntu 20.04です。amd64ではUbuntu 20.04と24.04で検証し、armhf・arm64ではUbuntu 20.04の対象CPU用コンテナをQEMUで実行して確認しています。ARM実機のGPUや音声デバイスの検証を含むものではありません。

GTK・GLibなどの共有ライブラリと必要なモジュールを同梱します。glibc、C++ランタイム、NSS、ALSA、X11・Wayland、GPUドライバーとGBMなどはホスト側を使用します。実行環境にはこれらのデスクトップ用ライブラリ、CA証明書、`curl`が必要です。例えば最小構成のコンテナには、AppImageだけを置いても実行に必要なライブラリがそろいません。

最低要件は、muonをビルドした際のCEFと完成した全ネイティブ部品から算出します。現在の基準ビルドでは、glibc 2.29以上、`libstdc++6`のパッケージ版9以上、GBM 21.1以上などの要求があります。これらの数値だけを満たす任意のディストリビューションでの動作を保証するものではありません。詳細はNPM配布物の`dist/runtime/<target>/linux-dependencies.json.txt`と同じ場所の`linux-dependencies.json`で確認できます。同梱ファイルの取得元とライセンスは`linux-bundle.json`に記録します。

実行時に将来のCEFを選んだ場合も、ビルド時の参照CEFから計算した要件を使用します。そのCEF固有の追加要件は再判定しません。ネイティブプラグインがさらに新しいライブラリを必要とする場合は、対象環境に合わせてプラグインもビルドしてください。[CEFのLinux実行環境](https://chromiumembedded.github.io/cef/branches_and_building.html)
