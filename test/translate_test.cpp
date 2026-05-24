// ============================================================
// translate_test.cpp - Ollama 翻訳スタンドアロンテスト
// ゲーム非依存。Ollama が起動している状態で実行する。
//
// 使用方法:
//   translate_test.exe                     対話モード
//   translate_test.exe "Hello world"       単文翻訳
//   translate_test.exe --raw "Hello world" 翻訳結果のみ stdout に出力 (GUI 連携用)
//   translate_test.exe --file chat_log.txt ログファイル一括翻訳
//   translate_test.exe --benchmark         プリセット比較ベンチマーク
//
// オプション:
//   --endpoint URL    Ollama API (既定: http://localhost:11434/api/generate)
//   --preset PRESET   プリセット  (既定: Medium, Low/Medium/High)
//   --lang LANG       翻訳先言語  (既定: Japanese)
//   --raw             翻訳結果のみ stdout に出力、ログ非表示 (GUI 連携用)
//   --output FILE     ベンチマーク CSV 出力先 (既定: benchmark_presets.csv)
//   --runs N          ウォームラン数 (既定: 5)
// ============================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <vector>
#include <chrono>

#include "translate.h"
#include "log.h"

// wchar_t → UTF-8 変換
static std::string WideToUtf8(const wchar_t* w) {
    if (!w || !*w) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string s(len - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], len, nullptr, nullptr);
    return s;
}

// CSV エスケープ (ダブルクォート内のダブルクォートを "" に)
static std::string CsvEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    out += '"';
    for (char c : s) {
        if (c == '"') out += '"';
        out += c;
    }
    out += '"';
    return out;
}

// ============================================================
// ベンチマーク定義
// ============================================================

struct TestSentence {
    const char* label;
    const char* lang;
    const char* text;
};

static const TestSentence kTestSentences[] = {
    { "Short-EN",  "EN", "need ammo" },
    { "Short-RU",  "RU", u8"нужны патроны" },
    { "Short-ZH",  "ZH", u8"需要弹药" },
    { "Short-KO",  "KO", u8"탄약 필요" },
    { "Medium-EN", "EN", "build the gates back up we need defenses now" },
    { "Medium-RU", "RU", u8"постройте ворота снова нам нужна оборона сейчас" },
    { "Medium-ZH", "ZH", u8"重建防线，我们需要立即防御" },
    { "Medium-KO", "KO", u8"적이 밀고 있습니다 방어선을 강화하세요" },
    { "Long-EN",   "EN", "hey guys we need more people at the front line the enemy is pushing hard and we are running low on supplies" },
    { "Long-RU",   "RU", u8"всем отрядам отступить к точке сбора противник прорвал линию обороны и движется в нашу сторону нужно срочно отступать" },
    { "Long-ZH",   "ZH", u8"各位注意前线需要更多兵力敌军正在猛烈进攻我们的补给已经严重不足请立即支援" },
    { "Long-KO",   "KO", u8"여러분 전선에 더 많은 병력이 필요합니다 적이 강하게 밀고 있고 보급품이 부족합니다 지금 당장 지원이 필요해요" },
};
static const int kNumSentences = (int)(sizeof(kTestSentences) / sizeof(kTestSentences[0]));

static const char* kPresets[] = { "Low", "Medium", "High" };
static const int   kNumPresets = 3;

struct BenchRow {
    const char* preset;
    const char* sentLabel;
    const char* lang;
    int         sentLen;
    int         run;
    long long   elapsedMs;
    double      tokPerSec;
    int         evalCount;
    std::string translation;
};

// ============================================================
// ベンチマーク実行
// ============================================================

