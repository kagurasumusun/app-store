# テスト

IPAShelf のテストは 3 層 + 成果物検証に分かれています。

| 層 | コマンド | 内容 |
| --- | --- | --- |
| コア | `bash Scripts/build_core.sh` | C++ の 627 チェック + 外部ツールによる検証 |
| 静的 | `bash Scripts/check_static.sh` | Swift 構文、Objective-C ブリッジ API、pbxproj、括弧 |
| ビルド | `bash Scripts/build_app.sh --device --configuration Release` | iOS 27 SDK での実機 arm64 ビルド |
| 成果物 | `bash Scripts/package_ipa.sh` | 実機用 .ipa の生成（`verify_ipa.py` で再検証） |
| 手動 | `docs/testing.md` のチェックリスト | 実機でのサインイン・ダウンロード・インストール |

```bash
bash Scripts/build_core.sh
bash Scripts/check_static.sh
bash Scripts/build_app.sh --device --configuration Release
bash Scripts/package_ipa.sh
```

成果物の検査だけを単体で実行する場合:

```bash
python3 Scripts/verify_ipa.py --self-test                 # 検証器の自己テスト
python3 Scripts/verify_ipa.py build/artifacts/IpaStore-0.1.0.ipa
```

サニタイザ付き:

```bash
SANITIZE=1 bash Scripts/build_core.sh
```

`build_core.sh` は `-std=c++20 -Wall -Wextra -Werror` でコンパイルし、
`build/core-tests/` に実行ファイルとフィクスチャを生成します。

---

## 1. コアのテスト（627 チェック）

### エンコードとデータ形式

| テスト | 検証内容 |
| --- | --- |
| `test_crypto` | MD5 / SHA-1 / SHA-256 の既知ベクタ、Base64 の往復、乱数の長さ |
| `test_json` | ネスト、エスケープ、Unicode、数値の原文保持、不正入力の拒否 |
| `test_plist_xml` | XML plist の生成・再パース、特殊文字のエスケープ、型の保持 |
| `test_plist_binary` | バイナリ plist の読み込み（`sample.bplist`） |
| `test_plist_binary_write` | バイナリ plist の生成（Python `plistlib` で再検証） |
| `test_url` | パーセントエンコード、クエリの順序、相対 URL、不正 URL |
| `test_account_json` | Keychain に保存するアカウント JSON の往復 |

### プロトコル

| テスト | 検証内容 |
| --- | --- |
| `test_bag` | `urlBag` の解析、安全でない URL の拒否、SAP パラメータ |
| `test_login_success` | ヘッダ署名、ボディ、`dsPersonId`/`passwordToken` の取り出し |
| `test_login_two_factor` | `-5000` で 2FA 要求、コード付き再送 |
| `test_login_bad_password` | `BadLogin` のマッピング、リトライ回数 |
| `test_search_and_lookup` | 検索 URL のパラメータ順、`lookup` の Bundle ID 解決 |
| `test_latest_external_version` | カタログ API の `externalVersionId`、Vision の `appExtVrsId` |
| `test_catalog_throttling` | カタログ 429/503 の `Retry-After` 順守リトライ、プロセス全体の結果キャッシュ（10 分）、`kTemporarilyUnavailable` 分類 |
| `test_download_unpinned_when_catalog_throttled` | カタログが制限中でもピンを断念してダウンロード／再ダウンロードを続行するフォールバック |
| `test_download_offer` | `volumeStoreDownloadProduct` の解析 |
| `test_download_fallback` | `/r/redownload` と `/up/updateProduct` のフォールバック |
| `test_purchase` | 無料アプリの購入、`pricingParameters` |
| `test_purchase_license_exists` | `204` を成功として扱う |
| `test_version_identifiers` | `softwareVersionExternalIdentifiers` の抽出 |
| `test_download_parsing` | `songList[0]` の URL・sinf・メタデータ |
| `test_map_store_failure` | `failureType` / `customerMessage` の変換 |
| `test_storefronts` | 50 地域、`-1,29` ヘッダ、コードの一意性 |
| `test_app_details` | スクリーンショット（iPhone/iPad）、評価、ジャンル、サイズ |
| `test_search_filters` | 無料のみ、ジャンル、並び替え |

### IPA

| テスト | 検証内容 |
| --- | --- |
| `test_ipa_patch` | miniz で ZIP を開き、`iTunesMetadata.plist`・`iTunesArtwork`・`SC_Info/*.sinf` を確認。生成物を Python の `zipfile` で再検証 |

### OTA と HTTP

| テスト | 検証内容 |
| --- | --- |
| `test_ota_manifest` | マニフェストの構造、XML エスケープ、`itms-services` URL のエンコード |
| `test_http_parser` | リクエスト行、ヘッダの大文字小文字、Content-Length、上限超過の拒否 |
| `test_ranges` | 単一 Range、末尾指定、範囲外、複数範囲 |
| `test_ota_router` | `/`、`/manifest.plist`、`/icon.png`、`/ipa`、`/ca.cer`、404、Host 不一致の 421 |
| `test_http_server` | ループバックに実サーバーを立て、Range 付き GET を実際に送受信 |

### サニタイザで見つかった不具合

`SANITIZE=1` の実行で、`Url::parse()` がポート番号の解析に
`std::strtol(std::string(port).c_str(), &end, 10)` を使い、
`*end` を**一時文字列の破棄後**に読んでいた問題
（`stack-use-after-scope`）を検出しました。現在は桁ごとに検証する
`parse_port()` に置き換え、`0`・65536 以上・非数字・6 桁以上・
IPv6 のポートをテストで固定しています。

