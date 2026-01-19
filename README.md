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
export AICLI_MODEL=gpt-4.1-mini
./src/aicli chat "hello"
```

### Web検索（デフォルト: Google Custom Search JSON API）

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

## 注意
- `execute` は read-only。ファイル変更/外部コマンド実行/リダイレクトは禁止。
- 読み込み可能ファイルは `--file` で指定したもののみ。
