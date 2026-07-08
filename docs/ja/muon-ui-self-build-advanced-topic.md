## muon-uiセルフビルド (Advanced topic)

必要なパッケージのインストール:

```bash
apt-get update
apt-get install -y \
  build-essential ca-certificates cmake curl dbus file g++-mingw-w64 \
  git libasound2-dev libdrm-dev libgbm-dev libgtk-3-dev \
  libnss3-dev libxss-dev ninja-build wine xvfb
apt-get install -y \
  nodejs npm
```

- Node.jsのインストールは [nvm](https://github.com/nvm-sh/nvm) 経由の方が良いかも知れません。バージョンは20以降です。

### ビルドとテスト

```bash
npm install
npm run build
npm run test
```

muonをデバッグページで起動するには:

```bash
npm run dev
```

### Windowsバイナリのe2eテスト

Windowsバイナリのe2eテストを実行するには、 [agent-rover](https://github.com/kekyo/agent-rover/) のリモートエージェントを起動した Windows 11 (amd64) のマシンが必要です。
これは、仮想マシン上のインスタンスでも構いません。その上で、以下のようにテストを起動します:

```bash
export AGENT_ROVER_WIN11_HOST=<agent-host-address>
export AGENT_ROVER_WIN11_TOKEN=<agent-token>

npm run test:windows-e2e --workspace muon-core-tester
```

あるいは、環境変数が定義されていれば、 `npm run test` で一括テストにWindows e2eテストが含まれます。

### パッケージ生成

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

その後、以下のコマンドで、すべてのプラットフォーム向けバイナリをビルドし、NPMパッケージを生成します:

```bash
npm run pack
```

このパッケージスクリプトは `./build_package.sh` に委譲します。パッケージ生成オプションを直接渡したい場合は、`./build_package.sh` を直接実行することも出来ます。

- サポートされているすべてのアーキテクチャ向けにネイティブコードをビルドおよびテストするため、非常に長い時間がかかります（30分以上かかる可能性があります）。

---

