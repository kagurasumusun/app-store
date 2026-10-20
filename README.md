# IPAShelf

iPhone / iPad 向けの、自分が購入・所有している App Store アプリを管理するための
クライアントです。App Store の検索・詳細表示・バージョン選択・ダウンロード・
端末へのインストールまでを 1 つのアプリで行います。

- プロトコル層は **C++20**、iOS 連携は **Objective-C++**、UI は **SwiftUI**
- **Go は使用していません**。`reference/` 配下のコードは一切コピーしていません
  （参照したのは公開されている仕様・挙動のみ）
- 外部依存は vendored な **miniz 3.0.2**（MIT、10 ファイル）だけです
- ターゲット SDK は **iOS 27 SDK**（Xcode 27 / 27.1）

> **重要**: Apple の非公開エンドポイントを利用します。Apple と無関係の
> 非公式アプリであり、App Store への提出・配布は想定していません。
> 自分のアカウントと自分の購入済みアプリにのみ使用してください。

---

## 主な機能

| 機能 | 内容 |
| --- | --- |
| 複数アカウント | Keychain に複数の Apple Account を保存し、ワンタップで切り替え。アカウントごとにストアフロント（国・地域）を保持 |
| ストアフロント | 50 か国の一覧から検索・切り替え。検索結果・評価・レビュー・パッケージが地域に追従 |
| 検索 | キーワード、無料/有料、カテゴリ、並び替え（関連度・名前・評価・新着）、検索履歴 |
| プラットフォーム | iPhone / iPad / Mac / Apple TV / Apple Watch / Vision Pro を切り替えて検索・ダウンロード |
| アプリ詳細 | スクリーンショット（iPhone / iPad を切り替え）、評価（星・件数）、レビュー、説明、What's New、サイズ、対応言語、販売元、App Store リンク |
| バージョン管理 | 所有済みアプリのビルド一覧（Apple が返す external version ID）、最新版/旧版の指定ダウンロード、端末内の複数バージョン保持・個別削除・全削除 |
| ダウンロード | 進捗・速度・残り時間、キャンセル、再試行、完了後の自動検証（MD5・Bundle ID）と IPA パッチ |
| ライブラリ | アプリ単位でグルーピング、バージョンごとの一覧、検索、並び替え、合計サイズ、共有、詳細（sidecar メタデータ） |
| インストール | 端末内でルート CA とリーフ証明書を生成し、HTTPS で OTA マニフェストと IPA を配信。`itms-services` で iOS にインストールさせる |
| ウィッシュリスト | 気になるアプリを保存し、検索画面と設定に表示 |
| 設定 | アカウント、ストアフロント、プラットフォーム、証明書ガイド、ライブラリ情報、診断情報、謝辞 |

---

## アーキテクチャ

```
┌──────────────────────────────────────────────────────────────┐
│ SwiftUI (IpaStore/Sources)                                   │
│  Search / Detail / Versions / Downloads / Library / Install   │
└───────────────▲──────────────────────────────────────────────┘
                │ Objective-C facade (NS_SWIFT_NAME)
┌───────────────┴──────────────────────────────────────────────┐
│ Bridge (IpaStore/Bridge, Objective-C++)                      │
│  IPStoreService  : ログイン / 検索 / 詳細 / バージョン / DL    │
│  IPInstallServer : TLS リスナー + 証明書生成 + OTA 配信        │
│  IPHTTPTransport : NSURLSession                              │
│  IPSAPBackend    : CoreFP / CommerceKit の SAP 呼び出し        │
│  IPKeychain      : アカウント・鍵の保存                        │
└───────────────▲──────────────────────────────────────────────┘
                │ ポータブル C++ API
┌───────────────┴──────────────────────────────────────────────┐
│ Core (Core/include/ipashelf, Core/src)                       │
│  store_client : 認証・購入・ダウンロード・バージョン・レビュー  │
│  bag/sap      : SAP セットアップと署名                         │
│  plist/json   : XML/バイナリ plist、JSON DOM                   │
│  ipa          : IPA 検査・iTunesMetadata/sinf 注入             │
│  ota          : OTA マニフェスト・HTTP パーサ・Range 配信       │
│  x509         : ECDSA P-256 証明書の DER 生成                  │
│  crypto/url   : MD5/SHA-256/Base64、URL エンコード             │
└──────────────────────────────────────────────────────────────┘
```

