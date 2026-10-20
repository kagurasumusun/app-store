# プロトコルリファレンス

このドキュメントは IPAShelf が**実装している** Apple のエンドポイントと
リクエスト形式をまとめたものです。公開情報と観測結果から独立に実装しており、
`reference/` のコードは使用していません。該当する実装は
`Core/src/store_client.cpp`、`Core/src/bag.cpp`、`Core/src/owned_apps.cpp`、
`Core/src/sap_software.cpp` です。

> 2026 年 8 月以降、認証系のリクエストは SAP 署名が必須になっています。
> 署名のないリクエストは 403（空ボディ、数ミリ秒）で拒否されます。
> IPAShelf の署名はアプリ内蔵の純ソフトウェア実装です（§8）。

---

## 1. バッグ

```
GET https://init.itunes.apple.com/bag.xml?guid=<percent-encoded guid>
```

応答は XML plist で、ルートの `urlBag` 辞書から以下のキーを取得します
（`Bag::parse()`）。

| キー | 内容 |
| --- | --- |
| `authenticateAccount` | 認証エンドポイント（`https://buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/authenticate`、pod・クエリ付きもあり） |
| `volumeStoreDownloadProduct` | **ent/download の URL**（`https://downloaddispatch.itunes.apple.com/WebObjects/DownloadDispatch.woa/wa/ent/download`）。現在のバッグでは旧 volumeStore の URL ではありません |
| `redownloadProduct` | 再ダウンロード（`https://downloaddispatch.itunes.apple.com/r/redownload`） |
| `updateProduct` | 更新用エンドポイント（`/up/updateProduct`） |
| `sign-sap-setup` | SAP セットアップ交換先（`https://fpinit.itunes.apple.com/v1/signSapSetup/legacy`） |
| `sign-sap-setup-cert` | SAP セットアップ証明書（`https://s.mzstatic.com/sap/setupCert.plist`） |
| `sign-sap-version` | SAP プロトコル版（文字列 `"200"`。整数で返る場合も受理） |

`Bag::parse()` 自体は文字列の抽出だけを行い、検証しません。検証は
利用箇所で行います。これは意図的な分離で、ダウンロード系は SAP 要件なしに
バッグを使い、サインイン系だけ厳格に検査します。

- `Bag::validate()` — 認証 URL が正規化可能で、dispatch 系 URL
  （ent / redownload / updateProduct）があれば
  `downloaddispatch.itunes.apple.com` 上の正確なパス
  （`/WebObjects/DownloadDispatch.woa/wa/ent/download`、`/r/redownload`、
  `/up/updateProduct`）であり、クエリ・fragment が無いこと。
- `Bag::validate_for_authentication()` — 上記に加え SAP
  setup / setup-cert が https で存在し、版が 200 であること。
- `normalize_auth.endpoint()` — https 固定、userinfo / fragment /
  パーセントエンコードされたパス禁止、ポートは空か 443。ホストは
  `buy.itunes.apple.com` 本体か末尾 `-buy.itunes.apple.com` の pod のみ。
  パスは `/WebObjects/MZFinance.woa/wa/authenticate[/]` のみ受理し、
  正規化は末尾 `/` の補完だけを行います（クエリと `:443` は保持）。

User-Agent が素の curl 等だと `sign-sap-*` を含まないバッグが返るため、
取得には Configurator の User-Agent を使います。

### ストアフロント

`models.cpp` に地域表があり、`storefronts()` が UI 用の一覧
（コード・名前・ID・通貨）を返します。購入・ダウンロード時の
`X-Apple-Store-Front` はアカウントに紐づく値（例 `143441-1,29`）を
そのまま使います。購入履歴（§6）だけは末尾のサフィックスを
`,34`（Configurator）と `,13`（Mac App Store）に読み替えて両方取得します。

---

## 2. 認証

```
POST {normalize(bag.authenticateAccount)}
Content-Type: application/x-www-form-urlencoded
X-Apple-ActionSignature: base64(SAP-Sign(body))
```

**ボディはフォームではなく XML plist です**（全値が文字列）。
MZFinance はフォーム形式のボディを処理しません。

