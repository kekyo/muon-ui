# CEFのダウンロードと更新

CEFのバイナリアセットファイルは非常にサイズが大きことで有名です。
また、CEFに脆弱性が発見された場合はCEFバイナリが更新されることになり、muonアプリをそのまま配布しているとCEFバイナリの更新のためにmuonアプリ全体の更新に見舞われます。

そこでmuonは、muonアプリの配布物のサイズ削減と、CEFバイナリの更新の簡略化のため、
muonアプリ起動時に、必要なCEFバイナリをダウンロードして動作完了を整えます。

`npm run dev` する時、 `muon-core` をビルドする時、あるいはビルド後の生成物を配布して、エンドユーザーがmuonアプリを起動する場合に、必要なCEFのバイナリがローカルに存在しない場合は、CEFの公式配布サイトから自動的にダウンロードされます。
これには少し時間がかかりますが、ダウンロードされたCEF tarballはローカルにキャッシュされるので、次回以降はキャッシュを使用します。

- ローカルのキャッシュは、Linuxでは `~/.cache/muon/` ディレクトリ内に、Windowsでは `$HOME\.cache\muon\` ディレクトリ内に配置されます。
- `MUON_CACHE_DIR` 環境変数を指定すると、キャッシュディレクトリを上書き出来ます。

キャッシュディレクトリには、カタログとダウンロード済みのCEF tarballだけが配置されます:

```text
~/.cache/muon/
├── catalog.json
└── artifacts
    └── cef_binary_<version>_<target>_minimal.tar.bz2
```

`muon build` の出力や通常インストーラー用のruntimeでは、配布された `dist-muon/<target>` ディレクトリは読み取り専用の元データとして扱われます。
エンドユーザーがアプリケーションを起動すると、`muon-bootstrap` は実行前にdist全体をユーザーステートディレクトリ配下の `runtime/` へステージングし、
そこへCEFバイナリを展開してから `muon-core` を起動します。
CEFプロファイルは同じアプリケーションステートルートの `profile/` に配置されます:

- Linux: `$XDG_STATE_HOME/<appId>/runtime/` と `$XDG_STATE_HOME/<appId>/profile/`、または `$HOME/.local/state/<appId>/runtime/` と `$HOME/.local/state/<appId>/profile/`
- Windows: `%LOCALAPPDATA%\<appId>.<arch>\runtime\` と `%LOCALAPPDATA%\<appId>.<arch>\profile\`。`<arch>` は `amd64` または `i686` です。

起動時の準備では、ユーザーステートディレクトリの `muon-bootstrap.ini` に従ってCEFバージョンとカタログ更新を判断します。
これらについての詳細は、別章を参照して下さい。

`muon pack --type zip` と `muon pack --type tgz` / `tar.gz` のポータブル配布物は例外です。
展開した `<packageName>/<target>` 直下にあるアプリランチャーを起動すると、`muon-bootstrap` は同じディレクトリ直下へCEFをin-placeで準備し、`muon-core` もそのディレクトリをカレントディレクトリとして起動します。
ポータブル配布物ではビルド時に `browser.profilePath` が `profile` に固定されるため、CEFプロファイルは展開先直下の `profile/` に作成されます。
runtimeとprofileのためにユーザーステートディレクトリは使用しません。
ただし、ダウンロード済みCEF tarballのキャッシュは従来通り `MUON_CACHE_DIR` または既定の `~/.cache/muon/` 系ディレクトリに保存されます。

---

