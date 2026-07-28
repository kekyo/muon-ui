# 外部ネットワークにアクセスする

最初に解説したとおり、muonの様々な機能は「ホワイトリスト」形式で使用可能になります。
そして、CEFで最も重要なネットワークアクセスも、ホワイトリストに従ってフィルタされています。

`muon.json` に以下のように許可リストを追加することで、外部ネットワークにアクセスが可能になります:

```json
{
  "network": {
    "allow": [
      "asset://main/**"
    ]
  },
}
```

`network.allow` にURLのリストを追加すると、そのURLへのアクセスが可能になります。
省略時の既定値は `["asset://**", "data:image/**"]` で、すべてのローカルアセットとインライン画像データへのアクセスを可能にします。
`network.allow` を指定した場合は、既定のリストへの追加ではなく置き換えとなります。

例えば、意図的に空リスト (`network.allow: []`) にすると、ローカルアセットを含むすべてのネットワークアクセスが無効となり、何も表示できなくなります。
しかし、実際に空にしてみると、 `npm run dev` でViteサーバーとmuonを起動してみても正しく表示されるでしょう。
これは、 `npm run dev` した時に、この `network.allow` リストにViteサーバーのURLが一時的に追加されるためです。
空リストのままビルドを実行すると、無効なmuonアプリが生成されてしまうので注意して下さい。

- 注意: `data:...` のようなインラインデータURLも `network.allow` の対象です。
  既定値で許可されるのは `data:image/**` に一致する画像データだけです。`network.allow` を明示的に設定した後もインライン画像が必要な場合は、このパターンを設定リストに含めてください。

あなたが実装しているページが、外部サーバを参照する場合、例えば `<img>` タグの画像データだけ外部の `https://img.example.com/images/...` を参照する場合は:

```json
{
  "network": {
    "allow": [
      "asset://main/**",
      "https://img.example.com/images/**"
    ]
  },
}
```

のように、有効なURLを追加しておきます。これでローカルアセットと外部サーバーだけを参照することが出来ます。

- 画像だけではなく、すべてのネットワークアクセス（CSSファイルへのアクセスや `iframe` タグや `fetch` APIを使用したアクセス、WebSocketなど）が対象となるため、
  必要なURLをすべて追加する必要があります。
- このURLでは、擬似的なglobフォーマットを使用出来ます。`*` は `:`, `/`, `?`, `#` の区切りを越えませんが、`**`は以降のすべての文字にマッチします。

## ローカルネットワークにアクセスする

ChromiumはloopbackまたはローカルネットワークへのリクエストにLocal Network Access権限を要求します。
`network.allow`に宛先URLを追加したうえで、リクエスト元のオリジンを`network.localAccess`に指定してください。

```json
{
  "network": {
    "allow": [
      "asset://main/**",
      "http://localhost:5171/**"
    ],
    "localAccess": {
      "loopbackOrigins": [
        { "scheme": "asset", "domain": "main" }
      ],
      "localNetworkOrigins": []
    }
  }
}
```

`loopbackOrigins`はlocalhostやloopbackアドレス向け、`localNetworkOrigins`はLAN上のホスト向けです。
どちらもページ側のオリジンを完全一致で指定し、ワイルドカードは使用出来ません。
未指定の権限要求は明示的に拒否されます。

`network.localAccess.allowInsecureLocalhost` は、これらの権限や `network.allow` とは独立した設定です。
localhostの無効なHTTPS証明書の扱いだけを変更します。
