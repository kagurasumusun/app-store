# プロトコルリファレンス

このドキュメントは IPAShelf が**実装している** Apple のエンドポイントと
リクエスト形式をまとめたものです。公開情報と観測結果から独立に実装しており、
`reference/` のコードは使用していません。該当する実装は
`Core/src/store_client.cpp` と `Core/src/bag.cpp` です。

> 2026 年 8 月以降、認証系のリクエストは SAP 署名が必須になっています。
> 署名のないリクエストは 403（空ボディ、数ミリ秒）で拒否されます。

---

## 1. バッグ

```
GET https://init.itunes.apple.com/bag.xml?guid=<guid>
```

XML plist を `Bag::parse()` で読みます。ルートに `urlBag` があれば
その辞書を使い、以下のキーを取得します。

| キー | 用途 |
| --- | --- |
| `authenticateAccount` | 認証エンドポイント（`https://buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/authenticate`） |
| `volumeStoreDownloadProduct` | ダウンロードエンドポイント |
| `redownloadProduct` | 再ダウンロード（`https://downloaddispatch.itunes.apple.com/r/redownload`） |
| `updateProduct` | 更新用エンドポイント（`/up/updateProduct`） |
| `storeFront` | 既定のストアフロント |
| `SAP-*` | SAP セットアップパラメータ |

`validate_endpoint()` は **https 固定・userinfo 禁止・port 禁止・fragment 禁止**
を強制します。バッグの URL をそのまま信用しないための防御です。
`bag.cpp` のテストは不正なスキームや `javascript:` を拒否することを確認します。

### ストアフロント

`storefront_header(storefront)` は `"<id>-1,29"` を返します
（`X-Apple-Store-Front` ヘッダの形式）。

| 地域 | ID |
| --- | --- |
| アメリカ | 143441 |
| 日本 | 143462 |
| イギリス | 143444 |
| ドイツ | 143443 |
| フランス | 143442 |
| 中国 | 143465 |
| 韓国 | 143466 |
| 台湾 | 143470 |
| 香港 | 143463 |
| シンガポール | 143464 |
| オーストラリア | 143460 |
| カナダ | 143455 |
| インド | 143467 |
| ブラジル | 143503 |
| メキシコ | 143468 |

`models.cpp` に 50 地域の表があり、`storefronts()` が
UI 用の一覧（コード・名前・ID・通貨）を返します。

---

## 2. 認証

```
POST {bag.authenticateAccount}
Content-Type: application/x-www-form-urlencoded
X-Apple-ActionSignature: base64(Sign(body))
User-Agent: Configurator/2.17 (Macintosh; OS X 15.2; 24C5089c) AppleWebKit/0620.1.16.11.6
```

ボディ（`build_login_body`）:

```
appleId=<email>
attempt=<1..3>
guid=<device id>
password=<password>          // 2FA 時は password + code
rmp=0
why=signIn
```

署名はボディのバイト列そのものです。`SapSigner` が使えない場合、
`IPCreateSAPBackend()` は **署名なしリクエストを送らずに**
`kSignerUnavailable` を返します。

成功時は XML plist に `dsPersonId`、`passwordToken`、`accountInfo`、
`storefront`、`pod` が入ります。これらは `Account` に変換され、
Keychain に保存されます（パスワードも同様）。

| failureType / customerMessage | 意味 |
| --- | --- |
| `-5000` | 2FA コードが必要 |
| `MZFinance.BadLogin.Configurator_message` | 資格情報が違う |
| `Your account is disabled.` | アカウント無効 |
| `Subscription Required` | サブスクリプションが必要 |
| `Your password has changed.` | パスワード変更済み |

`map_store_failure()` がこれらを `ErrorCode` に変換します。

### リトライ

`send_login_request()` は次のステータスを再試行対象にします。

```
204, 403, 404, 429, 5xx
```

待機時間は `10s * 2^n` を上限 30 秒で頭打ちにし、`Retry-After` が
あればそれを優先します（上限を超える値は無視）。試行回数は既定で 3 回です。

---

## 3. 検索

iTunes Search API（公開・署名不要）。

```
GET https://itunes.apple.com/search
    ?entity=<software|iPadSoftware|macSoftware>
    &limit=<1..200>
    &media=software
    &term=<query>
    &country=<cc>
    &genreId=<id>          (任意)
```

- 並び順はパラメータの順序まで `build_search_url()` が固定します
  （テストでバイト比較するため）。
- 価格フィルタと並び替えはクライアント側で適用します
  （`StoreClient::search(const SearchQuery&)`）。
