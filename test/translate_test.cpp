// ============================================================
// translate_test.cpp - Ollama 翻訳スタンドアロンテスト
// ゲーム非依存。Ollama が起動している状態で実行する。
//
// 使用方法:
//   translate_test.exe                     対話モード
//   translate_test.exe "Hello world"       単文翻訳
//   translate_test.exe --file chat_log.txt ログファイル一括翻訳
//
// オプション:
//   --endpoint URL    Ollama API (既定: http://localhost:11434/api/generate)
//   --model MODEL     モデル名    (既定: gemma3:4b)
//   --lang LANG       翻訳先言語  (既定: Japanese)
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>

#include "translate.h"
#include "log.h"

int main(int argc, char* argv[]) {
    // デフォルト設定
    translate::TranslateConfig cfg;
    std::string inputText;
    std::string inputFile;

    // 引数パース
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--endpoint") == 0 && i + 1 < argc) {
            cfg.endpoint = argv[++i];
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            cfg.model = argv[++i];
        } else if (strcmp(argv[i], "--lang") == 0 && i + 1 < argc) {
            cfg.targetLang = argv[++i];
        } else if (strcmp(argv[i], "--file") == 0 && i + 1 < argc) {
            inputFile = argv[++i];
        } else if (argv[i][0] != '-') {
            inputText = argv[i];
        }
    }

    // ログ初期化 (コンソール出力のみ)
    logging::Init(".\\", true);

    // 翻訳モジュール初期化
    if (!translate::Init(cfg)) {
        printf("Error: 翻訳モジュールの初期化に失敗しました\n");
        printf("  Ollama が起動しているか確認してください: ollama serve\n");
        logging::Shutdown();
        return 1;
    }

    SetConsoleOutputCP(CP_UTF8);

    if (!inputFile.empty()) {
        // ファイルモード: chat_log.txt の各行を翻訳
        std::ifstream file(inputFile);
        if (!file.is_open()) {
            printf("Error: ファイルを開けません: %s\n", inputFile.c_str());
            translate::Shutdown();
            logging::Shutdown();
            return 1;
        }

        printf("=== チャットログ翻訳: %s ===\n\n", inputFile.c_str());

        std::string line;
        int count = 0;
        while (std::getline(file, line)) {
            if (line.empty()) continue;

            // フォーマット: [timestamp] [channel] sender: message
            // メッセージ部分を抽出 (最初のタイムスタンプ以降の ": " を探す)
            size_t msgPos = line.find(": ", 20);
            if (msgPos == std::string::npos) continue;
            std::string msg = line.substr(msgPos + 2);
            if (msg.empty()) continue;

            printf("[原文]  %s\n", msg.c_str());
            std::string translated = translate::Sync(msg);
            if (!translated.empty()) {
                printf("[翻訳]  %s\n\n", translated.c_str());
            } else {
                printf("[翻訳]  (エラー)\n\n");
            }
            count++;
        }
        printf("=== %d 件翻訳完了 ===\n", count);

    } else if (!inputText.empty()) {
        // 単文モード
        printf("[原文]  %s\n", inputText.c_str());
        std::string translated = translate::Sync(inputText);
        if (!translated.empty()) {
            printf("[翻訳]  %s\n", translated.c_str());
        } else {
            printf("[翻訳]  (エラー)\n");
        }

    } else {
        // 対話モード
        printf("=== Foxhole Chat Translation Test ===\n");
        printf("Endpoint: %s\n", cfg.endpoint.c_str());
        printf("Model:    %s\n", cfg.model.c_str());
        printf("Target:   %s\n", cfg.targetLang.c_str());
        printf("テキストを入力してください (空行で終了):\n\n");

        char buf[1024];
        while (true) {
            printf("> ");
            if (!fgets(buf, sizeof(buf), stdin)) break;

            size_t len = strlen(buf);
            while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
                buf[--len] = 0;
            if (len == 0) break;

            std::string translated = translate::Sync(buf);
            if (!translated.empty()) {
                printf("=> %s\n\n", translated.c_str());
            } else {
                printf("=> (エラー)\n\n");
            }
        }
    }

    translate::Shutdown();
    logging::Shutdown();
    return 0;
}
