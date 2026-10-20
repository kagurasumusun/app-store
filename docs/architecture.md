# アーキテクチャ

IPAShelf は 3 層で構成されています。

```
SwiftUI (IpaStore/Sources, 15 ファイル)
      │  値型のモデル + async/await
Objective-C++ ブリッジ (IpaStore/Bridge, 10 ファイル)
      │  ポータブルな C++ API
C++20 コア (Core, 13 ヘッダ + 13 ユニット)
```

この分離には理由があります。

- **プロトコルを UI から切り離す**。Apple のリクエスト形式・レスポンス解析・
  plist/JSON の扱いはすべて C++ 側にあり、シミュレータを含むどの環境でも
  フェイクトランスポートでテストできます。
- **Objective-C++ は薄いまま**。`.mm` は C++ の型（`std::string`,
  `Result<T>`, `std::vector`）を受け渡す境界なので、Swift には
  Foundation の型だけを見せます。
- **Go を使わない**という制約を、プロトコル層の完全な再実装で満たしています。

---

## 1. コア（`Core/`）

| ヘッダ | 役割 |
| --- | --- |
| `base.hpp` | `Bytes`, `Status`/`Error`/`ErrorCode`, `Result<T>`, `CancellationToken`, ログ |
| `crypto.hpp` | MD5, SHA-1, SHA-256, Base64, ランダムバイト |
| `json.hpp` | 依存なしの JSON DOM（パース・シリアライズ、数値の原文保持） |
| `plist.hpp` | XML / バイナリ plist の読み書き |
| `url.hpp` | RFC 3986 のパーセントエンコード、クエリ、パス結合 |
| `models.hpp` | `App`, `Review`, `ReviewsPage`, `SearchQuery`, `Storefront`, `Account` |
| `http.hpp` | `HttpRequest`/`HttpResponse`, `HttpTransport` 抽象, ヘッダ管理 |
| `bag.hpp` | バッグ（認証エンドポイント・SAP パラメータ）の取得と解析 |
| `sap.hpp` | SAP 署名バックエンド抽象と、署名付きリクエストの組み立て |
| `store_client.hpp` | 認証・購入・ダウンロード・バージョン・レビュー・検索 |
| `ipa.hpp` | IPA の検査、`iTunesMetadata.plist`/`iTunesArtwork`/`SC_Info` の注入 |
| `ota.hpp` | OTA マニフェスト生成、HTTP/1.1 パーサ、Range 対応ルータ、サーバー |
| `x509.hpp` | ECDSA P-256 の証明書生成（CA / リーフ）、DER、PEM |

### 状態とスレッド

- `StoreClient` は**同期** API です。呼び出しはブリッジのシリアルキューから行い、
  進捗コールバックもそのキュー上で発火します。
- `HttpTransport` は抽象クラスで、iOS では `NSURLSession` 実装
  （`IPHTTPTransport.mm`）、テストではフェイク実装を使います。
- `CancellationToken` は `std::atomic<bool>` を持ち、ダウンロードの
  チャンク境界とリトライ待機で確認します。
- すべての失敗は `Error{code, message, http_status}` で返り、例外は投げません。

### データの流れ（ダウンロード）

```
User taps Download
  → IPStoreService.download(app:account:externalVersionID:platform:progress:completion:)
      → StoreClient::purchase()          (無料/所有済みなら 204 で成功)
      → StoreClient::download_offer()    (/r/redownload → volumeStoreDownloadProduct)
      → HttpTransport::send(download URL)
      → 署名済み sinf と iTunesMetadata を IPA に注入
      → Documents/IPAs/<Name>-<version>-<build>.ipa + sidecar .json
      → メインキューで completion(ipaPath)
```

### 設計上の選択

- **XML plist の生成順序を固定**。Apple のレスポンスと同様に
  `<key>`/値のペアを決定的な順序で出力し、テストでバイト比較できます。
- **JSON の数値は原文を保持**。`externalVersionId` は 10 桁を超える整数で、
  浮動小数に変換すると壊れるため `number_raw()` を使います。
- **OTA の HTTP は自前実装**。iOS に HTTP サーバーを置く手段がないため、
  `Network.framework` の `nw_listener` + 自前パーサで配信します。

---

## 2. ブリッジ（`IpaStore/Bridge/`）

| ファイル | 役割 |
| --- | --- |
| `IPStoreService.h/.mm` | 公開 API。検索・詳細・バージョン・レビュー・DL・アカウント |
| `IPModels.mm` | C++ のモデルと `IPStoreApp` などの変換 |
| `IPInternal.h` | 内部宣言（SAP ファクトリ、トランスポート、ライブラリパス） |
| `IPHTTPTransport.mm` | `NSURLSession` ベースの `HttpTransport` |
| `IPSAPBackend.mm` | CoreFP/CommerceKit を `dlopen` して SAP 署名 |
| `IPDeviceIdentity.mm` | 6 バイトの端末識別子（Keychain 保存） |
| `IPKeychain.mm` | アカウント・トークン・鍵の Keychain アクセス |
| `IPInstallServer.h/.mm` | TLS リスナー、証明書、OTA 配信 |

### Swift への公開規約

