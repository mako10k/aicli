# execute tool ドキュメント

本書は `aicli` の tool/function-call として提供される **`execute`** の仕様をまとめたものです。

`execute` は **シェルを起動しません**。入力は「限定 DSL（パイプライン）」として解釈され、
許可されたコマンド/引数のみが **メモリ上で** 適用されます。
そのため、リダイレクトやサブシェル等は使用できず、ファイルも allowlist に含まれるものだけが読み取り可能です。

- 実装根拠: `include/execute_dsl.h`, `src/execute_dsl.c`, `src/execute/run_from_file.c`, `src/execute/dispatch.c`

## 目的と安全性モデル

- **Read-only**: 実行対象は allowlist 登録済みファイルの「読み取り」のみです。書き込み・削除・ネットワークアクセスはありません。
- **No shell**: `;` `&` `>` `<` `$` `` ` `` `\\` などのシェルメタ文字は DSL パーサで拒否されます（`|` は許可）。
- **Allowlist 必須**: 実ファイルパスは `realpath()` で正規化後、allowlist と完全一致する必要があります。

## 入力（DSL）概要

`execute` の `command` は以下の形の **パイプライン**を受け付けます。

- 形式: `STAGE ("|" STAGE)*`
- ステージ数上限: **8** (`aicli_dsl_pipeline_t.stages[8]`)
- 1ステージの引数上限: **8** (`aicli_dsl_stage_t.argv[8]`)
- クォート: シングル/ダブルクォートの **最小限サポート**（空白を含む 1 トークン化目的）

クォートの扱い（POSIXに近い挙動）:

- シングルクォート `'...'`: 内容は基本そのまま（`\\` もリテラル）
- ダブルクォート `"..."`: `\\X` を `X` として扱う最小限のエスケープをサポート
  - ただし `$` と `` ` `` は引用内でも禁止（展開/置換を防止）

### 禁止文字

以下の文字を含む場合、パース時点で `forbidden` になります（`|` は例外）。

- `;` `&` `>` `<` `$` `` ` `` 改行（`\n`/`\r`）

補足:

- **バックスラッシュエスケープ**: 現在は POSIX に近づけるため、トークン内で `\\X` を `X` として扱います（引用内/外）。
  - ただし `$` と `` ` `` は引用内でも禁止（変数展開/コマンド置換を防止）です。
- **`--`**: 多くのコマンドと同様に「オプション終了」として扱い、単にトークン列から取り除きます。

## 使えるコマンド種別（ステージ）

DSL 内で許可されるコマンド（`aicli_cmd_kind_t`）は次の通りです。

- `cat`
- `nl`
- `head`
- `tail`
- `wc`
- `sort`
- `grep`
- `sed`

上記以外（例: `awk`, `cut`, `tr`, `rg`, `find` など）は `forbidden` になります。

## パイプライン形状の制約（ファイル入力）

実行器は「**最初に allowlist ファイルを読み込む**」形に正規化したうえで処理します。
許可される入口は次のいずれかです。

1. 先頭が `cat <FILE>`
2. 先頭が `head ... <FILE>` / `tail ... <FILE>` / `nl ... <FILE>` / `sed -n <SCRIPT> <FILE>` のような「末尾引数が FILE」の形
   - この場合、内部で `cat <FILE>` が先頭に挿入され、元コマンドから FILE 引数が取り除かれます。

それ以外（例: `grep PATTERN <FILE>`、`sort <FILE>`、`cat` 以外から始まるが FILE を取らない等）は

- `mvp_requires: cat <FILE> (or head/tail/nl/sed ... <FILE>)`

として拒否され、`exit_code=2` になります。

## 各コマンドの仕様（引数）

以下は `src/execute/pipeline_stages.c` の実装に基づく仕様です。

### `cat`

- 形式: `cat FILE`
- 役割: allowlist 上のファイルを読み込む入口（パイプライン先頭に必要）

### `nl`

- 形式: `nl`
- 役割: 行番号付与（`%6lu\t` 形式）

### `head`

- 形式: `head` / `head -n N` / `head -nN`
- 既定: `N=10`

### `tail`

- 形式: `tail` / `tail -n N` / `tail -nN`
- 既定: `N=10`

### `wc`

- 形式: `wc -l` または `wc -c`
- `-l`: 改行数
- `-c`: バイト数（入力長）

### `sort`

- 形式: `sort` または `sort -r`
- 並び: 行単位・辞書順（`memcmp` ベース、ロケール非対応）
- 出力: 入力が 1 行以上なら **必ず末尾改行を付与**します

### `grep`

- 形式: `grep PATTERN` または `grep -n PATTERN`
- マッチ: **固定文字列**（正規表現ではない、`memcmp` の部分一致）
- `-n`: `line_no:` プレフィクスを付与

### `sed`

- 形式: `sed -n SCRIPT`
- `SCRIPT` の許可形:
  - `Np` / `Nd`
  - `N,Mp` / `N,Md`
  - ここで `N`, `M` は 1 始まり整数、`N<=M`
- 意味:
  - `p`: 範囲内行を出力
  - `d`: 範囲内行を除外して出力

注意: `sed` は「`-n` とアドレススクリプト」のみ対応です。置換（`s///`）等は不可です。

## 出力（ページング）

`execute` は全出力をそのまま返すのではなく、`start`/`size` でページングします。

- `start`: 0-based のバイトオフセット
- `size`: 返す最大バイト数
- 上限: `size` は最大でも `AICLI_MAX_TOOL_BYTES`（現在 **4096**）
- 返却:
  - `stdout_text`: 切り出した内容（NUL 終端）
  - `stdout_len`: 返したバイト数
  - `total_bytes`: 全体のバイト数
  - `truncated`: まだ続きがある場合 true
  - `has_next_start` / `next_start`: 続き取得用

## ファイルサイズ制限（MVP）

内部実装は現時点で **ファイル全体をまとめてメモリに読み込み**ます。

- 読み込み最大: **1 MiB**
- これを超えるファイルは `file_too_large`（`exit_code=4`）

## 代表的な利用例

### 例1: allowlist ファイルを先頭 200 行だけ読む

- DSL:
  - `cat path/to/file | head -n 200`

### 例2: 行番号を付けて "TODO" を探す

- DSL:
  - `cat path/to/file | nl | grep -n TODO`

### 例3: 1〜200行だけを抽出（sed）

- DSL:
  - `cat path/to/file | sed -n '1,200p'`

### 例4: モデルが出しがちな形（正規化される）

次のような形は内部で `cat FILE | head -n 20` に正規化されます。

- `head -n 20 FILE`

同様に、次も正規化されます。

- `sed -n 1,200p FILE` → `cat FILE | sed -n 1,200p`

## エラーと `exit_code`

`execute` は多くの失敗を「ツール実行自体は成功（戻り値 0）だが、`out->exit_code` を非0にする」形で返します。

- `parse_error` / `empty` / `forbidden` / `too_many_stages` / `too_many_args`
  - `exit_code=2`
- `mvp_requires: cat <FILE> ...`
  - `exit_code=2`
- `mvp_unsupported_stage`
  - `exit_code=2`
- `invalid_path`
  - `exit_code=2`
- `file_not_allowed`
  - `exit_code=3`
- `file_too_large`
  - `exit_code=4`
- `oom` / `strerror(errno)` など
  - `exit_code=1`

## デバッグ（任意）

環境変数 `AICLI_DEBUG_FUNCTION_CALL` が設定されていると、以下のようなデバッグログが stderr に出ることがあります。

- DSL パース結果
- allowlist 判定の realpath

（CLI の `--debug-function-call` は内部的にこの環境変数を使う想定です）
