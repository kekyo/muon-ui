# ローカルアセットの構成

muonの特徴的な機能の一つが、ローカルアセットです。
デスクトップ版は `asset://` でローカルアセットを参照します。Android版はAPKに同梱したアセットを `https://main.asset.muon.invalid/` から読み込みます。[Androidのアセット設定](./android.md#設定とmuon-api)

muon Viteプラグインを使用している場合はあまり意識する必要はありませんが、muonアプリがどのようにページを表示するのかは知っておいたほうが良いでしょう。
デスクトップ版では、起動方法によってアセットの参照先が変わります。

1. `npm run dev` 実行時は、muonがViteの開発サーバーから `http://localhost:5173/index.html` を読み込みます。
2. `npx muon run` でmuonアプリを実行した場合は、 muonが `dist/` の内容を直接 `asset://main/` として参照可能にします。結果として `asset://main/index.html` が参照されます。
3. `npm run build` で生成されたmuonアプリ (`dist-muon/linux-amd64/`など) を実行した場合は、アセットファイル群を直接 `asset://main/` として参照可能にします。結果として `asset://main/index.html` が参照されます。このとき、アセットファイル群は `dist/` ではなく、これらをパッキングした `assets.zip` から参照します。

1は、ViteによるHMRデバッグの体験を向上させるために必要で、Viteから開発起動を行っている場合のみ、URLオリジンはViteサーバー(`http://localhost:5173/`)となります。2と3は、muonアプリとして配布した状態に近く、URLオリジンは `asset://main/` です。

このように、URLのオリジンが変化するため、muonアプリ実装時には注意が必要です。
例えば、現在のURLを固定的に `http://localhost:5173/` から判定するコードを記述すると、muonアプリとしてビルドされた途端に誤動作を起こします。
Viteを普段から使用している開発者であれば、デプロイ後にURLオリジンが変わることは一般的に想定されることなので、それほど違和感は無いと思われます。