```xml
<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0"><dict>
  <key>appleId</key><string>user@example.com</string>
  <key>attempt</key><string>1</string>
  <key>guid</key><string>A1B2C3D4E5F6</string>
  <key>password</key><string>secret</string>
  <key>rmp</key><string>0</string>
  <key>why</key><string>signIn</string>
</dict></plist>
```

- 2FA コードは空白を取り除いて 6 桁数字であることを確認し、
  パスワード末尾に連結します。
- **送信のたびに SAP 署名し直します**（リダイレクト・リトライも含む）。
  SAP 署名には毎回新しい IV が含まれるため、署名の使い回しはしません。
  SAP セッションを確立できない場合は未署名で送らずエラーにします。
- 電話番号でのサインインでは国を推定して
  `X-Apple-Store-Front` のヒントを付けます。中国（`861` 始まり 13 桁、
  または `1` 始まり国内 11 桁）→ `143465`、インド（国番号 `91` 付き
  12 桁、国内先行 `0` 付き 11 桁、`6`–`9` 始まり 10 桁）→ `143467`。
  対応外の国際番号にはヒントを付けません。メールの場合は付けません。
- リダイレクトは **301 / 302 / 307 / 308 のみ**追随します（303 はエラー）。
  宛先は `validate_auth_endpoint()` を通ること（pod ホストへの相対・
  絶対どちらも解決して検証）。attempt は維持し、上限は 4 回です。

### リトライ

`send_login_request()` は 204 / 403 / 404 / 429 / 5xx と
トランスポートエラーを再試行します（最大 3 回、10 秒から倍々に最大 30 秒）。
`Retry-After` は秒数と HTTP 日付（IMF-fixdate / RFC 850 / asctime）の
両形式に対応します。

### 応答の解釈

| 応答 | 扱い |
| --- | --- |
| `failureType=-5000`（資格情報エラー） | 1 回目の attempt だけ再試行（Apple 側の一過性応答対策）。2 回目以降は認証エラー |
| `failureType` 空 + `MZFinance.BadLogin.Configurator_message` | コード未送信なら **2FA が必要**。コード送信後なら認証エラー |
| `Your account is disabled.` | アカウント無効（専用エラー） |
| 成功 | `accountInfo` と **`X-Set-Apple-Store-Front` ヘッダ（単一値）** が必須。`pod` は応答内の値（任意） |

成功時のメールアドレスは `accountInfo` のものを優先します。
アカウント（パスワードとトークンを含む）は Keychain に保存されます。

---

## 3. 検索・詳細・レビュー

すべて公開エンドポイントで SAP 署名は不要です。

```
GET https://itunes.apple.com/search?entity=<entity>&limit=<1..200>
    &media=software&term=<query>&country=<cc>&genreId=<id>
GET https://itunes.apple.com/lookup?id=<appID>&country=<cc>
GET https://itunes.apple.com/<cc>/rss/customerreviews/page=<n>/id=<appID>
    /sortby=mostrecent/json
```

- 価格フィルタと並び替えはクライアント側で適用します。
- レビューの次ページ有無はフィードの `rel="next"` で判定します。

| プラットフォーム | search entity |
| --- | --- |
| iPhone | `software` |
| iPad | `iPadSoftware` |
| Mac | `macSoftware` |
| Apple TV | `software,tvSoftware` |
| Apple Watch | `software,watchSoftware` |
| Vision Pro | `xrosSoftware` |

最新ビルドの外部バージョン ID はカタログ API
（`apps.apple.com/api/apps/v1/catalog/...` の
`platformAttributes.<platform>.externalVersionId`、Vision は
検索レスポンスの `purchaseConfiguration.appExtVrsId`）から取得します。

---

## 4. 購入

```
POST https://p<pod>-buy.itunes.apple.com/WebObjects/MZFinance.woa/wa/buyProduct
     （pod が無ければ buy.itunes.apple.com）
```

- **無料アプリのみ**対象です（`price > 0` はエラー）。
  `pricingParameters` は通常 `STDQ`、Apple Arcade は `GAME`。
- 成功判定は `jingleDocType=purchaseSuccess` かつ `status=0`。
  既に所有している場合（HTTP 500、`failureType=5002` ライセンス既存）も
  成功として扱います。
- ダウンロード時に `kLicenseRequired` が返ったら、ブリッジが購入を
  試みてから再取得します。

---

## 5. ダウンロード