static int RunBenchmark(const translate::TranslateConfig& baseCfg,
                        const std::string& outputCsv, int warmRuns)
{
    printf("============================================================\n");
    printf(" Preset Comparison Benchmark\n");
    printf(" %d presets x %d sentences x %d warm runs = %d requests\n",
           kNumPresets, kNumSentences, warmRuns, kNumPresets * kNumSentences * warmRuns);
    printf("============================================================\n\n");

    std::vector<BenchRow> rows;
    rows.reserve(kNumPresets * kNumSentences * warmRuns);

    bool firstPreset = true;
    for (int pi = 0; pi < kNumPresets; pi++) {
        const char* preset = kPresets[pi];

        if (!firstPreset) translate::Shutdown();
        firstPreset = false;

        translate::TranslateConfig cfg = baseCfg;
        cfg.performancePreset = preset;

        printf("--- [%s] ---\n", preset);
        if (!translate::Init(cfg)) {
            printf("Error: Init 失敗 (preset=%s)\n", preset);
            return 1;
        }

        for (int si = 0; si < kNumSentences; si++) {
            const TestSentence& sent = kTestSentences[si];

            for (int run = 1; run <= warmRuns; run++) {
                auto t0 = std::chrono::high_resolution_clock::now();
                std::string result = translate::Sync(sent.text);
                auto t1 = std::chrono::high_resolution_clock::now();
                long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
                translate::SyncStats stats = translate::GetLastSyncStats();

                // コンソール: 先頭60文字まで表示
                std::string preview = result;
                // バイト数で切る (UTF-8 なのでマルチバイト境界を考慮)
                if (preview.size() > 60) {
                    size_t cut = 60;
                    while (cut > 0 && (preview[cut] & 0xC0) == 0x80) --cut;
                    preview = preview.substr(0, cut) + "...";
                }
                printf("  [%s] %s run%d: %lldms  %.1f tok/s  [%s]\n",
                       result.empty() ? "NG" : "OK",
                       sent.label, run, ms, stats.tokPerSec, preview.c_str());

                BenchRow row;
                row.preset      = preset;
                row.sentLabel   = sent.label;
                row.lang        = sent.lang;
                row.sentLen     = (int)strlen(sent.text);
                row.run         = run;
                row.elapsedMs   = ms;
                row.tokPerSec   = stats.tokPerSec;
                row.evalCount   = stats.evalCount;
                row.translation = result;
                rows.push_back(std::move(row));
            }
        }
        printf("\n");
    }
    translate::Shutdown();

    // ============================================================
    // CSV 出力
    // ============================================================
    {
        FILE* f = fopen(outputCsv.c_str(), "wb");
        if (!f) {
            printf("Error: CSV を開けません: %s\n", outputCsv.c_str());
            return 1;
        }
        // UTF-8 BOM
        fwrite("\xEF\xBB\xBF", 1, 3, f);
        fprintf(f, "Preset,Sentence,Lang,SentenceLen,Run,ElapsedMs,TokPerSec,EvalCount,Translation\r\n");
        for (const auto& r : rows) {
            fprintf(f, "%s,%s,%s,%d,%d,%lld,%.2f,%d,%s\r\n",
                    r.preset, r.sentLabel, r.lang, r.sentLen,
                    r.run, r.elapsedMs, r.tokPerSec, r.evalCount,
                    CsvEscape(r.translation).c_str());
        }
        fclose(f);
        printf("[Done] CSV: %s\n\n", outputCsv.c_str());
    }

    // ============================================================
    // 速度サマリ (ウォーム平均)
    // ============================================================
    printf("============================================================\n");
    printf(" Speed Summary  (warm avg, run 2-%d)\n", warmRuns);
    printf("============================================================\n\n");

    const char* langs[] = { "EN", "RU", "ZH", "KO" };
    const char* lenPfx[] = { "Short", "Medium", "Long" };

    for (int li = 0; li < 4; li++) {
        printf("  [%s]\n", langs[li]);
        printf("  %-10s %10s %10s %10s %8s\n",
               "Preset", "Short(ms)", "Med(ms)", "Long(ms)", "tok/s");
        printf("  %-10s %10s %10s %10s %8s\n",
               "----------", "----------", "----------", "----------", "--------");

        for (int pi = 0; pi < kNumPresets; pi++) {
            double sumMs[3] = {}, sumTps = 0.0;
            int    cntMs[3] = {}, cntTps = 0;

            for (const auto& r : rows) {
                if (r.run == 1) continue; // run1 はコールドスタートの可能性があるため除外
                if (strcmp(r.preset, kPresets[pi]) != 0) continue;
                if (strcmp(r.lang, langs[li]) != 0) continue;
                for (int lp = 0; lp < 3; lp++) {
                    if (strncmp(r.sentLabel, lenPfx[lp], strlen(lenPfx[lp])) == 0) {
                        sumMs[lp] += (double)r.elapsedMs;
                        cntMs[lp]++;
                    }
                }
                if (r.tokPerSec > 0) { sumTps += r.tokPerSec; cntTps++; }
            }

            double shortMs = cntMs[0] ? sumMs[0] / cntMs[0] : 0;
            double medMs   = cntMs[1] ? sumMs[1] / cntMs[1] : 0;
            double longMs  = cntMs[2] ? sumMs[2] / cntMs[2] : 0;
            double tps     = cntTps   ? sumTps / cntTps      : 0;
            printf("  %-10s %10.0f %10.0f %10.0f %8.1f\n",
                   kPresets[pi], shortMs, medMs, longMs, tps);
        }
        printf("\n");
    }

    // ============================================================
    // 翻訳品質 (各文の run1 出力)
    // ============================================================
    printf("============================================================\n");
    printf(" Translation Quality  (run 1 output)\n");
    printf("============================================================\n\n");

    for (int si = 0; si < kNumSentences; si++) {
        const TestSentence& sent = kTestSentences[si];
        printf("  [%s] %s\n", sent.label, sent.text);
        for (int pi = 0; pi < kNumPresets; pi++) {
            for (const auto& r : rows) {
                if (r.run != 1) continue;
                if (strcmp(r.preset, kPresets[pi]) != 0) continue;
                if (strcmp(r.sentLabel, sent.label) != 0) continue;
                printf("  %-10s: %s\n", r.preset, r.translation.c_str());
                break;
            }
        }
        printf("\n");
    }

    return 0;
}

