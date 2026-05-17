# ChatTranslator — Claude 向け開発ガイド

## プロジェクト概要

Foxhole ゲームのチャットを自動翻訳し、音声合成(TTS)で読み上げる DLL Mod。
- **翻訳エンジン**: Ollama ローカル LLM
- **TTS エンジン**: Sherpa-ONNX (C API を DLL 経由で動的ロード)
- **再生**: XAudio2

---

## ビルド

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

VSCode タスク: `Ctrl+Shift+B` → "Build & Deploy (Release)"

---

## TTS セットアップ

```powershell
powershell -ExecutionPolicy Bypass -File .\setup_tts.ps1
```

インストール先: `../../War/Binaries/Win64/tools/tts/`

---

## TTS テストツール

```powershell
powershell -ExecutionPolicy Bypass -File .\tools\tts_test.ps1
```

または VSCode タスク: "Run TTS Test Tool"

`tts_test.ps1` は Python 3.12 の自動選択と不足パッケージの自動インストールを行う。

---

## Sherpa-ONNX に関する重要な知見

### バージョン v1.13.x の ABI 変更 (v1.10.x との非互換)

v1.13.x では `SherpaOnnxOfflineTtsModelConfig` に多数の新モデル用フィールドが追加された。
C++ 側で古い定義を使うとメモリレイアウトがずれてクラッシュする。

**必ず全構造体を定義すること** (src/tts.cpp 参照):
- `SherpaOnnxOfflineTtsMatchaModelConfig`
- `SherpaOnnxOfflineTtsKokoroModelConfig`
- `SherpaOnnxOfflineTtsKittenModelConfig`
- `SherpaOnnxOfflineTtsZipvoiceModelConfig`
- `SherpaOnnxOfflineTtsPocketModelConfig`
- `SherpaOnnxOfflineTtsSupertonicModelConfig`

また `SherpaOnnxGeneratedAudio` のサンプル数フィールドが `num_samples` → `n` に改名された。

### DLL 名の変化

リリースパッケージによって DLL 名が異なる:
- `sherpa-onnx.dll` (旧パッケージ)
- `sherpa-onnx-c-api.dll` (新パッケージ)

`tts.cpp` では両方を順番に試す実装にしている。`setup_tts.ps1` では `sherpa-onnx-c-api.dll` が存在する場合に `sherpa-onnx.dll` としてコピーもする。

### DLL パッケージの選択

GitHub Release から正しいパッケージを選ぶ条件:
- `win-x64-shared` を含む
- `MD-Release` を含む
- `-lib.` を含まない (lib パッケージは DLL なし)
- `no-tts` を含まない

### 日本語 TTS

Piper 系モデルに日本語版は存在しない。**Supertonic 3** を使う:
- モデル名: `sherpa-onnx-supertonic-3-tts-int8-2026-05-11`
- 31言語対応、約123MB
- 判別方法: モデルディレクトリに `duration_predictor.int8.onnx` が存在するか否か

韓国語は `vits-mimic3-ko_KO-kss_low` を使う (Piper 系の韓国語モデルは存在しない)。

### C++/Python 共通: モデル自動判別ロジック

```
models/<lang>/duration_predictor.int8.onnx が存在する → Supertonic
models/<lang>/model.onnx + tokens.txt が存在する      → VITS (Piper/mimic3)
```

### 英語フォールバック

対象言語のモデルが未インストールの場合、英語モデルで代替する (C++/Python 両方で実装)。

---

## セットアップスクリプト (setup_tts.ps1) の注意点

- **文字コード**: PowerShell は UTF-8 BOM 必須。編集後は必ず BOM 付きで保存すること:
  ```powershell
  [System.IO.File]::WriteAllText($path, $content, [System.Text.UTF8Encoding]::new($true))
  ```
- **tar コマンド**: Git の `/usr/bin/tar` は Windows ドライブレターを解釈できない。
  必ず `$env:SystemRoot\System32\tar.exe` を使うこと:
  ```powershell
  & "$env:SystemRoot\System32\tar.exe" -xf $Archive -C $DestDir
  ```

---

## Python テストツール (tools/tts_test.py) の注意点

- 使用 Python: `%LOCALAPPDATA%\Programs\Python\Python312\python.exe`
- 必須パッケージ: `sherpa-onnx`, `sounddevice`, `numpy`
- `tts_test.ps1` ランチャーが自動インストールを行う
- テストツールのラジオエフェクトは `tts.cpp` の `ApplyRadioEffect` と同一ロジック (HPF 300Hz / LPF 3400Hz / クリッピング 0.85)
- VSCode タスクの Python パスが別 venv を向いていると動作しない → タスクは `tts_test.ps1` 経由で起動すること

---

## ファイル構成 (TTS 関連)

```
src/tts.cpp              C++ TTS 実装 (Sherpa-ONNX 動的ロード + XAudio2)
src/tts.h                公開 API (Init / Speak / Stop / Shutdown)
setup_tts.ps1            DLL + モデルのダウンロード・配置
tools/tts_test.py        GUI テストツール (Python / tkinter)
tools/tts_test.ps1       テストツール ランチャー (Python 選択 + 依存インストール)
```