`StoreClient::download_offer()` は次の順に試します。各応答は
アイテム ID・外部バージョン ID・Bundle ID の一致を検証してから採用し
（ent の応答は特に厳格）、構造化エラー（ライセンス要求 9610、
トークン失効 2034/2042/1008/5002 など）は即座に返します。

1. **ent/download**（バッグの `volumeStoreDownloadProduct` キー）
   - kbsync 生成器が設定されている場合のみ。バージョン ID が必須で、
     未指定なら platform 別の最新 ID を先に解決します。
   - ペイロードは plist（`salableAdamId` は文字列、`kbsync`、
     `serialNumber`）で `Content-Type: application/x-www-form-urlencoded;
     charset=utf-8`、`X-Token` 付き、リダイレクト禁止
     （トークンを転送先に漏らさないため）。
   - kbsync ブロブはプラットフォームのキャッシュ（iOS では Keychain の
     `kbsync-v1`）から読み、成功したときだけ保存します。キャッシュした
     ブロブが拒否されたら破棄して 1 回だけ再生成します。
   - **iOS 版には kbsync 生成器が無い**ため、この経路は使われず
     2 以降のレガシーチェーンで取得します（生成は FairPlay の
     機械語エミュレーションに依存する処理で、ポータブルな
     再実装は存在しないため）。
2. **volumeStore** —
   `https://p<pod>-buy.itunes.apple.com/.../volumeStoreDownloadProduct` を
   アカウントの pod から組み立てます（バッグの URL は使いません）。
   バージョンキーは `externalVersionId`。
3. **redownload**（バッグの `/r/redownload`）— 2 が「空」
   （200 で失敗情報もアイテムも無い）または「no longer available」の
   ときだけ。バージョンキーは `appExtVrsId`。
4. **updateProduct**（バッグの `/up/updateProduct`）— 3 が HTTP 500
   または利用不可で、エンドポイントとバージョンがあるときだけ。

レガシー系（2–4）のペイロードは plist
（`creditDisplay=""`、`guid`、`salableAdamId` は整数、
`serialNumber="0"`）で `Content-Type: application/x-apple-plist`、
URL に `?guid=` を付けます。

応答 `songList[0]` からパッケージ URL、`md5`、sinf 一覧、
メタデータ（`itemId`、`softwareVersionExternalIdentifier(s)`、
`bundleShortVersionString` など）、`artworkURL` を読みます。

### バージョン一覧

`version_list()` はダウンロード応答メタデータの
`softwareVersionExternalIdentifiers` をサーバーの順序のまま返し、
最新 ID を別に返します（並び替えや重複除去はしません）。

### バージョン情報（表示バージョンとリリース日）

`version_metadata()` はバージョンを固定したダウンロード応答の
アイテムメタデータ（`bundleShortVersionString` と `releaseDate`）を
返します。応答側の外部バージョン ID がリクエストと一致することを
ダウンロード経路で検証済みなので、そのメタデータは対象ビルド自身の
ものです。パッケージ本体は取得しません。

### ストア系 POST のリダイレクト

`buy.itunes.apple.com` 系・DownloadDispatch 系のエンドポイントは
pod 間リダイレクト（301/302/303/307/308）を返し、**同一 POST の
再送**を期待します。プラットフォーム標準の自動追随は POST を
ボディ無し GET に変換してしまうため、コアはリダイレクトを自前で
処理し、想定したストアエンドポイントへの遷移だけを許可します
（ダウンロード系と購入 `buyProduct`）。

### パッケージ本体の取得とレジューム

実ファイルの取得はブリッジの `NSURLSessionDownloadTask` が担います。
キャンセル・失敗時は **resume data** をアプリ+バージョン単位で
Caches に保存し、再試行やアプリ再起動後も途中から継続します。
HTTP エラーのボディが「ファイル」として保存される事故を防ぐため、
完了時はステータスが 200/206 であることを確認します
（ステールな resume data はその場で破棄）。

---

## 6. 購入履歴（所有アプリ）

`owned_apps()` は DAAP エンドポイント
`https://pd.itunes.apple.com/WebObjects/MZPurchaseDaap.woa/purchase`
を使います。

1. `POST /login`（無署名・ボディなし）→ セッション ID `mlid`
2. `POST /update` — 生フォーム文字列
   `session-id=<id>&revision-number=(null)&query=<query>` を
   **SAP 署名** → リビジョン `musr`
