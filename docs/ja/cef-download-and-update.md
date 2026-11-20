# CEFのダウンロードと更新

CEFのバイナリアセットファイルは非常にサイズが大きいことで有名です。
また、CEFに脆弱性が発見された場合はCEFバイナリが更新されることになり、muonアプリをそのまま配布しているとCEFバイナリの更新のためにmuonアプリ全体の更新に見舞われます。

そこでmuonは、muonアプリの配布物のサイズ削減と、CEFバイナリの更新の簡略化のため、
muonアプリ起動時に、必要なCEFバイナリをダウンロードして動作完了を整えます。
デスクトップアプリに[Node.jsサービス](services.md)を定義すると、各サービスの`package.json`にある`engines.node`を満たすNode.jsランタイムも準備します。
`muon build`と`muon pack`は、サービスのコードと実行時の依存パッケージ、必要なNode.jsのバージョン条件を収録します。実行ファイルはランチャーが準備します。

`npm run dev` する時、 `muon-core` をビルドする時、あるいはビルド後の生成物を配布して、エンドユーザーがmuonアプリを起動する場合に、必要なCEFのバイナリがローカルに存在しない場合は、CEFの公式配布サイトから自動的にダウンロードされます。
これには少し時間がかかりますが、ダウンロードされたCEF tarballはローカルにキャッシュされるので、次回以降はキャッシュを使用します。
Node.jsランタイムが必要な場合も、開発時または配布後のランチャー起動時にNode.jsの公式配布サイトから取得され、同じmuonキャッシュを再利用します。
CEFとNode.jsの両方が必要な場合は並行して準備されます。進行表示への通知は直列化されるため、同時ダウンロード中も一つの進行表示を安全に更新できます。

- ローカルのキャッシュは、Linuxでは `~/.cache/muon/` ディレクトリ内に、Windowsでは `$HOME\.cache\muon\` ディレクトリ内に配置されます。
- `MUON_CACHE_DIR` 環境変数を指定すると、キャッシュディレクトリを上書き出来ます。

キャッシュディレクトリには、カタログ、検証情報、ダウンロード済みアーカイブが配置されます:

```text
~/.cache/muon/
├── cef-catalog.json
├── node-catalog.json
├── node-checksums
│   └── <version>
│       └── SHASUMS256.txt
└── artifacts
    ├── cef
    │   └── cef_binary_<version>_<target>_minimal.tar.bz2
    └── node
        └── <version>
            └── node-<version>-<target>.<archive-extension>
```

Node.jsのリリース情報には公式の `https://nodejs.org/dist/index.json` を使用し、選択したアーカイブは同じリリースの `SHASUMS256.txt` に記載されたSHA-256で検証します。
Node.jsサービスを使う場合は、対象プラットフォーム向けの公式リリースから、各サービスの`engines.node`とmuonの対応範囲をすべて満たすものを選びます。
どのサービスも`engines.node`を指定していなければ、条件に合う最新のLTSを使います。該当するLTSがなければ準備は失敗します。
いずれかのサービスが`engines.node`を指定している場合も、条件に合う最新のLTSを優先します。該当するLTSがなければ、条件を満たす全リリースから最新のものを選びます。

`muon build` の出力や通常インストーラー用のruntimeでは、配布された `dist-muon/<target>` ディレクトリは読み取り専用の元データとして扱われます。
エンドユーザーがアプリケーションを起動すると、`muon-launcher` は実行前にdist全体をユーザーステートディレクトリ配下の `runtime/` へステージングし、
そこへCEFバイナリと、必要な場合はNode.jsランタイムを展開してから `muon-core` を起動します。
Node.jsランタイムはアプリケーションルートと区別された `runtimes/node/` に配置され、内容は `LICENSE` と `bin/node`（Windowsでは `bin/node.exe`）だけです。
公式アーカイブに含まれるnpmやCorepackなどのファイルはruntimeへ配置されません。
CEFプロファイルは同じアプリケーションステートルートの `profile/` に配置されます:

- Linux: `$XDG_STATE_HOME/<appId>/runtime/` と `$XDG_STATE_HOME/<appId>/profile/`、または `$HOME/.local/state/<appId>/runtime/` と `$HOME/.local/state/<appId>/profile/`
- Windows: `%LOCALAPPDATA%\<appId>.<arch>\runtime\` と `%LOCALAPPDATA%\<appId>.<arch>\profile\`。`<arch>` は `amd64` または `i686` です。

起動時の準備では、ユーザーステートディレクトリの `muon-launcher.ini` に従ってCEFバージョンとカタログ更新を判断します。
このファイルのCEFバージョンポリシーはCEF専用であり、Node.jsバージョンは埋め込まれたランタイム要件から決定されます。
これらについての詳細は、別章を参照して下さい。

`muon pack --type zip` と `muon pack --type tgz` / `tar.gz` のポータブル配布物は例外です。
展開した `<packageName>/<target>` 直下にあるアプリランチャーを起動すると、`muon-launcher` は同じディレクトリ直下へCEFと必要なNode.jsランタイムをin-placeで準備し、`muon-core` もそのディレクトリをカレントディレクトリとして起動します。
ポータブル配布物ではビルド時に `browser.profilePath` が `profile` に固定されるため、CEFプロファイルは展開先直下の `profile/` に作成されます。
runtimeとprofileのためにユーザーステートディレクトリは使用しません。
ただし、ダウンロード済みCEFおよびNode.jsアーカイブのキャッシュは従来通り `MUON_CACHE_DIR` または既定の `~/.cache/muon/` 系ディレクトリに保存されます。

通常のLinux debパッケージも、上記の通常インストーラーと同じユーザーステートディレクトリのruntimeを使用します。
`muon pack --type deb --linux-sandbox=setuid`で生成したdebパッケージだけは例外で、root所有のprivileged helperがsystem runtimeへCEFと必要なNode.jsランタイムを準備します。
このsystem runtimeの配置には管理者権限が必要ですが、その代わりLinuxのCEFで必要となる`cef-sandbox`が完全に有効化されます。
