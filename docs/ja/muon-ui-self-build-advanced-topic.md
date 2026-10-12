# muon-uiセルフビルド (Advanced topic)

必要なパッケージをインストールします。

```bash
apt-get update
apt-get install -y \
  build-essential ca-certificates cmake curl dbus file g++-mingw-w64 \
  git libasound2-dev libdrm-dev libgbm-dev libgtk-3-dev \
  libnss3-dev libxss-dev ninja-build wine xvfb
apt-get install -y \
  nodejs npm
```

- Node.jsは24で検証しています。[nvm](https://github.com/nvm-sh/nvm)からも導入できます。

Android部品を含むMuon本体のビルドと全体テストには、JDK 17～25とAndroid SDKも必要です。JDK 25.0.3で検証しています。[Androidアプリの準備](./android.md#必要な環境)を行ったうえで、本体のネイティブ部品を作るためのNDKとCMakeを追加してください。通常のMuonアプリ利用者には、この追加は不要です。

```bash
android --sdk="$ANDROID_HOME" sdk install ndk/29.0.14206865 cmake/4.1.2
```

## ソースの配置

実行時の機能は`core/`、CLI・Vite連携・アプリのビルドと配布機能は`ui/`に置いています。それぞれの`common/`は共通処理、`cef/`と`android/`はバックエンド固有の処理です。テストは実装と同じ階層の`common-test/`、`cef-test/`、`android-test/`に置きます。

`core/android-poc/`にはQuickJSの試作と固有テストがあります。周辺機能は、ネイティブビルド支援を`builder/`、Node.jsホストを`node/`に分けています。`ui/`の公開npmパッケージ名は`muon-ui`、`node/`は`muon-node`です。

## ビルドとテスト

```bash
npm install
npm run build
npm run test
```

muonをデバッグページで起動します。

```bash
npm run dev
```

## Androidの端末テスト

全体テストはAPKのビルドと静的検査までを行います。端末上の検証はADB接続後に次のコマンドで実行してください。エミュレーターはAPI 37・x86_64・16 KiBページ、Pixel 6はAPI 37・arm64-v8a・4 KiBページを検証対象としています。

```bash
ANDROID_SERIAL=emulator-5556 npm run test:android --workspace muon-android-tester
ANDROID_SERIAL=your-pixel6-serial npm run test:android:pixel6 --workspace muon-android-tester
```

QuickJSの試作は別のworkspaceで検証します。

```bash
ANDROID_SERIAL=emulator-5556 npm run test:android --workspace muon-android-poc
ANDROID_SERIAL=your-pixel6-serial npm run test:android:pixel6 --workspace muon-android-poc
```

npm配布物だけを使う独立アプリの検証対象は、公開CLI/Vite、署名、保存、再起動、更新後のデータ保持です。端末上の専用テストアプリ`dev.muon.e2e.publicconsumer`は検証のために作り直します。

```bash
npm run build --workspace muon-ui
mkdir -p .run/android-e2e
npm pack --workspace muon-ui --pack-destination .run/android-e2e
ANDROID_SERIAL=your-device-serial node ui/android-test/test-packaged-application.mjs .run/android-e2e/muon-ui-0.0.1.tgz
```

tgz名は実際のバージョンに合わせてください。`--plugins`を付ける場合は、先に`npm test --workspace muon-android-tester`でテストプラグインを生成してください。成果物は`core/android-test/.build/plugins/<abi>/`へ配置されます。`--validate`を併用するとvalidateモードの権限検査も実行します。CIでも同じ独立アプリを16 KiBエミュレーターで検証します。

## Windowsバイナリのe2eテスト

Windowsバイナリのe2eテストには、[agent-rover](https://github.com/kekyo/agent-rover/)のリモートエージェントを起動したWindows 11（amd64）のマシンが必要です。仮想マシンも使用できます。接続先のPATHにはNode.jsも必要です。Node.js 24で検証しています。インストール後はagent-roverを再起動し、新しいPATHを反映してください。

接続にはagent-rover 0.10.0の標準設定を使います。[公式READMEの接続設定](https://github.com/kekyo/agent-rover/blob/0.10.0/README_ja.md#接続設定ファイル)に従い、テストを起動するユーザーのホームディレクトリにある`~/.agent-rover`へ接続先を登録してください。既に登録済みなら、ホストやトークンを環境変数へ転記する必要はありません。

設定ファイルの例です。

```ini
[win11]
test-agent.example.com=<access-token>
```

次のコマンドでWindows i686・amd64の画面テストと、インストーラーの実機テストを実行できます。

```bash
npm run test:windows-e2e --workspace muon-core-tester
npm run test:windows-nsis-e2e --workspace muon-ui
```

既定では`win11`セクションを使います。別のセクションを選ぶ場合は、コマンドに`MUON_E2E_AGENT_SECTION=vm`などを指定してください。接続先の選択と使用中の再試行はSDKが行います。SDKは実行時の作業ディレクトリにある`.agent-rover`を優先するため、npm workspace内に同名ファイルを置く場合は設定の優先順位に注意してください。

実機テストは、`MUON_E2E_REMOTE_WINDOWS=1`を設定する上記の専用コマンドで有効になります。通常の`npm test`ではスキップする仕様です。各スイートは終了時に接続を解放します。

実機上の作業ディレクトリは、`MUON_E2E_REMOTE_WORK_DIR`で変更可能です。画面テストはCDPとHTTP用のIPv4通信経路を自動で確認します。HTTPサーバーへの戻り先を別途指定する必要があるネットワークでは、`MUON_E2E_REMOTE_HTTP_HOST`を使用できます。

## パッケージ生成

```bash
# Prerequisities
sudo apt-get install -y podman
sudo podman run --rm --privileged docker.io/multiarch/qemu-user-static --reset -p yes

# Verify QEMU is working:
podman run --rm --platform linux/arm64 docker.io/library/debian:trixie-slim uname -m
# Should output: aarch64
```

パッケージ生成前に、ビルド用のコンテナイメージを準備します。この手順でネイティブビルドと
プラットフォーム検証に必要な依存関係をターゲット別のPodmanイメージに導入するため、パッケージ生成のたびに
各コンテナ内でaptパッケージをインストールする時間を削減出来ます。

```bash
# Build prerequisite images
./prereq.sh
```

その後、次のコマンドで、すべてのプラットフォーム向けバイナリをビルドし、NPMパッケージを生成します。

```bash
npm run pack
```

パッケージ生成の入口は`build_package.sh`です。生成オプションを直接渡したい場合は、`build_package.sh`を実行してください。

- サポートされているすべてのアーキテクチャ向けにネイティブコードをビルドおよびテストするため、非常に長い時間がかかります（30分以上かかる可能性があります）。