### 証明書

| テスト | 検証内容 |
| --- | --- |
| `test_x509` | CA/リーフの `basicConstraints`/`keyUsage` の厳密な DER 形、パラメータ無しの `ecdsa-with-SHA256`（RFC 5758）、SKID/AKID、リーフの SAN/EKU、P1363→DER 署名変換の KAT |

`build_core.sh` はさらに外部ツールで生成物を検証します。

| 検証 | ツール |
| --- | --- |
| XML plist / バイナリ plist | Python `plistlib` |
| IPA の ZIP 構造 | Python `zipfile` |
| OTA マニフェスト | Python `plistlib` |
| SPKI・CA・リーフ・PEM | `openssl x509` / `openssl pkey` |

---

## 2. 静的チェック（Xcode なしでできる範囲）

`bash Scripts/check_static.sh` は次を実行します。

1. **Swift 構文** — `tree-sitter-swift` で 15 ファイルを解析し、
   `ERROR` / 欠損ノードが無いことを確認。
2. **ブリッジ API** — `Scripts/check_bridge_api.py` が
   `IpaStore/Bridge/*.h` のセレクタから Swift 名を生成し、
   `IpaStore/Sources/*.swift` の呼び出し（ラベル・個数・末尾クロージャ）と
   突き合わせる。
3. **Xcode プロジェクト** — `pbxproj` でターゲットのビルドフェーズと
   ファイル参照数を検証。
4. **括弧の対応** — Swift / ObjC++ / C++ をコメント・文字列除去のうえで検査。

`check_static.sh` の出力例:

```
== Swift syntax (tree-sitter)
  15 Swift files, 0 with syntax errors
== Objective-C++ bridge API
bridge API check: 13 call sites verified, 0 mismatches
== Xcode project
  target IpaStore: {'PBXSourcesBuildPhase': 39, ...}
```

### 限界

- Swift/ObjC++ の実コンパイルは CI の `apple` ジョブ（Xcode 27）でのみ
  行われます。ローカルの静的チェックは構文とセレクタの整合までで、
  型推論やインポータ名は実ビルドでしか確定しません。
- SwiftUI のプレビューが無いため、レイアウトの回帰は目視に依存します。
- プロトコルテストはフェイクトランスポートと合成フィクスチャで
  ワイヤ形式を固定するものですが、実 Apple サーバー相手の成否は
  実機確認（`docs/device-verification.md`）でのみ最終確定します。
- SAP のソフトウェア実装自体は単体テストでも実物を使います
  （ハンドシェイクフィクスチャでバイト一致を検証）。

---

## 3. 実機での手動チェックリスト

### サインイン

- [ ] 正しい資格情報でサインインできる
- [ ] 2FA が要求され、コード入力で完了する
- [ ] パスワードを間違えるとエラーが表示され、アプリが落ちない
- [ ] アカウントを 2 つ追加し、切り替えでストアフロントが変わる
- [ ] アカウント削除後、Keychain からトークンが消える（再サインインが必要）
- [ ] Simulator では「SAP 署名が利用できません」と表示される

### 検索と詳細

- [ ] 日本語・英語の検索がヒットする
- [ ] ストアフロントを変えると結果と価格が変わる
- [ ] 無料のみ・ジャンル・並び替えが反映される
- [ ] 検索履歴が保持され、クリアできる
- [ ] 詳細でスクリーンショットが横スクロールできる（iPhone / iPad 切替）
- [ ] 評価の星と件数が表示される
- [ ] レビューが表示され、「もっと見る」で次ページが読める
- [ ] レビューが無いアプリで空表示になる（エラーにならない）

### 購入履歴

- [ ] 「購入済み」タブに所有アプリが新しい順に表示される
- [ ] プラットフォームフィルタと検索が反映される
- [ ] 配信終了したアプリも表示され、ダウンロードできる
- [ ] アカウント切り替えで一覧が入れ替わる

### ダウンロード

- [ ] 無料アプリをダウンロードできる
- [ ] 進捗・速度・残り時間が更新される
- [ ] キャンセル後・失敗後の再試行で、続きから再開する（レジューム）
- [ ] アプリを再起動してもレジュームが効く
- [ ] 完了後にライブラリへ移動し、サイズとバージョンが表示される
- [ ] 有料アプリは「購入できません」と表示される

### バージョン

- [ ] 所有済みアプリでバージョン一覧が取得できる
- [ ] 各ビルドの表示バージョンとリリース日が埋まっていく
- [ ] 最新ビルドに「Latest」バッジが付く
- [ ] 旧バージョンを指定してダウンロードできる
- [ ] 同じアプリの複数バージョンが保持される
- [ ] 個別削除・全削除ができる

### インストール

- [ ] 証明書ページが Safari で開ける
- [ ] プロファイルをインストールし、信頼を有効化できる
- [ ] `itms-services` でホーム画面に追加される
- [ ] アプリを切り替えてもインストールシートの状態が壊れない
- [ ] 証明書のフィンガープリントが設定画面と一致する

### ライブラリ

- [ ] アプリ単位でグルーピングされる
- [ ] 名前・サイズ・日付で並び替えられる
- [ ] Bundle ID・名前・バージョンで検索できる
- [ ] 共有シートで IPA を渡せる
- [ ] 詳細で sidecar のメタデータが表示される

### 設定

- [ ] 地域・プラットフォームの変更が検索に反映される
- [ ] ライブラリの合計サイズが正しい
- [ ] キャッシュ削除で再取得される
- [ ] 謝辞に miniz のライセンスが表示される