詳細は [`docs/architecture.md`](docs/architecture.md)、プロトコルは
[`docs/protocol.md`](docs/protocol.md) を参照してください。

---

## 動作要件

- macOS Tahoe 26.6 以降（Xcode 27 系の要件）
- **Xcode 27.1 以降**（iOS 27.1 SDK）
- iOS 27.0 以降を搭載した実機 iPhone / iPad
  - サインインは Apple の SAP プリミティブ（CoreFP 等）を利用するため、
    **Simulator では動作しません**（CoreFP が実機にのみ存在するため）

プロジェクトは `Scripts/generate_xcodeproj.py` で再生成できます。

---

## ビルド

```bash
# 1. ポータブルコアのビルドとテスト（macOS / Linux どちらでも）
bash Scripts/build_core.sh

# 2. Xcode プロジェクトの再生成（ファイル追加後に実行）
python3 Scripts/generate_xcodeproj.py

# 3. Xcode で開く
open IpaStore.xcodeproj
```

`build_core.sh` の期待される出力:

```
PASS: 362/362 checks
python plistlib validation: ok
python IPA validation: ok
python OTA manifest validation: ok
openssl SPKI validation: ok
openssl CA certificate validation: ok
openssl leaf certificate validation: ok
openssl PEM validation: ok
```

### ビルド検証状況

このリポジトリは次の環境で検証済みです。

| 対象 | 検証方法 | 結果 |
| --- | --- | --- |
| C++ コア | `g++ 12 -std=c++20 -Wall -Wextra -Werror` + ASan/UBSan | ✅ 362 チェック |
| plist / IPA | Python `plistlib` による再パース | ✅ |
| OTA マニフェスト | `plistlib` で構造検証 | ✅ |
| X.509 / SPKI | `openssl x509` / `openssl pkey` で DER を解析 | ✅ |
| Xcode プロジェクト | `pbxproj` ライブラリでパース + 参照解決 | ✅ 71 参照すべて解決 |
| Swift 16 ファイル | tree-sitter-swift による構文解析 | ✅ エラー 0 |
| ブリッジ API | Swift の呼び出しと ObjC セレクタの照合 | ✅ 16 箇所一致 |
| ObjC++ 7 ファイル | Xcode 27（iOS 27 SDK）で実コンパイル | ✅ |
| シミュレータ | 起動 + アプリ内スモークテスト | ✅ 14/14 チェック |

GitHub Actions の `apple` ジョブは macOS 27 / Xcode 27 上で次を実行します。

1. `Scripts/build_app.sh --simulator`（Debug）
2. `Scripts/build_app.sh --device`（arm64、署名なし）
3. `Scripts/build_app.sh --simulator --configuration Release`
4. バンドル検査（Info.plist、`nm` による ObjC++ シンボル、`lipo`）
5. `Scripts/smoke_test.sh`（シミュレータで起動 + アプリ内スモークテスト）

`smoke_test.sh` はアプリをシミュレータにインストールし、UI が起動した
ままであることを確認してスクリーンショットを保存したあと、
`-IPSmokeTest` 付きで再起動します。アプリ側（`SmokeTest.swift`）は
Keychain を使ったルート CA の生成、平文 HTTP と TLS のリスナー起動、
`/`・`/ca.cer`・`/install.plist`・`/payload.ipa` の取得までを実行し、
`SMOKE: PASS` を出力して終了します。

---

## インストール機能について

iOS が IPA をインストールできるのは、**信頼された証明書で HTTPS 配信された
OTA マニフェスト**を `itms-services` で開いた場合だけです。IPAShelf は:

1. 端末内で **ECDSA P-256 のルート CA とリーフ証明書**を生成（秘密鍵は Keychain、
   証明書は Application Support、バックアップ対象外）
2. `Network.framework` の TLS リスナーでマニフェスト・アイコン・IPA を配信
   （HTTP パース、Range 対応、ファイル配信は C++ コア）
3. ルート証明書だけは**平文 HTTP** で配信（まだ信頼されていないため TLS では
   取得できない）。Safari で開き、プロファイルをインストールして
   「証明書信頼設定」で有効化してもらう