3. `POST /databases/<musr>/items` — DMAP `adsr` ドキュメント
   （`mstc` 現在時刻、`mlid`、`mikd=2`、`musr`、`mder=0`、
   `mque` クエリ、`aetl` 空）を **SAP 署名**

クエリはメディア種別 131072（アプリ）/ 262144（Arcade）/
67108864（Mac）の和集合です。ストアフロントは `,34` と `,13` の両方を
取得してアプリ ID 単位でマージします（対応 platform は和集合、
購入日は新しい方）。結果は購入日の新しい順（日付なしは末尾）で、
ページ単位で返します（`limit` 最大 100）。HTTP / DMAP ステータスの
401・403 はトークン失効として返します。

アイテム解釈: `aeSI`=アプリ ID、`aeBI`=Bundle ID、
`aeLN`（無ければ `minm`）=名前、`aePd`=表示バージョン、
`asdp`=購入日（Unix 秒）。対応 platform はメディア種別 67108864 なら
macOS、それ以外は対応製品ビット（1=iPhone / 2=iPad / 8=macOS /
16=visionOS）から決まります。

DAAP リクエストには端末のタイムゾーン（`X-Apple-I-TimeZone`、
`X-Apple-TZ`=オフセット分）と現在時刻のヘッダを付けます。

---

## 7. IPA の仕上げ

`write_patched_ipa()` がソースの ZIP をそのまま複製しながら次を行います。

1. ルートの既存 `iTunesMetadata.plist` / `iTunesArtwork` と、
   上書き対象の sinf エントリを除外
2. `iTunesMetadata.plist` を追加 — サーバー提供メタデータ +
   購入アカウント（`apple-id` / `userName`）を **バイナリ plist** で
3. `iTunesArtwork` を追加 — ダウンロード応答の `artworkURL` から取得
   （取得失敗は致命的扱いにせず、アートワークなしで完成させる）
4. sinf を注入 — `SC_Info/Manifest.plist` の `SinfPaths` があれば
   応答の sinf と 1 対 1 で対応付け（数が食い違えばエラー）。
   無ければ `SC_Info/<CFBundleExecutable>.sinf` に先頭の sinf
5. `.partial` に書いて検証後に rename（原子的配置）

パッチ前にブリッジが MD5・Bundle ID・**要求 platform と
`CFBundleSupportedPlatforms` の一致**を検証します（宣言自体が無い
レガシー iOS アプリだけは iPhoneOS 要求を許容）。

---

## 8. SAP 署名（純ソフトウェア実装）

署名器は `Core/src/sap_software.cpp` の完全なソフトウェア実装です。
プライベートフレームワークや機械語エミュレーションは使わないため、
サンドボックス内の iOS アプリでも Simulator でも同一に動作します。

- セットアップ: バッグの `sign-sap-setup-cert` から証明書を取得し、
  `sign-sap-setup` へ 2 往復の交換（exchange）を行って
  セッションを確立します。入力は 6 バイトの端末識別子と公開証明書だけで、
  パスワードは使いません。
- 署名形式: `02 || IV || tag || BE32(len(blob)) || blob`。
  blob はセッション内でキャッシュされ、IV は署名ごとに新規です。
- プロトコルの定数テーブルは `ThirdParty/sap-tables/` に置き、
  `Scripts/generate_sap_tables.py` が C++ ソース
  （`sap_tables.hpp/.cpp`）を生成します。
- 実装は独立したテストベクタ（合成ベクタ + ハンドシェイク
  フィクスチャ 4 ケース）にバイト一致することをテストで検証しています。
- `SapBackend` 抽象（initialize / exchange / sign / teardown）は
  テスト差し替え用に残してあります。プラットフォーム側のファクトリを
  設定しない場合、コアがこのソフトウェア実装を自動で選択します。

---

## 9. 意図的に対象外

- 有料アプリの新規購入決済（Apple の支払い UI を第三者が出せない）
- kbsync ブロブの生成（ent/download の優先経路。§5 参照）
- macOS `.pkg` のバージョン情報取得
- アプリ内課金の復元、家族共有の管理
- Apple Account の作成・パスワード変更
- コード署名の除去やアプリコードの復号（sinf はそのまま保持）
