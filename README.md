# Foxhole Chat Translator

Foxholeのゲーム内チャットをリアルタイムで日本語に翻訳し、原文を音声で読み上げる雰囲気Mod。

英語・ロシア語・韓国語・中国語のチャットが自動的に日本語に翻訳されて画面に表示され、同時に音声で読み上げられます。翻訳はローカルAI（Ollama）で処理するため、インターネット接続やAPIキーは不要です。

![動作イメージ](Mods/ChatTranslator/docs/demo.png)

> **注意**: DLLインジェクション方式のため EAC BAN のリスクがあります。自己責任でご利用ください。

## できること

- ゲーム内チャット（全チャンネル）をリアルタイムで自動翻訳
- 翻訳結果を画面右下にオーバーレイ表示（原文＋翻訳の2行）
- チャットメッセージを音声で読み上げ（送信者ごとに異なる声）
- ラジオアイコンのクリックでON/OFF切替
- 翻訳AIの異常検知と自動復旧

## インストール

### 1. ファイルを配置

以下のファイルを `Foxhole\War\Binaries\Win64\` にコピーしてください：

```
War\Binaries\Win64\
  ├── version.dll          ← Modローダー
  ├── chat_translator.dll  ← 翻訳エンジン
  ├── config.ini           ← 設定ファイル
  ├── assets\
  │     ├── NotoSansCJKjp-Regular.otf  ← フォント
  │     ├── radio_on.wav               ← サウンド
  │     └── radio_off.wav              ← サウンド
  └── tools\
        └── ollama\         ← 翻訳AIエンジン（同梱済み）
```

### 2. 音声読み上げの準備（任意）

音声読み上げを使うには、Windows の音声パックをインストールします。管理者権限の PowerShell で実行してください：

```powershell
Add-WindowsCapability -Online -Name "Language.Speech~~~en-US~0.0.1.0"
Add-WindowsCapability -Online -Name "Language.Speech~~~zh-CN~0.0.1.0"
Add-WindowsCapability -Online -Name "Language.TextToSpeech~~~ru-RU~0.0.1.0"
Add-WindowsCapability -Online -Name "Language.TextToSpeech~~~ko-KR~0.0.1.0"
```

日本語音声は Windows 日本語版なら標準搭載です。この手順をスキップしても翻訳テキスト表示は動作します。

### 3. ゲームを起動

通常通りFoxholeを起動するだけで自動的に有効になります。初回はAIモデル（約3GB）のダウンロードが行われるため、翻訳が始まるまで数分かかります。

## 使い方

- **画面右下のラジオアイコン**をクリックするとON/OFFが切り替わります
- ON：チャットが翻訳されて表示＋読み上げ
- OFF（半透明）：翻訳停止
- 赤色：翻訳AIに異常発生 → クリックで再起動

## アンインストール

`War\Binaries\Win64\` から以下を削除するだけです：
- version.dll
- chat_translator.dll
- config.ini
- assets フォルダ
- tools フォルダ
