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

Windowsバイナリのe2eテストを実行するには、 [agent-rover](https://github.com/kekyo/agent-rover/) のリモートエージェントを起動した Windows 11 (amd64) のマシンが必要です。
仮想マシンも使用できます。次のコマンドでテストを起動してください。

muon-uiのWindows E2Eでは接続先のPATHにNode.jsも必要です。Node.js 24で検証しています。インストール後はagent-roverを再起動し、新しいPATHを反映してください。

```bash
export AGENT_ROVER_WIN11_HOST=<agent-host-address>
export AGENT_ROVER_WIN11_TOKEN=<agent-token>

npm run test:windows-e2e --workspace muon-core-tester
```

あるいは、環境変数が定義されていれば、 `npm run test` で一括テストにWindows e2eテストが含まれます。

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
