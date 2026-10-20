# インストール（OTA）の仕組み

iOS は、**信頼された証明書で HTTPS 配信された OTA マニフェスト**を
`itms-services` で開いたときだけ IPA をインストールします。
IPAShelf はその一式を端末内で完結させます。

```
┌──────────────┐  1. 証明書を配布（平文 HTTP）
│  Safari      │◀─────────────────────────────┐
│  (iOS)       │                              │
│              │  2. プロファイルをインストール │
│              │     設定で信頼を有効化        │
│              │                              │
│  itms-services│  3. マニフェスト取得（HTTPS） │
│              │─────────────────────────────▶│
│              │  4. IPA ダウンロード（HTTPS）  │  IPInstallServer
└──────────────┘                              │  (nw_listener :0)
                                              │
                                    ┌─────────┴─────────┐
                                    │ ルート CA (ECDSA)  │
                                    │ リーフ証明書       │
                                    │ Keychain の秘密鍵  │
                                    └───────────────────┘
```

---

## 1. 証明書

初回起動時（または `Settings → インストール`）に、コアの
`build_ec_certificate()` が 2 枚の証明書を生成します。

| | ルート CA | リーフ |
| --- | --- | --- |
| 名前 | `IPAShelf Root CA` | `IPAShelf Device` |
| シリアル | 1 | 2 |
| 有効期間 | 10 年 | 10 年 |
| 拡張 | `basicConstraints CA:TRUE`、`keyUsage keyCertSign` | `extendedKeyUsage serverAuth`、SAN（ホスト名・IP） |
| 鍵 | ECDSA P-256（Keychain、`kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly`） | 同じ |

秘密鍵は Keychain にのみ保存し、`kSecAttrSynchronizable=false` で
バックアップにも含めません。Application Support に置くのは DER
証明書だけで、`isExcludedFromBackup` を設定しています。

### フィンガープリント

`Settings → インストール証明書` に SHA-256 の先頭 23 文字を表示します。
Safari で証明書を確認するときの照合用です。

---

## 2. 配信

`IPInstallServer` は `Network.framework` の `nw_listener` を
`0.0.0.0:0`（自動割り当て）で開き、2 つのポートを使います。

| ポート | プロトコル | 内容 |
| --- | --- | --- |
| TLS | HTTPS | `/manifest.plist`、`/icon.png`、`/ipa`、`/`（案内ページ） |
| 平文 | HTTP | `/ca.cer` のみ（信頼前でも取得できるように） |

- TLS は 1.2 / 1.3。証明書は `sec_identity_create()` で
  `SecIdentity` から作成します。
- HTTP の解析・Range 対応・ファイル配信は C++ コア（`ota.cpp`）が行い、
  Objective-C++ 側はソケットと TLS だけを担当します。
- 64 KiB 単位で送信し、進捗を `stateHandler` に通知します。
- `Host` ヘッダが実際の接続先と一致しない場合は **421** を返します
  （DNS リバインディング対策）。

### マニフェスト

```xml
<plist version="1.0">
<dict>
  <key>items</key>
  <array><dict>
    <key>assets</key>
    <array>
      <dict>
        <key>kind</key><string>software-package</string>
        <key>url</key><string>https://192.168.1.10:49152/ipa</string>
      </dict>
      <dict>
        <key>kind</key><string>display-image</string>
        <key>needs-shine</key><false/>
        <key>url</key><string>https://192.168.1.10:49152/icon.png</string>
      </dict>
      <dict>
        <key>kind</key><string>full-size-image</string>
        <key>url</key><string>https://192.168.1.10:49152/icon.png</string>
      </dict>
    </array>
    <key>metadata</key>
    <dict>
      <key>bundle-identifier</key><string>com.example.app</string>
      <key>bundle-version</key><string>1.2.3</string>
      <key>kind</key><string>software</string>
      <key>title</key><string>Example</string>
    </dict>
  </dict></array>
</dict>
</plist>
```

インストール URL は次の形式です。

```
itms-services://?action=download-manifest&url=https%3A%2F%2F192.168.1.10%3A49152%2Fmanifest.plist
```

---

## 3. ユーザーの手順

アプリの「インストール」シートが順に案内します。

1. **証明書ページを開く**（平文 HTTP）
   Safari が「構成プロファイル」をダウンロードします。
2. **設定 → 一般 → VPN とデバイス管理 → ダウンロード済みプロファイル** で
   インストール。
3. **設定 → 一般 → 情報 → 証明書信頼設定** で
   「IPAShelf Root CA」を有効化。
4. アプリに戻り **インストール** をタップ。
   `itms-services` URL が開き、ホーム画面に追加されます。

> Root CA の信頼は端末全体に影響します。自己利用の端末でのみ行ってください。
> 不要になったら「証明書信頼設定」をオフにし、プロファイルを削除してください。

---

## 4. トラブルシューティング

| 症状 | 原因と対処 |
| --- | --- |
| 「サーバに接続できません」 | TLS 証明書の SAN と接続先 IP が不一致。Wi-Fi を切り替えたらシートを閉じて開き直す（サーバーと証明書を作り直す） |
| インストールが始まらない | 証明書が「信頼」されていない。設定 → 情報 → 証明書信頼設定を確認 |
| 「App をインストールできません」 | IPA の Bundle ID とマニフェストが不一致、または `sinf` が壊れている。ライブラリから再ダウンロード |
| 途中で止まる | アプリがバックグラウンドで停止した。`beginBackgroundAssertion()` で最大 30 秒延長するが、大きな IPA は画面を消さずに実行する |
| ポートが開かない | ローカルネットワークの許可を拒否した。設定 → プライバシーとセキュリティ → ローカルネットワーク |

---

## 5. なぜ自前サーバーが必要か

- `itms-services` は **https** のマニフェスト URL しか受け付けません。
- 証明書は端末が信頼している必要があるため、初回は平文 HTTP で
  ルート証明書を配る必要があります。
- iOS には汎用の HTTP サーバー API がないため、`nw_listener` と
  C++ の HTTP 実装を組み合わせています。

この方式は App Store の審査を想定していません。自己利用・検証用途です。
