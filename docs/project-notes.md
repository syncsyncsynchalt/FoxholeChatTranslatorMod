## Foxhole Chat Translator
- C++ DLLインジェクション (version.dll プロキシ) でUE4 ProcessEventをフック
- 場所: `C:\Program Files (x86)\Steam\steamapps\common\Foxhole\Mods\ChatTranslator\`
- ビルド全体: `cmake --build build --config Release`
- ワーカーのみ: `cmake --build build --config Release --target chat_translator`
- CMake: `"C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"`
- MSVC `/utf-8` フラグ必須
- 3段階: (1)チャットログ取得 ✅ (2)Ollama翻訳(gemma3:4b) (3)翻訳結果のゲーム内表示
- Stage 1完了・整理済み: チャットキャプチャ成功、chat_log.txtに全チャンネル出力
- 現在: Stage 2 Ollama翻訳実装へ
