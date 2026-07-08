## ローカルアセットの権限 (Advanced topics)

`asset://` URLスキームは、証明書が存在しないことを除き、 `https://` と同様に独立した信頼性のあるオリジンとして扱われます。
このことは、CORSやCSRFに影響を与える可能性があるため、厳密な設計を行う場合は考慮が必要です。

ところで、ローカルアセットは、何らかのサブディレクトリ内に配置する必要があります。
既定では `assets/main/` のように、`main/` サブディレクトリ内にアセット群を配置します。

この `main` という名称は、 `asset://main/` のように、URLのドメイン名部分に対応していて、
`assets/sub/` のようにディレクトリを分けた場合は、異なるオリジンとして振る舞わせることが可能です。
（注意: `main` という単語自体には意味はありません）

この機能と、先程のCORS,CSRF制御や `plugin.pages` や `browser.allowUnsafeJavaScriptParentAccess` を組み合わせて、
muon上でのページ権限を細かく調整することも可能です。以下は既定の定義です:

```json
{
  "plugin": {
    // 既定ではvalidateモードで使用する
    "mode": "validate",
    // muonプラグイン関数は `asset://main/` のページでのみ使用可能にする
    // 例えば、 `asset://sub/` のページではmuonプラグインブリッジを参照できない
    "pages": ["asset://main/**"]
  }
}
```

---