- 完了ハンドラは必ずメインキューで呼びます（`onMain` ヘルパ）。
- エラーは `NSError`（`IPStoreErrorDomain` + `IPStoreErrorCode`）に変換します。
- `NS_SWIFT_NAME` で Swift らしい名前を付け、`Scripts/check_bridge_api.py`
  が Swift 側の呼び出しと突き合わせます。
- nullability は `NS_ASSUME_NONNULL_BEGIN` で明示。

### SAP 署名

`sap.hpp` の `SapBackend` は 5 つの関数（Initialize / Exchange / Sign /
Teardown / dispose）を持つ抽象です。iOS 実装は `IPSAPBackend.mm` が
CoreFP・CommerceKit・CommerceCore をロードして呼び出します。

- CoreFP が無い環境（Simulator）では `IPCreateSAPBackend()` が
  `kSignerUnavailable` を返し、**未署名リクエストは絶対に送りません**。
- 署名対象はリクエストボディそのもので、ヘッダは
  `X-Apple-ActionSignature: base64(signature)` です。

---

## 3. SwiftUI（`IpaStore/Sources/`）

| ファイル | 内容 |
| --- | --- |
| `StoreTypes.swift` | `StoreApp`, `StoreAccount`, `StoreVersion`, `LibraryItem` などの値型 |
| `StoreService.swift` | ObjC ブリッジを async/await に変換 |
| `AppModel.swift` | `@Observable` な単一モデル。検索・詳細・DL・ライブラリ・設定 |
| `InstallService.swift` | インストールサーバーのライフサイクルとフェーズ |
| `Components.swift` | 共通部品（アートワーク、星、スクリーンショット、空状態） |
| `SearchView.swift` | 検索、フィルタ、ストアフロント、履歴、ウィッシュリスト |
| `AppDetailView.swift` | 詳細、評価、スクリーンショット、レビュー、バージョン |
| `DownloadsView.swift` | 進行中・完了したダウンロード |
| `LibraryView.swift` | アプリ単位のグルーピング、パッケージ検査 |
| `WishlistView.swift` | 保存したアプリ |
| `InstallSheet.swift` | 証明書・インストールの手順表示 |
| `AccountsView.swift` | アカウント一覧、サインイン、2FA |
| `SettingsView.swift` | 設定、証明書ガイド、謝辞 |
| `RootView.swift` | 5 つのタブ |
| `IpaStoreApp.swift` | エントリポイント、バックグラウンド遷移 |

### 状態管理

- `AppModel` は `@MainActor @Observable`。View は
  `@Environment(AppModel.self)` で受け取ります。
- 永続化は UserDefaults（地域・プラットフォーム・検索履歴・ウィッシュリスト）
  と Keychain（アカウント）、ファイル（IPA と sidecar）に分かれます。
- ダウンロードは `DownloadItem` が進捗・速度・残り時間を保持し、
  `AppModel` が配列で管理します。キャンセルはブリッジの
  `cancelDownloads()` に委譲します。

### インストール

`InstallService` は `IPInstallServer` を生成し、証明書と URL を公開します。
`InstallSheet` は

1. 証明書の SHA-256 フィンガープリント
2. 平文 HTTP の証明書ページ（Safari で開く）
3. 「設定 → 一般 → 情報 → 証明書信頼設定」での有効化
4. `itms-services` URL を開く

という順序を、現在のフェーズ（`.ready` → `.sending` → `.completed`）に
合わせて表示します。バックグラウンドに移る瞬間に
`beginBackgroundAssertion()` を呼び、iOS がペイロードを取得し終えるまで
プロセスを生かします。

---

## エラー処理

| コア | ブリッジ | 意味 |
| --- | --- | --- |
| `ErrorCode::kNetwork` | `IPStoreErrorNetwork` | 接続・TLS・タイムアウト |
| `kHttpStatus` | `IPStoreErrorHTTPStatus` | 4xx/5xx（`http_status` 付き） |
| `kUnauthorized` | `IPStoreErrorUnauthorized` | 認証失敗 |
| `kTwoFactorRequired` | `IPStoreErrorTwoFactorRequired` | 2FA コード要求 |
| `kLicenseRequired` | `IPStoreErrorLicenseRequired` | 未購入（有料アプリ） |
| `kSignerUnavailable` | `IPStoreErrorSignerUnavailable` | CoreFP 不在（Simulator） |
| `kCancelled` | `IPStoreErrorCancelled` | ユーザーによるキャンセル |
| `kIntegrity` | `IPStoreErrorIntegrity` | MD5 不一致・壊れた IPA |
| `kStorage` | `IPStoreErrorStorage` | 書き込み失敗・容量不足 |

`StoreError` はコードごとに日本語/英語の説明を出し分けます
（`StoreTypes.swift`）。

---

## ファイル構成の規約

- 追加した Swift ファイルは `Scripts/generate_xcodeproj.py` を再実行すると
  自動でターゲットに入ります（glob）。
- C++ ユニットを追加したら `Scripts/build_core.sh` の `SOURCES` と
  `generate_xcodeproj.py` の glob の両方に反映されます（どちらも glob）。
- `reference/` はビルド対象外です。生成スクリプトは `IpaStore/` と `Core/`
  だけを走査します。
