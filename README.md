# aicli

軽量 Native OpenAI Client (C) + 限定ツール `execute` + Web検索（デフォルト: Google Custom Search JSON API）。

## 目的
- 単一バイナリ志向の軽量CLI
- 環境を極力汚さない（設定は環境変数中心、状態はデフォルト不保持）
- Function Calling により、必要時だけ `execute`（読取専用）でファイル参照/簡易テキスト処理
- Web検索は provider 切替（デフォルト: Google Programmable Search Engine / Custom Search JSON API）

## 依存
- `libcurl`（HTTPS）
- `yyjson`（JSON）

Debian/Ubuntu 例:

```bash
sudo apt-get update
sudo apt-get install -y build-essential pkg-config libcurl4-openssl-dev
# yyjson は各自で導入（/usr/include/yyjson.h などに置く）
```

## ビルド

```bash
make
```

## 実行（例）

### OpenAI

```bash
export OPENAI_API_KEY=... 
export AICLI_OPENAI_API_KEY=... # (代替) OPENAI_API_KEY の代わりに使える
export AICLI_MODEL=gpt-4.1-mini
./src/aicli chat "hello"
```

### Web検索（デフォルト: Google Custom Search JSON API）

## 設定

設定ファイル名は `.aicli.json` です。優先順位は「上ほど強い」です。

1. コマンドラインオプション
2. 環境変数
3. カレントディレクトリの設定（ただし `$HOME` 配下の場合のみ）
4. ホーム配下のカレント上位ディレクトリの設定（再帰的に `$HOME` まで）
5. `$HOME` の設定

`aicli run` では以下のオプションも使えます。

- `--config PATH`: 設定ファイルを明示指定
- `--no-config`: 設定ファイル探索を無効化

例:

```json
{
	"openai_api_key": "...",
	"model": "gpt-5-mini",
	"openai_base_url": "https://api.openai.com",
	"search_provider": "google_cse",
	"google_api_key": "...",
	"google_cse_cx": "..."
}
```

```bash
export GOOGLE_API_KEY=...
export GOOGLE_CSE_CX=...
./src/aicli web search "OpenAI native client C" --count 5 --lang ja --freshness week

# Brave に切り替える場合:
export AICLI_SEARCH_PROVIDER=brave
export BRAVE_API_KEY=...
./src/aicli web search "OpenAI native client C" --count 5 --lang ja --freshness week
```
```

### run（自動検索 + ファイル許可）

```bash
export OPENAI_API_KEY=...
export GOOGLE_API_KEY=...
export GOOGLE_CSE_CX=...
./src/aicli run --auto-search --file README.md "このリポジトリの要点をまとめて"
```

## ドキュメント
- 設計: `docs/design.md`
- `execute` 詳細: `docs/execute.md`

## 注意
- `execute` は read-only。ファイル変更/外部コマンド実行/リダイレクトは禁止。
- 読み込み可能ファイルは `--file` で指定したもののみ。

## ツール（Function Calling）

### `list_allowed_files`

`execute` で読み込み可能なファイル（allowlist）を **一覧** する read-only ツールです。
説明用/探索用で、ファイル内容の取得はしません。`execute` 可能なパスを知りたいときに使います。

引数:
- `query`（任意）: full path に対する case-insensitive 部分一致フィルタ
- `start`（任意）: 0-based の開始インデックス（ページング）
- `size`（任意）: 返す最大件数（1〜200、デフォルト 50）

戻り値（`output` 内 JSON）:
- `total`: フィルタ後の総件数
- `returned`: 今回返した件数
- `has_next` / `next_start`: 次ページがある場合の開始位置
- `files[]`: `path`, `name`, `size_bytes`

例（概念的）:
- 先頭 50 件: `{"query":"","start":0,"size":50}`
- README を含むパスだけ: `{"query":"README","start":0,"size":50}`

### `execute`

allowlist に含まれるローカルファイルを、限定DSLで read-only 参照します。
まず `list_allowed_files` で対象パスを把握してから `execute` するのが安全です。

## stdin（`run` / `_exec`）

`run` と `_exec` は `--stdin` または `--file -` を指定すると標準入力を読み込み、内部で一時ファイル化して allowlist に追加します（read-only 方針維持）。
