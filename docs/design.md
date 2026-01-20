# 設計（aicli）

## ゴール
- Cで実装された軽量なネイティブCLI
- OpenAI Responses API の Function Calling を利用
- 限定シェル相当のツール `execute` を内蔵（read-only + パイプ + ページング）
- Web検索は provider 切替（デフォルト: Google Programmable Search Engine / Custom Search JSON API）。`aicli web search` で明示実行も `aicli run --auto-search` で自動実行も可能
- ローカルファイル読取は **CLI引数で指定したファイルのみ**

## 非ゴール
- 任意シェル実行（禁止）
- ファイル編集/生成（禁止）
- 無制限のWebクロール（禁止）

---

## CLI仕様（案）

### `aicli chat`
- 目的: 1回のプロンプトをモデルに投げて応答を返す
- 入力: 引数文字列 または stdin
- オプション:
  - `--model` モデル
  - `--system FILE` system prompt 追加
  - `--file PATH`（複数可） 許可ファイルとして登録（この時点では読まない）

### `aicli web search`
- 目的: Web検索（providerに応じて Google CSE / Brave）を行い、整形結果を返す（jq不要）
- 例:
  - `aicli web search "query" --count 5 --lang ja --freshness week`
- 環境変数:
  - `AICLI_SEARCH_PROVIDER`（任意）: `google_cse|google|brave`（既定: `google_cse`）
  - Google CSE（既定 provider）:
    - `GOOGLE_API_KEY` 必須
    - `GOOGLE_CSE_CX` 必須
  - Brave（provider=brave の場合）:
    - `BRAVE_API_KEY` 必須

### `aicli run`
- 目的: モデル応答生成の途中で、必要に応じて `execute` を呼び出して追加情報を取得してから最終回答を作る
- `--auto-search`:
  - モデルが「検索が必要」と判断した場合のみ、検索クエリ生成→Brave検索→結果注入→回答生成

---

## 設定

優先順位: CLI引数 > 環境変数 > デフォルト

- `OPENAI_API_KEY`（必須）
  - 代替: `AICLI_OPENAI_API_KEY`
  - 代替: 設定ファイル `.aicli.json` の `openai_api_key`
- `OPENAI_BASE_URL`（任意）既定: `https://api.openai.com/v1`
- `AICLI_MODEL`（任意）既定: `gpt-4.1-mini`（例）

検索:
- `AICLI_SEARCH_PROVIDER`（任意）既定: `google_cse`
- `GOOGLE_API_KEY` / `GOOGLE_CSE_CX`（provider=google_cse）
- `BRAVE_API_KEY`（provider=brave）

状態:
- デフォルトで履歴保存なし
- 冪等キャッシュはメモリ内（プロセス生存中のみ）

---

## ツール: `execute`（限定シェル）

### 目的
- ローカル許可ファイルの読取
- 単純なテキスト処理をパイプで連結
- 結果を最大4KBに抑え、ページングを可能にする

### 入力（Function Calling Parameters）

```
execute({
  "command": "...",
  "file": "...",         // optional hint
  "idempotency": "...",  // optional key
  "start": 0,             // optional byte offset
  "size": 4096            // optional max bytes
})
```

- `command` は限定DSL。許可コマンドのみ、`|` による直列パイプのみ。
- `file` は主要対象のヒント（実装側は `command` 内のファイル参照と整合性を取る）。
- `start/size` によりページング（byte単位）。`size` 既定4096。
- `idempotency` が指定された場合、同一要求はキャッシュ利用可。

### 許可コマンド
- `cat <FILE>`: byte range 読取対応（start/sizeに従って返す）
- `nl`: 行番号付与
- `head -n N` / `tail -n N`
- `wc -l|-c`
- `sort`（単純な昇順）

禁止:
- ファイル書き込み、リダイレクト、サブシェル、任意コマンド実行

### `curl`（fetch用途、任意）
- `curl <URL>` は GET のみ
- URL は allowlist prefix に一致する場合のみ許可
- 最大取得サイズ/タイムアウト/リダイレクト回数を制限

### 出力（Tool Result）

```json
{
  "ok": true,
  "stdout_text": "...<=4096 bytes...",
  "stderr_text": "",
  "exit_code": 0,
  "total_bytes": 12345,
  "next_start": 4096,
  "truncated": true,
  "cache_hit": false
}
```

- `total_bytes` はパイプ全体の出力サイズ（可能なら正確に。難しければ推定+フラグ）
- `next_start` は次に要求すべき byte offset。終端は `null`。

### ページングの考え方
- ツールは常に `size` 以内に切り詰めて返す
- 追加が必要ならモデルが `start=next_start` を指定して再呼び出し

### 冪等キャッシュ
- キー: `idempotency + command + start + size + allowlist_fingerprint`
- LRU（小容量）

---

## Web検索: Google CSE（デフォルト）

### リクエスト
- Endpoint: `https://www.googleapis.com/customsearch/v1`
- Query:
  - `key=$GOOGLE_API_KEY`
  - `cx=$GOOGLE_CSE_CX`
  - `q=...`

---

## Web検索: Brave（任意）

### リクエスト
- Endpoint: `https://api.search.brave.com/res/v1/web/search`
- Header: `X-Subscription-Token: $BRAVE_API_KEY`

### aicli出力（整形）
- 上位N件の `title/url/snippet` を、一定文字数に圧縮して表示
- `run --auto-search` では、同形式を会話入力として注入

---

## `run --auto-search` のフロー（後者: 条件付き検索）
1. モデルに「検索が必要か」「必要なら検索クエリ」を短いJSONで返させる
2. 必要な場合のみ provider に応じて Web検索
3. `SEARCH_RESULTS:` として会話に追加
4. 最終回答生成（必要なら `execute` でファイル参照）

---

## セキュリティ境界
- 読めるファイルは `--file` 指定のみ（realpathで正規化比較）
- `execute` のDSLは最小: 許可コマンド + パイプのみ
- `curl` は allowlist prefix に限定
- 返却は最大4KB（データの過剰流出とツール連打を抑制）
