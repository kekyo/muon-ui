# CEFバイナリ更新の詳細 (Advanced Topics)

この情報は、CEFバイナリアップデート処理の詳細な情報ですが、
問題が発生した場合の分析のために示しています。
`muon-bootstrap.ini` は手動で構成することを想定していないため注意してください。

`muon-bootstrap.ini` の `versionPolicy` には以下の値が指定されます:

| 値                  | 動作                                                                                                                                             |
| :------------------ | :----------------------------------------------------------------------------------------------------------------------------------------------- |
| `tested`            | muon-coreのビルド時に検証された埋め込みCEF artifactを使用します。既定値です。                                                                     |
| `same-major-latest` | `cefReference.version` と同じCEF majorのstable/minimal候補から、CEF API hashが一致する最新artifactを使用します。見つからない場合は `tested` です。 |
| `compat-latest`     | stable/minimal候補全体から、CEF API hashが一致する最新artifactを使用します。見つからない場合は `tested` です。                                    |
| `exact`             | `exactVersion` に指定したCEF versionを使用します。`tested` と異なるversionではCEF API hash一致が必須です。                                         |

`catalogRefreshIntervalSeconds` はカタログ自動更新間隔です。既定値は7日間 (`604800`) で、`0` を指定すると自動更新を行いません。
bootstrapプラグインの `triggerUpdate()` を呼ぶと `requested=true` が保存され、次回 `muon-bootstrap` 起動時にカタログ更新を試行します。更新に成功した場合だけ `requested=false` に戻ります。

`muon.json` を `muon embed-config` で実行ファイルに埋め込む場合、`bootstrap.defaultVersionPolicy` を `muon-bootstrap` 起動時にも有効にするには、`muon-core` だけでなく最終的に起動する `muon-bootstrap` 実行ファイルも指定して下さい:

```bash
muon embed-config \
  --runtime-path ./dist/runtime/linux-amd64 \
  --bootstrap-path ./myapp \
  --config ./muon.json
```

このプロセスは大きく3段階あります:

1. policyや更新要求に応じてCEFカタログをダウンロードし、`catalog.json` に配置します。既存のカタログがある場合、更新に失敗しても既存の内容を使用します。
2. 必要なCEF tarballを `artifacts/` にダウンロードします。既に存在する場合は、外部CEFカタログが提供するSHA1とサイズを確認して使用します。
3. 実行時の準備では配布元distをユーザーstate配下へコピーしてからCEFを同じruntimeディレクトリへ展開し、`muon-core` のビルド時には同じpreparerを使って `muon-core/.cef/` にビルド用のCEFツリーを展開します。

ここで使用するSHA1は、外部CEFカタログのartifact検証値に合わせるためのものです。
プラグイン、muonが生成するアセットアーカイブ、bootstrap準備状態の整合性検証にはSHA-256を使用します。

CEFのバイナリは公式のカタログファイルをダウンロードして、必要なバージョンを確認します。
テストやミラー運用では、`MUON_CEF_CATALOG_URL` 環境変数でカタログファイルのURLを上書き出来ます。artifactのURLはカタログURLと同じディレクトリを基準に解決されます。
