# CEFバイナリ更新の詳細 (Advanced Topics)

この情報は、CEFおよびNode.jsランタイム準備処理の詳細な情報ですが、問題が発生した場合の分析のために示しています。
`muon-launcher.ini` は手動で構成することを想定していないため注意してください。

## CEFバージョンポリシー

`muon-launcher.ini` の `[cef]` セクションにある `versionPolicy` には以下の値が指定されます:

| 値                  | 動作                                                                                                                                             |
| :------------------ | :----------------------------------------------------------------------------------------------------------------------------------------------- |
| `tested`            | muon-coreのビルド時に検証された埋め込みCEF artifactを使用します。既定値です。                                                                     |
| `same-major-latest` | `cefReference.version` と同じCEF majorのstable/minimal候補から、CEF API hashが一致する最新artifactを使用します。見つからない場合は `tested` です。 |
| `compat-latest`     | stable/minimal候補全体から、CEF API hashが一致する最新artifactを使用します。見つからない場合は `tested` です。                                    |
| `exact`             | `exactVersion` に指定したCEF versionを使用します。`tested` と異なるversionではCEF API hash一致が必須です。                                         |

これらのポリシーと `exactVersion` はCEF専用であり、Node.jsバージョンの選択には影響しません。

`muon.json` を `muon embed-config` で実行ファイルに埋め込む場合、`launcher.defaultVersionPolicy` を `muon-launcher` 起動時にも有効にするには、`muon-core` だけでなく最終的に起動する `muon-launcher` 実行ファイルも指定して下さい:

```bash
muon embed-config \
  --runtime-path ./dist/runtime/linux-amd64 \
  --launcher-path ./myapp \
  --config ./muon.json
```

## カタログ、バージョン選択、キャッシュ

CEFカタログは `cef-catalog.json`、Node.jsの公式リリースインデックスは `node-catalog.json` としてmuonキャッシュの直下に保存されます。
`[runtime]` の `catalogRefreshIntervalSeconds` は、各起動で適用対象となるカタログの自動更新間隔です。
既定値は7日間 (`604800`) で、`0` を指定すると間隔による自動更新を行いません。
更新に失敗しても既存のカタログが利用可能なら、それをフォールバックとして使用します。

CEFでは、ポリシーに従って公式CEFカタログからartifactを選択します。
Node.jsでは、公式の `https://nodejs.org/dist/index.json` にあるリリースのうち、対象プラットフォーム向けファイルを提供し、かつプロジェクトの `package.json` にある `engines.node` とmuon Nodeブリッジの互換範囲の両方を満たすものを選択します。
`engines.node` が省略されている場合は最新の互換LTSを使用し、一致するLTSがなければ失敗して非LTSへはフォールバックしません。
指定されている場合は一致するLTSの最大バージョンを優先し、一致するLTSがなければ一致する全リリースの最大バージョンを使用します。

キャッシュ内の配置は次の通りです:

```text
<cache>/
├── cef-catalog.json
├── node-catalog.json
├── node-checksums
│   └── <version>
│       └── SHASUMS256.txt
└── artifacts
    ├── cef
    │   └── <CEF archive>
    └── node
        └── <version>
            └── <Node.js archive>
```

CEFアーカイブは外部CEFカタログが提供するSHA1とサイズで検証します。
Node.jsアーカイブはリリースごとの公式 `SHASUMS256.txt` から対象ファイル名に対応する値を一つだけ選び、SHA-256で検証します。
プラグイン、muonが生成するアセットアーカイブ、launcher準備状態の整合性検証にもSHA-256を使用します。

CEFとNode.jsの両方が必要な場合、それぞれに適用されるカタログとアーカイブの準備は並行して行われます。
並行処理から進行表示コールバックを呼び出す箇所は直列化されます。
直列化を利用できない場合は並行処理を行わず、順番に準備します。

テストやミラー運用では、`MUON_CEF_CATALOG_URL` 環境変数でカタログファイルのURLを上書き出来ます。artifactのURLはカタログURLと同じディレクトリを基準に解決されます。
Node.jsでは `MUON_NODE_DIST_URL` で既定の `https://nodejs.org/dist` を上書き出来ます。`index.json`、各バージョンの `SHASUMS256.txt`、アーカイブはこのURLを基準に解決されます。

## ランチャー状態と更新要求

`muon-launcher.ini` は次の4セクションに分かれています:

```ini
[runtime]
catalogRefreshIntervalSeconds=604800

[cef]
versionPolicy=tested
lastCatalogUpdateUnix=0

[node]
lastCatalogUpdateUnix=0

[update]
requested=false
requestedAtUnix=0
```

`[cef]` と `[node]` の `lastCatalogUpdateUnix` は、それぞれのカタログを実際に置き換えられた時だけ更新されます。
launcherプラグインの `triggerUpdate()` を呼ぶと、共有の `[update]` に `requested=true` が保存され、次回の `muon-launcher` 起動時に適用対象のカタログ更新を試行します。
Node.jsランタイムを必要としないアプリではNode.jsカタログは適用対象になりません。
更新要求は、適用対象となるすべてのカタログをその起動で更新できた場合だけ `requested=false` に戻ります。
既存カタログへのフォールバックは成功した更新とは見なされないため、その場合は要求が残ります。

## runtimeの公開トランザクション

実行時の準備では配布元distとCEFをruntimeへ配置し、Node.jsが必要なら公式アーカイブから `runtimes/node/LICENSE` と `runtimes/node/bin/node`（Windowsでは `node.exe`）だけを配置します。
npm、Corepack、その他のNode.jsアーカイブ内ファイルは配置しません。
`muon-core` のビルド時には同じCEF preparerを使って `muon-core/.cef/` にビルド用のCEFツリーを展開します。

準備済みruntimeには `.muon-runtime-ready.json` があり、配布元muon、CEF、Node.jsアーカイブに対応するSHA-256 fingerprintを保持します。
Node.jsが不要な場合のNode.js fingerprintなど、適用されない構成要素には64桁の`0`からなるsentinelを使用します。ポータブル配布物のin-place準備では、配布元muonのfingerprintにも同じsentinelを使用します。
fingerprintが一致するruntimeは再利用されます。

通常のステージングでruntime内容を更新する場合は、muonファイル、CEF、Node.js、`muon-launcher.ini`、ready markerを一時ディレクトリへすべて書き込み、その候補が完成してからruntimeディレクトリとして公開します。fingerprintが一致する場合は既存runtimeを再利用し、launcher状態だけをatomicに置き換えます。
ポータブル配布物のin-place準備では、変更前に既存ready markerを無効化し、CEFとNode.jsの配置およびlauncher状態の保存がすべて成功してから新しいmarkerを公開します。
したがって、準備途中のruntimeや状態だけが先行したruntimeはreadyとして扱われません。