// ============================================================
// main
// ============================================================

int main(int /*argc*/, char* /*argv*/[]) {
    // Unicode コマンドライン取得
    int wargc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    std::vector<std::string> args;
    for (int i = 0; i < wargc; i++)
        args.push_back(WideToUtf8(wargv[i]));
    LocalFree(wargv);

    // デフォルト設定
    translate::TranslateConfig cfg;
    std::string inputText;
    std::string inputFile;
    std::string outputCsv = "benchmark_presets.csv";
    bool presetSpecified = false;
    bool benchmarkMode   = false;
    bool rawMode         = false;
    int  warmRuns        = 5;

    // 引数パース
    for (int i = 1; i < (int)args.size(); i++) {
        if (args[i] == "--endpoint" && i + 1 < (int)args.size()) {
            cfg.endpoint = args[++i];
        } else if (args[i] == "--preset" && i + 1 < (int)args.size()) {
            cfg.performancePreset = args[++i];
            presetSpecified = true;
        } else if (args[i] == "--lang" && i + 1 < (int)args.size()) {
            ++i; // targetLang は config::GetTranslationMode() に移行済み、引数は無視
        } else if (args[i] == "--file" && i + 1 < (int)args.size()) {
            inputFile = args[++i];
        } else if (args[i] == "--output" && i + 1 < (int)args.size()) {
            outputCsv = args[++i];
        } else if (args[i] == "--runs" && i + 1 < (int)args.size()) {
            warmRuns = atoi(args[++i].c_str());
            if (warmRuns < 1) warmRuns = 1;
        } else if (args[i] == "--benchmark") {
            benchmarkMode = true;
        } else if (args[i] == "--raw") {
            rawMode = true;
        } else if (args[i][0] != '-') {
            inputText = args[i];
        }
    }

    // --preset 未指定時は config.ini から読む
    if (!presetSpecified) {
        char buf[64] = {};
        GetPrivateProfileStringA("Translation", "PerformancePreset", "Medium",
                                  buf, sizeof(buf), ".\\config.ini");
        cfg.performancePreset = buf;
    }

    // ログ初期化 (コンソール出力のみ)
    logging::Init(".\\", !rawMode);
    SetConsoleOutputCP(CP_UTF8);

    // ベンチマークモード
    if (benchmarkMode) {
        int ret = RunBenchmark(cfg, outputCsv, warmRuns);
        logging::Shutdown();
        return ret;
    }

    // 翻訳モジュール初期化 (ベンチマーク以外)
    if (!translate::Init(cfg)) {
        printf("Error: 翻訳モジュールの初期化に失敗しました\n");
        printf("  Ollama が起動しているか確認してください: ollama serve\n");
        logging::Shutdown();
        return 1;
    }

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
            size_t msgPos = line.find(": ", 20);
            if (msgPos == std::string::npos) continue;
            std::string msg = line.substr(msgPos + 2);
            if (msg.empty()) continue;

            printf("[原文]  %s\n", msg.c_str());
            std::string translated = translate::Sync(msg);
            printf("[翻訳]  %s\n\n", translated.empty() ? "(エラー)" : translated.c_str());
            count++;
        }
        printf("=== %d 件翻訳完了 ===\n", count);

    } else if (!inputText.empty()) {
        // 単文モード
        if (!rawMode) printf("[原文]  %s\n", inputText.c_str());
        std::string translated = translate::Sync(inputText);
        if (rawMode) {
            if (!translated.empty()) printf("%s\n", translated.c_str());
            if (translated.empty()) {
                translate::Shutdown();
                logging::Shutdown();
                return 1;
            }
        } else {
            printf("[翻訳]  %s\n", translated.empty() ? "(エラー)" : translated.c_str());
        }

    } else {
        // 対話モード
        printf("=== Foxhole Chat Translation Test ===\n");
        printf("Endpoint: %s\n", cfg.endpoint.c_str());
        printf("Preset:   %s\n", cfg.performancePreset.c_str());
        printf("Target:   (runtime TranslationMode)\n");
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
            printf("=> %s\n\n", translated.empty() ? "(エラー)" : translated.c_str());
        }
    }

    translate::Shutdown();
    logging::Shutdown();
    return 0;
}