- プラットフォームごとの `entity`:

  | プラットフォーム | entity |
  | --- | --- |
  | iPhone | `software` |
  | iPad | `iPadSoftware` |
  | Mac | `macSoftware` |
  | Apple TV | `software,tvSoftware` |
  | Apple Watch | `software,watchSoftware` |
  | Vision Pro | `xrosSoftware` |

---

## 4. 詳細・レビュー・バージョン

### 詳細

```
GET https://itunes.apple.com/lookup?id=<appID>&country=<cc>
GET https://itunes.apple.com/lookup?bundleId=<bundleID>&country=<cc>
```

`results[0]` を `app_from_json()` が `App` に変換します。
`screenshotUrls` は iPhone、`ipadScreenshotUrls` は iPad 用です。

### レビュー

```
GET https://itunes.apple.com/<cc>/rss/customerreviews/page=<1..10>/id=<appID>/sortby=mostrecent/json
```

`has_more` はフィード内の `rel="next"` リンクで判定します。
404（レビューが無いアプリ）はエラーとして返し、UI 側
（`AppModel.loadReviews`）が「レビューなし」として空表示に変換します。

### 最新バージョン

```
GET https://apps.apple.com/api/apps/v1/catalog/<cc>/apps/<appID>?platform=<ios|ipados|macos>
```

`platformAttributes.<platform>.externalVersionId` を読みます。
Vision Pro は `<cc>/vision/search` の
`purchaseConfiguration.appExtVrsId` を使います。

---

## 5. 購入とダウンロード

### 購入

```
POST https://buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/buyProduct
```

- **無料アプリのみ**購入します。`price > 0` の場合は
  `kUnsupported`（「有料アプリは購入できません」）を返します。
- 既に所有している場合や無料アプリの取得は `204 No Content` で成功します。
- `pricingParameters` は無料なら `STDQ`、Arcade は `GAME` です。
- アカウントの `pod` に応じてホストを `p<NN>-buy.itunes.apple.com` に
  書き換えます（`apply_pod()`、sanitize 済み）。

### ダウンロード

```
POST {bag.volumeStoreDownloadProduct}?guid=<guid>
Content-Type: application/x-www-form-urlencoded
X-Apple-Store-Front: <storefront>-1,29
```

ダウンロードのフォールバック順（`StoreClient::download_offer`）:

1. `volumeStoreDownloadProduct`
2. 失敗したら `bag.redownloadProduct`（`/r/redownload`）に
   `appExtVrsId` を付けて再試行
3. バージョン未指定なら、まず公開カタログから最新の
   `externalVersionId` を取得してから 2 を実行
4. `500` / `kNoOffer` / `kTemporarilyUnavailable` の場合は
   `bag.updateProduct`（`/up/updateProduct`）を試す

レスポンスの `songList[0]` から `URL`（IPA の URL）、`sinf`、
`metadata`（`iTunesMetadata.plist` 相当）、
`softwareVersionExternalIdentifiers` を読みます。

### バージョン一覧

`version_identifiers()` はダウンロード応答の
`softwareVersionExternalIdentifiers` をそのまま返します。
Apple は**表示用のバージョン番号を返さない**ため、UI は
「Build 1234567890」と表示します。最新版は先頭に補完されます。

---

## 6. IPA のパッチ

1. ダウンロードした `.ipa` を miniz で開く
2. `Payload/<X>.app/` を特定
3. `iTunesMetadata.plist`（購入情報）をルートに追加
4. `iTunesArtwork`（512×512 PNG）をルートに追加
5. `Payload/<X>.app/SC_Info/<CFBundleExecutable>.sinf` を追加
6. 新しい ZIP を `.partial` に書き、MD5 を検証してから rename

`ipa.cpp` は ZIP のエントリ名・圧縮方式・サイズを検査し、
パストラバーサルや暗号化エントリを拒否します。

---

## 7. SAP 署名

`SapBackend` は 5 つの関数を持ちます。

```cpp
Initialize(uint64_t* out, uint8_t* input);   // セットアップ
Exchange(...);                               // ハンドシェイク
Sign(...);                                   // ボディ署名
Teardown(...);                               // 終了
dispose(...);
```

iOS 実装（`IPSAPBackend.mm`）は CoreFP・CommerceKit・CommerceCore を
`dlopen` してこの ABI を呼びます。ハードウェア識別子は 6 バイトで、
先頭に LE の u32 長（≤20）を持つ 24 バイトのブロックとして渡します。

入力は**端末識別子と公開証明書だけ**で、パスワードは使いません。
Simulator には CoreFP が無いため、サインインは実機専用です。

---

## 8. 意図的に対象外

- 有料アプリの新規購入（Apple の支払い UI を第三者が出せない）
- アプリ内課金の復元、家族共有の管理
- Apple Account の作成・パスワード変更
- コード署名の除去（`sinf` はそのまま保持）