4. 準備ができたら `itms-services://?action=download-manifest&url=…` を開く

手順はアプリ内の「証明書ガイド」にも記載しています。
詳細は [`docs/install.md`](docs/install.md) を参照してください。

---

## セキュリティとプライバシー

- アカウントのパスワードとトークンは **Keychain** にのみ保存
- 端末識別子（6 バイト）と署名鍵も Keychain。バックアップ対象外
- 解析・クラッシュ送信・独自サーバーへの送信は一切なし
- ネットワーク通信は Apple のエンドポイントと、端末内のインストールサーバーのみ
- `PrivacyInfo.xcprivacy` を同梱（トラッキングなし、収集データなし）
- アプリコードの復号は行いません。Sinf（FairPlay 署名）とメタデータを
  パッケージに注入するだけです

---

## 既知の制約

1. **サインインは実機のみ**。Simulator には CoreFP がありません。
2. **有料アプリの新規購入は不可**。Apple の支払いシートを第三者が表示できないため、
   無料アプリまたは購入済み（ライセンスあり）のアプリのみダウンロードできます。
3. **旧バージョンの表示名は不明**。Apple は external version ID の一覧のみ返すため、
   旧ビルドは「Build 123456789」と表示されます。
4. **インストールには証明書の信頼が必要**。初回のみ手動設定が必要です。
5. **LAN の IP が変わると証明書の SAN と一致しなくなる**ことがあります。
   その場合はインストールサーバーを再起動してください（証明書を再生成します）。
6. **App Review 上のリスク**。CoreFP / CommerceKit / AppleMediaServices などの
   非公開フレームワークを利用するため、App Store 審査を通過する保証はありません。
   自己利用・社内配布向けです。
7. iOS 27 SDK が必須です（Xcode 27.1 以降）。deployment target は iOS 27.0 です。

---

## 参考情報とコード非流用の宣言

- `reference/ipatool`（Go）と `reference/Asspp`（SwiftUI）は **情報源としてのみ**
  参照しました。ソースコードのコピー・翻訳・移植は行っていません。
- 実装は、Apple が公開している仕様（OTA マニフェスト、`itms-services`、
  X.509、HTTP/1.1、Property List、iTunes Search API、RSS レビューフィード）と、
  観測されたリクエスト/レスポンス形式に基づく独立実装です。
- 証明書は実行時に端末内で生成します。参照アプリに同梱されていた証明書や
  秘密鍵は使用していません。
- miniz のみ MIT ライセンスの外部コードとして vendoring しています
  （`ThirdParty/miniz`, 謝辞画面に全文掲載）。

---

## ディレクトリ構成

```
Core/                 ポータブル C++20 コア + テスト
  include/ipashelf/   公開ヘッダ
  src/                実装（13 ユニット）
  tests/              フェイクトランスポートによる 362 チェック
IpaStore/
  Bridge/             Objective-C++ ブリッジ（サービス / インストール / SAP / Keychain）
  Sources/            SwiftUI 15 ファイル
  Resources/          アプリアイコン、インストール用アイコン、PrivacyInfo
  IpaStore-Bridging-Header.h
Config/               Info.plist / entitlements
Scripts/              build_core.sh / generate_xcodeproj.py / check_static.sh
ThirdParty/miniz/     vendored miniz 3.0.2
docs/                 アーキテクチャ・プロトコル・テスト・インストール手順
reference/            参照用リポジトリ（ビルド対象外・コピー禁止）
```

---

## テスト

```bash
bash Scripts/build_core.sh            # 362 チェック + plistlib + openssl
bash Scripts/check_static.sh          # Swift 構文 + ブリッジ + pbxproj
SANITIZE=1 bash Scripts/build_core.sh # ASan/UBSan 付き
bash Scripts/check_static.sh          # Swift 構文・ブリッジ API・pbxproj
```

テスト内容の一覧は [`docs/testing.md`](docs/testing.md) を参照してください。

---

## 謝辞

- [miniz](https://github.com/richgel999/miniz) — MIT License
- Apple の公開ドキュメント（Property List, X.509, HTTP, OTA インストール）

本プロジェクトは Apple Inc. と提携・関連していません。
