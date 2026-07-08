# CEFバージョンとCEF APIバージョン (Advanced topics)

CEFには、ネイティブAPIのバージョニングが存在します。通常、このバージョニングは「バージョンウインドウ」が存在し、CEFのいくつかのバージョンに渡って互換性が維持されます。

以下は概念図です。あるmuonバージョンは、固定されたCEF APIバージョンを使用します。
そして、そのCEF APIバージョンをサポートする複数のCEFバイナリの範囲が、バージョンウインドウになります:

```mermaid
flowchart LR
  subgraph muon_versions["muonパッケージ"]
    muon_a["muon A<br/>CEF API 13301"]
    muon_b["muon B<br/>CEF API 13301"]
    muon_c["muon C<br/>CEF API 13600"]
  end

  subgraph cef_versions["CEFバイナリ"]
    cef_133["CEF 133.x<br/>supports API 13301"]
    cef_134["CEF 134.x<br/>supports API 13301"]
    cef_135["CEF 135.x<br/>supports API 13301"]
    cef_136["CEF 136.x<br/>supports API 13600"]
    cef_137["CEF 137.x<br/>supports API 13600"]
  end

  muon_a --> cef_133
  muon_a --> cef_134
  muon_a --> cef_135
  muon_b --> cef_133
  muon_b --> cef_134
  muon_b --> cef_135
  muon_c --> cef_136
  muon_c --> cef_137
```

muon-coreと起動ヘルパーには、muon-coreのビルド情報と、muonバイナリが参照するCEFバージョン情報が埋め込まれています。
`tested` 以外のpolicyでは、この `cefReference` とカタログファイル、さらに候補archive内の `include/cef_api_versions.h` に含まれるAPI hashから、使用可能なCEFバージョンを特定します。
実行中にロードされたCEFの情報は `muon.environments.getRuntimeInfo()` の `cefRuntime` で確認出来ます。

- 注意: CEF APIバージョンはABI互換を目的としていますが、CEF機能の挙動互換は必ずしも維持されない可能性があります。
  つまり、API hashが一致していても、CEFのブラウザ機能としての差異は発生するかもしれません。
  CEF APIバージョニングの詳細は、CEF公式の [API Versioning](https://chromiumembedded.github.io/cef/api_versioning.html) を参照して下さい。

`compat-latest` や `same-major-latest` はABI互換を確認しますが、Chromium/CEFのブラウザ機能としての挙動差までは保証しません。アプリケーション側で対象CEFの検証を行ってから配布して下さい。

