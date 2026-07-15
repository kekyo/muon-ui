# Local asset packing (Advanced topics)

Local asset files can be "packed" into a single file.
This prevents files from being scattered, compresses them to reduce storage size, and enables corruption verification.
By default, local assets are automatically packed when packages are generated.

The packing format is a zip file, and any zip compression tool can be used.
The structure inside the zip file should follow the structure inside the `assets/` directory as-is.
In other words, place files like this:

```text
assets.zip
+-- main/
|   +-- index.html
|   +-- app.js
|   +-- style.css
|   +-- images/
|       +-- logo.png
+-- sub/
    +-- index.html
    +-- child.js
```

Then specify that file in `asset.sourcePath`:

```json
{
  "asset": {
    "sourcePath": "./assets.zip"
  }
}
```

When `asset.sourcePath` points to a file, muon treats it as packed local assets and accesses it accordingly.

You can also enable verification of the entire packed file:

```json
{
  "asset": {
    "sourcePath": "./assets.zip",
    "signature": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
    "salt": "0d48cab58f2a45efa1f883c1f0c6f88c"
  }
}
```

- `asset.signature` optionally pins the SHA-256 digest of the zip file, and can only be a 64-digit hexadecimal string.
  When this value is specified, `asset.salt` must also be specified as an even-length hexadecimal byte string.
  muon concatenates the zip file bytes and the bytes decoded from `asset.salt` in that order, then calculates and compares `SHA-256(zip bytes || decoded salt bytes)`.
- When `muon build` or `muon pack` generates an asset archive, `salt` and `signature` are also generated automatically and inserted into the final configuration.
  If you directly use a pre-packed zip file, you can specify these values manually, but using the muon CLI is recommended.
- A digest with a published `salt` is an integrity check for detecting asset corruption.
  An attacker who can replace both assets and configuration values can also recalculate the digest, so this does not guarantee content authenticity or tamper resistance against such an attacker.
  If those guarantees are required, provide them outside muon through the package system, code signing, trusted distribution channels, or similar mechanisms.

If this verification fails when both `asset.signature` and `asset.salt` are specified, the muon application cannot start.
