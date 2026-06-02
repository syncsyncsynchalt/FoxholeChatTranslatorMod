// ============================================================
// tts.cpp - 多言語TTS (Sherpa-ONNX / VOICEVOX Core)
//
// sherpa-onnx.dll / voicevox_core.dll を tools/tts/ から動的ロード
// 日本語: VOICEVOX (JA=青山龍星/JAZ=ずんだもん), その他: Sherpa-ONNX Piper/Supertonic
// 再生: XAudio2
// ============================================================

#include "tts.h"
#include "tts_install.h"
#include "config.h"
#include "log.h"

#include <windows.h>
#include <xaudio2.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <regex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// ============================================================
// Sherpa-ONNX C API 構造体 (v1.13.x ABI)
// ============================================================

struct SherpaOnnxOfflineTtsVitsModelConfig {
    const char* model;
    const char* lexicon;
    const char* tokens;
    const char* data_dir;
    float       noise_scale;
    float       noise_scale_w;
    float       length_scale;
    const char* dict_dir;
};

struct SherpaOnnxOfflineTtsMatchaModelConfig {
    const char* acoustic_model;
    const char* vocoder;
    const char* lexicon;
    const char* tokens;
    const char* data_dir;
    float       noise_scale;
    float       length_scale;
    const char* dict_dir;
};

struct SherpaOnnxOfflineTtsKokoroModelConfig {
    const char* model;
    const char* voices;
    const char* tokens;
    const char* data_dir;
    float       length_scale;
    const char* dict_dir;
    const char* lexicon;
    const char* lang;
};

struct SherpaOnnxOfflineTtsKittenModelConfig {
    const char* model;
    const char* voices;
    const char* tokens;
    const char* data_dir;
    float       length_scale;
};

struct SherpaOnnxOfflineTtsZipvoiceModelConfig {
    const char* tokens;
    const char* encoder;
    const char* decoder;
    const char* vocoder;
    const char* data_dir;
    const char* lexicon;
    float       feat_scale;
    float       t_shift;
    float       target_rms;
    float       guidance_scale;
};

struct SherpaOnnxOfflineTtsPocketModelConfig {
    const char* lm_flow;
    const char* lm_main;
    const char* encoder;
    const char* decoder;
    const char* text_conditioner;
    const char* vocab_json;
    const char* token_scores_json;
    int32_t     voice_embedding_cache_capacity;
};

struct SherpaOnnxOfflineTtsSupertonicModelConfig {
    const char* duration_predictor;
    const char* text_encoder;
    const char* vector_estimator;
    const char* vocoder;
    const char* tts_json;
    const char* unicode_indexer;
    const char* voice_style;
};

struct SherpaOnnxOfflineTtsModelConfig {
    SherpaOnnxOfflineTtsVitsModelConfig       vits;
    int32_t                                   num_threads;
    int32_t                                   debug;
    const char*                               provider;
    SherpaOnnxOfflineTtsMatchaModelConfig     matcha;
    SherpaOnnxOfflineTtsKokoroModelConfig     kokoro;
    SherpaOnnxOfflineTtsKittenModelConfig     kitten;
    SherpaOnnxOfflineTtsZipvoiceModelConfig   zipvoice;
    SherpaOnnxOfflineTtsPocketModelConfig     pocket;
    SherpaOnnxOfflineTtsSupertonicModelConfig supertonic;
};

struct SherpaOnnxOfflineTtsConfig {
    SherpaOnnxOfflineTtsModelConfig model;
    const char* rule_fsts;
    int32_t     max_num_sentences;
    const char* rule_fars;
    float       silence_scale;
};

struct SherpaOnnxGeneratedAudio {
    const float* samples;
    int32_t      n;           // v1.13.x では 'n' (旧 num_samples)
    int32_t      sample_rate;
};

struct SherpaOnnxOfflineTts; // opaque

typedef SherpaOnnxOfflineTts*           (*PFN_CreateTts)(const SherpaOnnxOfflineTtsConfig*);
typedef void                            (*PFN_DestroyTts)(SherpaOnnxOfflineTts*);
typedef const SherpaOnnxGeneratedAudio* (*PFN_Generate)(const SherpaOnnxOfflineTts*, const char*, int32_t, float);
typedef void                            (*PFN_DestroyAudio)(const SherpaOnnxGeneratedAudio*);

// ============================================================
// VOICEVOX Core C API (v0.16.x) - 日本語ずんだもん TTS
// ============================================================

enum VoicevoxResultCode : int32_t { VOICEVOX_RESULT_OK = 0 };

struct VoicevoxLoadOnnxruntimeOptions { const char* filename; };

struct VoicevoxInitializeOptions {
    int32_t  acceleration_mode; // 1 = CPU
    uint16_t cpu_num_threads;
};

struct VoicevoxTtsOptions { bool enable_interrogative_upspeak; };

typedef uint32_t VoicevoxStyleId;

struct VoicevoxOnnxruntime;    // opaque
struct OpenJtalkRc;            // opaque
struct VoicevoxSynthesizer;    // opaque
struct VoicevoxVoiceModelFile; // opaque

typedef int32_t (*PFN_VvOnnxLoad) (VoicevoxLoadOnnxruntimeOptions, const VoicevoxOnnxruntime**);
typedef int32_t (*PFN_VvJtalkNew) (const char*, OpenJtalkRc**);
typedef VoicevoxInitializeOptions (*PFN_VvDefInitOpt)(void);
typedef int32_t (*PFN_VvSynthNew) (const VoicevoxOnnxruntime*, const OpenJtalkRc*, VoicevoxInitializeOptions, VoicevoxSynthesizer**);
typedef int32_t (*PFN_VvModelOpen)(const char*, VoicevoxVoiceModelFile**);
typedef int32_t (*PFN_VvLoadModel)(const VoicevoxSynthesizer*, const VoicevoxVoiceModelFile*);
typedef void    (*PFN_VvModelDel) (VoicevoxVoiceModelFile*);
typedef VoicevoxTtsOptions (*PFN_VvDefTtsOpt)(void);
typedef int32_t (*PFN_VvTts)      (const VoicevoxSynthesizer*, const char*, VoicevoxStyleId, VoicevoxTtsOptions, uintptr_t*, uint8_t**);
typedef void    (*PFN_VvWavFree)  (uint8_t*);
typedef void    (*PFN_VvSynthDel) (VoicevoxSynthesizer*);
typedef void    (*PFN_VvJtalkDel) (OpenJtalkRc*);

// ============================================================
// 言語判定
// ============================================================

enum class Lang { EN, RU, KO, ZH, JA };

static Lang DetectLanguage(const char* textUtf8) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(textUtf8);
    int cyrillic = 0, hangul = 0, cjk = 0, hiragana = 0, katakana = 0;
    int total = 0;

    while (*p && total < 200) {
        uint32_t cp = 0;
        int bytes = 0;
        if (*p < 0x80) {
            cp = *p; bytes = 1;
        } else if ((*p & 0xE0) == 0xC0) {
            cp = *p & 0x1F; bytes = 2;
        } else if ((*p & 0xF0) == 0xE0) {
            cp = *p & 0x0F; bytes = 3;
        } else if ((*p & 0xF8) == 0xF0) {
            cp = *p & 0x07; bytes = 4;
        } else {
            p++; continue;
        }
        for (int i = 1; i < bytes && p[i]; i++)
            cp = (cp << 6) | (p[i] & 0x3F);
        p += bytes;
        total++;

        if      (cp >= 0x0400 && cp <= 0x04FF) cyrillic++;
        else if (cp >= 0xAC00 && cp <= 0xD7AF) hangul++;
        else if (cp >= 0x3040 && cp <= 0x309F) hiragana++;
        else if (cp >= 0x30A0 && cp <= 0x30FF) katakana++;
        else if (cp >= 0x4E00 && cp <= 0x9FFF) cjk++;
    }

    if (cyrillic > 0)              return Lang::RU;
    if (hangul > 0)                return Lang::KO;
    if (hiragana > 0 || katakana > 0) return Lang::JA;
    if (cjk > 0)                   return Lang::ZH;
    return Lang::EN;
}

// ============================================================
// TTS 読み辞書 (tts_readings.txt)
// 書式: pattern [lang]=読み [i]
// ============================================================

struct TtsReading { std::regex re; std::string replacement; };
static std::unordered_map<std::string, std::vector<TtsReading>> g_ttsReadings;

static void LoadTtsReadings(const std::string& path) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {
        logging::Debug("[TTS] tts_readings.txt 未検出: %s", path.c_str());
        return;
    }
    int count = 0;
    char buf[512];
    while (fgets(buf, sizeof(buf), f)) {
        // 末尾の改行・空白を除去
        size_t len = strlen(buf);
        while (len > 0 && (buf[len-1] == '\r' || buf[len-1] == '\n' || buf[len-1] == ' '))
            buf[--len] = '\0';
        if (len == 0 || buf[0] == '#') continue;

        // '=' で左右に分割
        char* eq = strchr(buf, '=');
        if (!eq) continue;
        *eq = '\0';
        std::string left  = buf;
        std::string right = eq + 1;

        // 右部: 末尾 " i" → icase フラグ
        bool icase = false;
        if (right.size() >= 2 && right.back() == 'i' && right[right.size()-2] == ' ') {
            icase = true;
            right = right.substr(0, right.size() - 2);
        }
        while (!right.empty() && (right.back() == ' ' || right.back() == '\t'))
            right.pop_back();
        if (right.empty()) continue;

        // 左部: 末尾スペースで pattern と lang に分割
        while (!left.empty() && (left.back() == ' ' || left.back() == '\t'))
            left.pop_back();
        std::string lang, pattern;
        size_t sp = left.rfind(' ');
        if (sp == std::string::npos) {
            pattern = left;
            lang    = "";  // 全言語
        } else {
            lang    = left.substr(sp + 1);
            pattern = left.substr(0, sp);
            while (!pattern.empty() && (pattern.back() == ' ' || pattern.back() == '\t'))
                pattern.pop_back();
        }
        if (pattern.empty()) continue;

        // \b を自動付与
        std::string reStr;
        if (pattern.size() < 2 || pattern.substr(0, 2) != "\\b") reStr = "\\b";
        reStr += pattern;
        if (pattern.size() < 2 || pattern.substr(pattern.size()-2) != "\\b") reStr += "\\b";

        auto flags = std::regex::ECMAScript;
        if (icase) flags |= std::regex::icase;
        try {
            g_ttsReadings[lang].push_back({std::regex(reStr, flags), right});
            count++;
        } catch (const std::regex_error&) {
            logging::Debug("[TTS] tts_readings.txt 正規表現エラー: %s", reStr.c_str());
        }
    }
    fclose(f);
    logging::Debug("[TTS] tts_readings.txt: %d エントリ読み込み (%s)", count, path.c_str());
}

static std::string ApplyTtsReadings(const std::string& text, const char* lang) {
    if (g_ttsReadings.empty()) return text;
    std::string result = text;
    auto it = g_ttsReadings.find("");
    if (it != g_ttsReadings.end())
        for (auto& r : it->second)
            result = std::regex_replace(result, r.re, r.replacement);
    if (lang) {
        it = g_ttsReadings.find(lang);
        if (it != g_ttsReadings.end())
            for (auto& r : it->second)
                result = std::regex_replace(result, r.re, r.replacement);
    }
    return result;
}

// ============================================================
// DLL ベースディレクトリ取得
// ============================================================

static std::string GetTtsToolDir() {
    char path[MAX_PATH];
    HMODULE hSelf = nullptr;
    GetModuleHandleExA(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(&tts::Init), &hSelf);
    GetModuleFileNameA(hSelf, path, MAX_PATH);
    std::string dir(path);
    size_t pos = dir.rfind('\\');
    if (pos != std::string::npos) dir = dir.substr(0, pos + 1);
    return dir + "tools\\tts\\";
}

// ============================================================
// Sherpa-ONNX DLL 動的ロード
// ============================================================

static HMODULE    g_sherpaLib    = nullptr;
static PFN_CreateTts   g_fnCreate   = nullptr;
static PFN_DestroyTts  g_fnDestroy  = nullptr;
static PFN_Generate    g_fnGenerate = nullptr;
static PFN_DestroyAudio g_fnFreeAudio = nullptr;

// VOICEVOX グローバル
static HMODULE               g_vvLib        = nullptr;
static bool                  g_vvReady      = false;
static uint32_t              g_vvStyleId    = 3;   // JAZ モード (ずんだもんノーマル)
static uint32_t              g_vvJaStyleId  = 13;  // JA モード (青山龍星ノーマル)
static bool                  g_vv15Ready    = false; // 15.vvm (青山龍星) ロード済みフラグ

static PFN_VvOnnxLoad        g_vvOnnxLoad   = nullptr;
static PFN_VvJtalkNew        g_vvJtalkNew   = nullptr;
static PFN_VvDefInitOpt      g_vvDefInitOpt = nullptr;
static PFN_VvSynthNew        g_vvSynthNew   = nullptr;
static PFN_VvModelOpen       g_vvModelOpen  = nullptr;
static PFN_VvLoadModel       g_vvLoadModel  = nullptr;
static PFN_VvModelDel        g_vvModelDel   = nullptr;
static PFN_VvDefTtsOpt       g_vvDefTtsOpt  = nullptr;
static PFN_VvTts             g_vvTts        = nullptr;
static PFN_VvWavFree         g_vvWavFree    = nullptr;
static PFN_VvSynthDel        g_vvSynthDel   = nullptr;
static PFN_VvJtalkDel        g_vvJtalkDel   = nullptr;

static const VoicevoxOnnxruntime* g_vvOnnxruntime = nullptr;
static OpenJtalkRc*               g_vvOpenJtalk   = nullptr;
static VoicevoxSynthesizer*       g_vvSynthesizer = nullptr;

static bool LoadSherpaLib(const std::string& ttsDir) {
    std::string dllPath = ttsDir + "sherpa-onnx-c-api.dll";
    g_sherpaLib = LoadLibraryExA(dllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_sherpaLib) {
        logging::Debug("[TTS] sherpa-onnx-c-api.dll ロード失敗 (err=%lu): %s", GetLastError(), ttsDir.c_str());
        return false;
    }

    g_fnCreate    = reinterpret_cast<PFN_CreateTts>  (GetProcAddress(g_sherpaLib, "SherpaOnnxCreateOfflineTts"));
    g_fnDestroy   = reinterpret_cast<PFN_DestroyTts> (GetProcAddress(g_sherpaLib, "SherpaOnnxDestroyOfflineTts"));
    g_fnGenerate  = reinterpret_cast<PFN_Generate>   (GetProcAddress(g_sherpaLib, "SherpaOnnxOfflineTtsGenerate"));
    g_fnFreeAudio = reinterpret_cast<PFN_DestroyAudio>(GetProcAddress(g_sherpaLib, "SherpaOnnxDestroyOfflineTtsGeneratedAudio"));

    if (!g_fnCreate || !g_fnDestroy || !g_fnGenerate || !g_fnFreeAudio) {
        logging::Debug("[TTS] sherpa-onnx エクスポート取得失敗");
        FreeLibrary(g_sherpaLib);
        g_sherpaLib = nullptr;
        return false;
    }

    logging::Debug("[TTS] sherpa-onnx-c-api.dll ロード完了");
    return true;
}

// ============================================================
// VOICEVOX Core 初期化・合成
// ============================================================

static bool InitVoicevox(const std::string& ttsDir) {
    std::string vvDir  = ttsDir + "voicevox\\";
    std::string dllPath = vvDir + "c_api\\lib\\voicevox_core.dll";

    if (GetFileAttributesA(dllPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        logging::Debug("[TTS-VV] VOICEVOX DLL が見つかりません (自動インストール待機中)");
        return false;
    }

    g_vvLib = LoadLibraryExA(dllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_vvLib) {
        logging::Debug("[TTS-VV] DLL ロード失敗 err=%lu", GetLastError());
        return false;
    }

    auto gp = [&](const char* name) { return GetProcAddress(g_vvLib, name); };
    g_vvOnnxLoad   = (PFN_VvOnnxLoad)  gp("voicevox_onnxruntime_load_once");
    g_vvJtalkNew   = (PFN_VvJtalkNew)  gp("voicevox_open_jtalk_rc_new");
    g_vvDefInitOpt = (PFN_VvDefInitOpt)gp("voicevox_make_default_initialize_options");
    g_vvSynthNew   = (PFN_VvSynthNew)  gp("voicevox_synthesizer_new");
    g_vvModelOpen  = (PFN_VvModelOpen) gp("voicevox_voice_model_file_open");
    g_vvLoadModel  = (PFN_VvLoadModel) gp("voicevox_synthesizer_load_voice_model");
    g_vvModelDel   = (PFN_VvModelDel)  gp("voicevox_voice_model_file_delete");
    g_vvDefTtsOpt  = (PFN_VvDefTtsOpt) gp("voicevox_make_default_tts_options");
    g_vvTts        = (PFN_VvTts)       gp("voicevox_synthesizer_tts");
    g_vvWavFree    = (PFN_VvWavFree)   gp("voicevox_wav_free");
    g_vvSynthDel   = (PFN_VvSynthDel)  gp("voicevox_synthesizer_delete");
    g_vvJtalkDel   = (PFN_VvJtalkDel)  gp("voicevox_open_jtalk_rc_delete");

    if (!g_vvOnnxLoad || !g_vvJtalkNew || !g_vvSynthNew || !g_vvModelOpen ||
        !g_vvLoadModel || !g_vvModelDel || !g_vvTts || !g_vvWavFree ||
        !g_vvSynthDel  || !g_vvJtalkDel) {
        logging::Debug("[TTS-VV] 関数取得失敗");
        FreeLibrary(g_vvLib); g_vvLib = nullptr;
        return false;
    }

    // ONNX Runtime: onnxruntime/lib/ 内の最初の .dll を使う
    std::string ortDir = vvDir + "onnxruntime\\lib\\";
    std::string ortPath;
    {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((ortDir + "*.dll").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            ortPath = ortDir + fd.cFileName;
            FindClose(h);
        }
    }
    if (ortPath.empty()) {
        logging::Debug("[TTS-VV] voicevox_onnxruntime.dll が見つかりません: %s", ortDir.c_str());
        FreeLibrary(g_vvLib); g_vvLib = nullptr;
        return false;
    }
    VoicevoxLoadOnnxruntimeOptions ortOpts = { ortPath.c_str() };
    auto rc = g_vvOnnxLoad(ortOpts, &g_vvOnnxruntime);
    if (rc != VOICEVOX_RESULT_OK) {
        logging::Debug("[TTS-VV] ONNX Runtime ロード失敗 rc=%d", rc);
        FreeLibrary(g_vvLib); g_vvLib = nullptr;
        return false;
    }

    // OpenJTalk: dict/ 内の最初のサブディレクトリを辞書パスとして使う
    std::string dictParent = vvDir + "dict\\";
    std::string dictDir;
    {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((dictParent + "*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != '.') {
                    dictDir = dictParent + fd.cFileName;
                    break;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    if (dictDir.empty()) dictDir = dictParent; // フォールバック

    rc = g_vvJtalkNew(dictDir.c_str(), &g_vvOpenJtalk);
    if (rc != VOICEVOX_RESULT_OK) {
        logging::Debug("[TTS-VV] OpenJTalk 初期化失敗 rc=%d dir=%s", rc, dictDir.c_str());
        FreeLibrary(g_vvLib); g_vvLib = nullptr;
        return false;
    }

    // Synthesizer 作成
    VoicevoxInitializeOptions initOpts = g_vvDefInitOpt ? g_vvDefInitOpt()
                                                        : VoicevoxInitializeOptions{1, 0};
    initOpts.acceleration_mode = 0; // AUTO: GPU 優先 (CUDA)、なければ CPU
    initOpts.cpu_num_threads   = 1;
    rc = g_vvSynthNew(g_vvOnnxruntime, g_vvOpenJtalk, initOpts, &g_vvSynthesizer);
    if (rc != VOICEVOX_RESULT_OK) {
        logging::Debug("[TTS-VV] Synthesizer 作成失敗 rc=%d", rc);
        g_vvJtalkDel(g_vvOpenJtalk); g_vvOpenJtalk = nullptr;
        FreeLibrary(g_vvLib); g_vvLib = nullptr;
        return false;
    }

    // 全 VVM モデルをロード
    std::string vvmsDir = vvDir + "models\\vvms\\";
    int loaded = 0;
    {
        WIN32_FIND_DATAA fd;
        HANDLE h = FindFirstFileA((vvmsDir + "*.vvm").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::string vvmPath = vvmsDir + fd.cFileName;
                VoicevoxVoiceModelFile* model = nullptr;
                if (g_vvModelOpen(vvmPath.c_str(), &model) == VOICEVOX_RESULT_OK) {
                    g_vvLoadModel(g_vvSynthesizer, model);
                    g_vvModelDel(model);
                    loaded++;
                }
            } while (FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    if (loaded == 0) {
        logging::Debug("[TTS-VV] VVM モデルが見つかりません: %s", vvmsDir.c_str());
        g_vvSynthDel(g_vvSynthesizer); g_vvSynthesizer = nullptr;
        g_vvJtalkDel(g_vvOpenJtalk);   g_vvOpenJtalk   = nullptr;
        FreeLibrary(g_vvLib); g_vvLib = nullptr;
        return false;
    }

    g_vv15Ready = (GetFileAttributesA((vvmsDir + "15.vvm").c_str()) != INVALID_FILE_ATTRIBUTES);
    logging::Debug("[TTS-VV] 初期化完了: %d VVM, styleId=%u, vv15Ready=%d", loaded, g_vvStyleId, g_vv15Ready);
    g_vvReady = true;
    return true;
}


// ============================================================
// SEH ヘルパー: C++ デストラクタのないスコープで __try を使う
// ============================================================

// C2712 対策: __try と C++ オブジェクトを同関数内に置けないため、
//             API 呼び出しだけを純粋な C ライクな関数に隔離する。

static SherpaOnnxOfflineTts* TryCreateModel(const SherpaOnnxOfflineTtsConfig* cfg, DWORD* outCode) {
    *outCode = 0;
    __try {
        return g_fnCreate(cfg);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outCode = GetExceptionCode();
        return nullptr;
    }
}

static const SherpaOnnxGeneratedAudio* TryGenerate(
    SherpaOnnxOfflineTts* model, const char* text, DWORD* outCode)
{
    *outCode = 0;
    __try {
        return g_fnGenerate(model, text, 0, 1.0f);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outCode = GetExceptionCode();
        return nullptr;
    }
}

static int32_t TryVvTts(
    VoicevoxSynthesizer* synth, const char* text, VoicevoxStyleId styleId,
    VoicevoxTtsOptions opts, uintptr_t* outLen, uint8_t** outBuf, DWORD* outCode)
{
    *outCode = 0;
    __try {
        return g_vvTts(synth, text, styleId, opts, outLen, outBuf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *outCode = GetExceptionCode();
        return -1;
    }
}

struct PcmData { std::vector<int16_t> samples; int32_t sampleRate = 0; };

// 16kHz ダウンサンプル (モノラル 16bit 16000Hz — 狭帯域音声品質)
// ナイーブ間引き = アンチエイリアスなし → エイリアシングが無線らしい質感を加える
static void ResampleTo16k(std::vector<int16_t>& samples, int32_t& sampleRate) {
    const int32_t kDstRate = 16000;
    if (sampleRate <= kDstRate) return;
    int srcCount = static_cast<int>(samples.size());
    int dstCount = static_cast<int>(static_cast<int64_t>(srcCount) * kDstRate / sampleRate);
    std::vector<int16_t> out(dstCount);
    for (int i = 0; i < dstCount; i++) {
        int srcIdx = static_cast<int>(static_cast<int64_t>(i) * sampleRate / kDstRate);
        out[i] = samples[srcIdx < srcCount ? srcIdx : srcCount - 1];
    }
    samples    = std::move(out);
    sampleRate = kDstRate;
}

static bool ParseWavPcm(const uint8_t* wav, uintptr_t size, PcmData& out) {
    if (size < 12) return false;
    if (memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVE", 4)) return false;

    uint32_t sampleRate = 0, dataSize = 0;
    uint16_t bitsPerSample = 0;
    const uint8_t* dataPtr = nullptr;
    size_t pos = 12;

    const size_t fileSize = static_cast<size_t>(size);
    while (pos + 8 <= fileSize) {
        uint32_t chunkSize = *reinterpret_cast<const uint32_t*>(&wav[pos + 4]);
        size_t chunkEnd = pos + 8 + static_cast<size_t>(chunkSize);
        // オーバーフロー・範囲外チェック: chunkEnd が折り返すか fileSize を超える場合は終了
        if (chunkEnd < pos || chunkEnd > fileSize) break;
        if (!memcmp(&wav[pos], "fmt ", 4) && chunkSize >= 16) {
            sampleRate    = *reinterpret_cast<const uint32_t*>(&wav[pos + 12]);
            bitsPerSample = *reinterpret_cast<const uint16_t*>(&wav[pos + 22]);
        } else if (!memcmp(&wav[pos], "data", 4)) {
            dataPtr  = &wav[pos + 8];
            dataSize = chunkSize;
        }
        pos = chunkEnd;
        if (chunkSize % 2) pos++;
    }
    if (!dataPtr || !sampleRate || bitsPerSample != 16) return false;
    if (dataPtr + dataSize > wav + fileSize) return false;  // data チャンクがバッファ内に収まるか確認

    out.sampleRate = static_cast<int32_t>(sampleRate);
    out.samples.assign(reinterpret_cast<const int16_t*>(dataPtr),
                       reinterpret_cast<const int16_t*>(dataPtr) + dataSize / 2);
    return true;
}

static bool SynthesizeVoicevox(const std::string& text, PcmData& out, uint32_t styleId) {
    VoicevoxTtsOptions opts = g_vvDefTtsOpt ? g_vvDefTtsOpt() : VoicevoxTtsOptions{false};
    opts.enable_interrogative_upspeak = false;

    if (config::Get().ttsVerboseLog)
        logging::Debug("[TTS-VV] 合成開始: bytes=%zu styleId=%u text=%.40s", text.size(), styleId, text.c_str());

    uintptr_t wavLen = 0;
    uint8_t*  wavBuf = nullptr;
    DWORD     excCode = 0;
    int32_t   rc = TryVvTts(g_vvSynthesizer, text.c_str(), styleId,
                             opts, &wavLen, &wavBuf, &excCode);
    if (excCode) {
        logging::Debug("[TTS-VV] 合成例外 code=0x%08X text=%.40s",
                       excCode, text.c_str());
        return false;
    }
    if (rc != VOICEVOX_RESULT_OK || !wavBuf) {
        logging::Debug("[TTS-VV] 合成失敗 rc=%d", rc);
        return false;
    }
    bool ok = ParseWavPcm(wavBuf, wavLen, out);
    g_vvWavFree(wavBuf);
    if (!ok)
        logging::Debug("[TTS-VV] WAV 解析失敗");
    else if (config::Get().ttsVerboseLog)
        logging::Debug("[TTS-VV] 合成完了: samples=%zu rate=%d",
                       out.samples.size(), out.sampleRate);
    return ok;
}

// ============================================================
// 言語別モデル管理 (遅延初期化)
// ============================================================

struct LangModel {
    SherpaOnnxOfflineTts* handle = nullptr;
    bool                  tried  = false; // 初期化済み (失敗含む)
};

static std::mutex              g_modelMutex;
static std::map<Lang, LangModel> g_models;
static std::string             g_ttsDir;
static std::string             g_sherpaProvider; // 初回検出後キャッシュ ("cuda" or "cpu")

// nvcuda.dll の存在で NVIDIA GPU を検出し、最適プロバイダーを返す
static const char* SelectSherpaProvider() {
    if (!g_sherpaProvider.empty()) return g_sherpaProvider.c_str();
    HMODULE hCuda = LoadLibraryA("nvcuda.dll");
    if (hCuda) {
        FreeLibrary(hCuda);
        g_sherpaProvider = "cuda";
    } else {
        g_sherpaProvider = "cpu";
    }
    return g_sherpaProvider.c_str();
}

// 15.vvm (青山龍星) をシンセサイザーに追加ロード — ダウンロード完了後の hot-reload 用
static void TryLoadVvm15() {
    if (!g_vvReady || g_vv15Ready) return;
    if (!g_vvModelOpen || !g_vvLoadModel || !g_vvModelDel || !g_vvSynthesizer) return;
    std::string path = g_ttsDir + "voicevox\\models\\vvms\\15.vvm";
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    VoicevoxVoiceModelFile* model = nullptr;
    if (g_vvModelOpen(path.c_str(), &model) != VOICEVOX_RESULT_OK || !model) {
        logging::Debug("[TTS-VV] 15.vvm ロード失敗");
        return;
    }
    auto rc = g_vvLoadModel(g_vvSynthesizer, model);
    g_vvModelDel(model);
    if (rc != VOICEVOX_RESULT_OK) {
        logging::Debug("[TTS-VV] 15.vvm シンセサイザーへのロード失敗 rc=%d", rc);
        return;
    }
    g_vv15Ready = true;
    logging::Debug("[TTS-VV] 15.vvm ロード完了 (青山龍星 style=%u 有効)", g_vvJaStyleId);
}

static const char* LangSubdir(Lang lang) {
    switch (lang) {
    case Lang::EN: return "en";
    case Lang::RU: return "ru";
    case Lang::KO: return "ko";
    case Lang::ZH: return "zh";
    case Lang::JA: return "ja";
    }
    return "en";
}

static SherpaOnnxOfflineTts* CreateSupertonicModel(const std::string& modelDir) {
    std::string dp  = modelDir + "duration_predictor.int8.onnx";
    std::string te  = modelDir + "text_encoder.int8.onnx";
    std::string ve  = modelDir + "vector_estimator.int8.onnx";
    std::string voc = modelDir + "vocoder.int8.onnx";
    std::string jsn = modelDir + "tts.json";
    std::string ui  = modelDir + "unicode_indexer.bin";
    std::string vs  = modelDir + "voice.bin";

    SherpaOnnxOfflineTtsConfig cfg = {};
    cfg.model.supertonic.duration_predictor = dp.c_str();
    cfg.model.supertonic.text_encoder       = te.c_str();
    cfg.model.supertonic.vector_estimator   = ve.c_str();
    cfg.model.supertonic.vocoder            = voc.c_str();
    cfg.model.supertonic.tts_json           = jsn.c_str();
    cfg.model.supertonic.unicode_indexer    = ui.c_str();
    cfg.model.supertonic.voice_style        = vs.c_str();
    cfg.model.num_threads   = 1;
    cfg.model.debug         = 0;
    cfg.max_num_sentences   = 1;

    cfg.model.provider = SelectSherpaProvider();
    DWORD excCode = 0;
    SherpaOnnxOfflineTts* handle = TryCreateModel(&cfg, &excCode);
    if ((excCode || !handle) && g_sherpaProvider != "cpu") {
        logging::Debug("[TTS] Supertonic: %s 利用不可、CPU にフォールバック", g_sherpaProvider.c_str());
        g_sherpaProvider = "cpu";
        cfg.model.provider = "cpu";
        excCode = 0;
        handle = TryCreateModel(&cfg, &excCode);
    }
    if (excCode)
        logging::Debug("[TTS] Supertonic モデル作成例外 code=0x%08X: %s",
                       excCode, modelDir.c_str());
    else if (!handle)
        logging::Debug("[TTS] Supertonic モデル作成失敗: %s", modelDir.c_str());
    else
        logging::Debug("[TTS] Supertonic モデル作成完了 (provider=%s): %s",
                       cfg.model.provider, modelDir.c_str());
    return handle;
}

static SherpaOnnxOfflineTts* CreateModel(Lang lang) {
    std::string modelDir  = g_ttsDir + "models\\" + LangSubdir(lang) + "\\";

    // Supertonic モデルを優先確認 (duration_predictor.int8.onnx の有無で判定)
    std::string dpPath = modelDir + "duration_predictor.int8.onnx";
    if (GetFileAttributesA(dpPath.c_str()) != INVALID_FILE_ATTRIBUTES)
        return CreateSupertonicModel(modelDir);

    // VITS (Piper / mimic3) モデル
    std::string modelPath  = modelDir + "model.onnx";
    std::string tokensPath = modelDir + "tokens.txt";
    std::string espeakDir  = g_ttsDir + "espeak-ng-data";

    if (GetFileAttributesA(modelPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        logging::Debug("[TTS] モデルなし: %s", modelPath.c_str());
        return nullptr;
    }

    std::string lexiconPath;
    std::string dictDir;

    std::string lexiconFile = modelDir + "lexicon.txt";
    if (GetFileAttributesA(lexiconFile.c_str()) != INVALID_FILE_ATTRIBUTES)
        lexiconPath = lexiconFile;

    if (lang == Lang::ZH) {
        std::string jiebaDir = modelDir + "dict";
        if (GetFileAttributesA(jiebaDir.c_str()) != INVALID_FILE_ATTRIBUTES)
            dictDir = jiebaDir;
    }

    bool hasEspeak = GetFileAttributesA(espeakDir.c_str()) != INVALID_FILE_ATTRIBUTES;
    logging::Debug("[TTS] CreateModel: lang=%s hasEspeak=%d model=%s",
                   LangSubdir(lang), (int)hasEspeak, modelPath.c_str());

    // espeak-ng を必要とする VITS モデルは Create/Generate 両フェーズで abort() クラッシュが確認済み。
    // SEH では捕捉不可のため、呼び出す前にスキップする。
    // ZH は espeak-ng 不要 (jieba) なので対象外。
    if (hasEspeak && lang != Lang::ZH) {
        logging::Debug("[TTS] %s: espeak-ng VITS は abort クラッシュ既知のためスキップ", LangSubdir(lang));
        return nullptr;
    }

    SherpaOnnxOfflineTtsConfig cfg = {};
    cfg.model.vits.model         = modelPath.c_str();
    cfg.model.vits.tokens        = tokensPath.c_str();
    cfg.model.vits.lexicon       = lexiconPath.empty() ? nullptr : lexiconPath.c_str();
    cfg.model.vits.dict_dir      = dictDir.empty() ? nullptr : dictDir.c_str();
    cfg.model.vits.data_dir      = (hasEspeak && lang != Lang::ZH) ? espeakDir.c_str() : nullptr;
    cfg.model.vits.noise_scale   = 0.667f;
    cfg.model.vits.noise_scale_w = 0.8f;
    cfg.model.vits.length_scale  = 1.0f;
    cfg.model.num_threads        = 1;
    cfg.model.debug              = 0;
    cfg.max_num_sentences        = 5;

    cfg.model.provider = SelectSherpaProvider();
    DWORD excCode = 0;
    SherpaOnnxOfflineTts* handle = TryCreateModel(&cfg, &excCode);
    if ((excCode || !handle) && g_sherpaProvider != "cpu") {
        logging::Debug("[TTS] VITS: %s 利用不可、CPU にフォールバック", g_sherpaProvider.c_str());
        g_sherpaProvider = "cpu";
        cfg.model.provider = "cpu";
        excCode = 0;
        handle = TryCreateModel(&cfg, &excCode);
    }
    if (excCode)
        logging::Debug("[TTS] VITS モデル作成例外 code=0x%08X lang=%s",
                       excCode, LangSubdir(lang));
    else if (!handle)
        logging::Debug("[TTS] モデル作成失敗 (lang=%s)", LangSubdir(lang));
    else
        logging::Debug("[TTS] モデル作成完了 (lang=%s provider=%s)", LangSubdir(lang), cfg.model.provider);
    return handle;
}

static SherpaOnnxOfflineTts* GetModel(Lang lang) {
    std::lock_guard<std::mutex> lock(g_modelMutex);
    auto& entry = g_models[lang];
    if (!entry.tried) {
        entry.tried  = true;
        entry.handle = CreateModel(lang);
    }
    return entry.handle;
}

// ============================================================
// TTS ワーカースレッド
// ============================================================

static constexpr float  kPlaybackSpeed = 1.05f;  // 再生速度 (1.0=等速)

// 合成入力キュー (最新上書き)
static std::thread             g_synthThread;
static std::mutex              g_ttsMutex;      // g_latestText 保護
static std::condition_variable g_ttsCv;
static std::string             g_latestText;    // 次の発話テキスト (上書き最新)
static std::atomic<bool>       g_ttsRunning{false};
static std::string             g_ttsLanguage = "auto";

// 合成済みキュー (SynthWorker → PlayWorker)
struct ReadyItem { std::string text; PcmData pcm; };
static constexpr size_t        kReadyQueueMax = 2;
static std::queue<ReadyItem>   g_readyQueue;
static std::mutex              g_readyMutex;
static std::condition_variable g_readyCv;
static std::thread             g_playThread;

static std::mutex  g_speakingMutex;             // g_currentSpeakingText 保護
static std::string g_currentSpeakingText;        // 現在発話中テキスト (overlay ハイライト用)

// ============================================================
// XAudio2 再生ヘルパー (PlayWorker 専用)
// ============================================================

static void PlayPcmXAudio2(IXAudio2* xaudio, const PcmData& pcm) {
    WAVEFORMATEX wfx = {};
    wfx.wFormatTag      = WAVE_FORMAT_PCM;
    wfx.nChannels       = 1;
    wfx.nSamplesPerSec  = static_cast<DWORD>(pcm.sampleRate);
    wfx.wBitsPerSample  = 16;
    wfx.nBlockAlign     = 2;
    wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * 2;

    IXAudio2SourceVoice* sv = nullptr;
    HRESULT hr = xaudio->CreateSourceVoice(&sv, &wfx);
    if (FAILED(hr) || !sv) {
        logging::Debug("[TTS] CreateSourceVoice 失敗: hr=0x%08X", hr);
        return;
    }

    XAUDIO2_BUFFER buf = {};
    buf.AudioBytes = static_cast<DWORD>(pcm.samples.size() * 2);
    buf.pAudioData = reinterpret_cast<const BYTE*>(pcm.samples.data());
    buf.Flags      = XAUDIO2_END_OF_STREAM;

    sv->SubmitSourceBuffer(&buf);
    sv->SetVolume(config::Get().ttsVolume);
    sv->SetFrequencyRatio(kPlaybackSpeed);
    sv->Start(0);

    for (;;) {
        if (!g_ttsRunning.load()) { sv->Stop(0); break; }
        XAUDIO2_VOICE_STATE state;
        sv->GetState(&state);
        if (state.BuffersQueued == 0) break;
        Sleep(50);
    }
    sv->DestroyVoice();
}

// ============================================================
// PlayWorker: 合成済み PCM を受け取り XAudio2 で再生する
// ============================================================

static void PlayWorker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IXAudio2* xaudio = nullptr;
    HRESULT hr = XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        logging::Debug("[TTS] PlayWorker XAudio2Create 失敗: 0x%08X", hr);
        CoUninitialize();
        return;
    }

    IXAudio2MasteringVoice* masterVoice = nullptr;
    xaudio->CreateMasteringVoice(&masterVoice);
    logging::Debug("[TTS] PlayWorker 起動");

    while (g_ttsRunning.load()) {
        ReadyItem item;
        {
            std::unique_lock<std::mutex> lk(g_readyMutex);
            g_readyCv.wait(lk, [] { return !g_readyQueue.empty() || !g_ttsRunning.load(); });
            if (!g_ttsRunning.load() && g_readyQueue.empty()) break;
            if (g_readyQueue.empty()) continue;
            item = std::move(g_readyQueue.front());
            g_readyQueue.pop();
            g_readyCv.notify_one(); // SynthWorker にキュー空きを通知
        }

        // ハイライト ON → PCM は合成済みなのでこの直後に音が出る
        { std::lock_guard<std::mutex> lk(g_speakingMutex); g_currentSpeakingText = item.text; }

        logging::Debug("[TTS] 再生開始: bytes=%zu text=%.60s",
                       item.pcm.samples.size() * 2, item.text.c_str());
        PlayPcmXAudio2(xaudio, item.pcm);

        { std::lock_guard<std::mutex> lk(g_speakingMutex); g_currentSpeakingText.clear(); }
    }

    if (masterVoice) masterVoice->DestroyVoice();
    if (xaudio)      xaudio->Release();
    logging::Debug("[TTS] PlayWorker 終了");
    CoUninitialize();
}

// ============================================================
// SynthWorker: テキストを合成し ReadyQueue に積む
// ============================================================

static void SynthWorker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    logging::Debug("[TTS] SynthWorker 起動");

    // Init() から移動: ゲーム起動後にローダーロック競合なしでロード
    if (!g_sherpaLib) LoadSherpaLib(g_ttsDir);
    if (!g_vvReady)   InitVoicevox(g_ttsDir);
    tts_install::StartIfNeeded(g_ttsDir, g_ttsLanguage);

    while (g_ttsRunning.load()) {
        std::string text;
        {
            std::unique_lock<std::mutex> lock(g_ttsMutex);
            bool hasText = g_ttsCv.wait_for(lock, std::chrono::seconds(30),
                [] { return !g_latestText.empty() || !g_ttsRunning.load(); });
            if (!g_ttsRunning.load()) break;
            if (!hasText) {
                // タイムアウト: インストール完了後に各エンジンを再試行
                if (!g_sherpaLib) {
                    if (LoadSherpaLib(g_ttsDir))
                        logging::Debug("[TTS] Sherpa-ONNX ロード完了");
                }
                if (!g_vvReady) {
                    if (InitVoicevox(g_ttsDir))
                        logging::Debug("[TTS] VOICEVOX 初期化完了、ずんだもんに切り替えます");
                }
                TryLoadVvm15();
                continue;
            }
            text = std::move(g_latestText);
            g_latestText.clear();
        }

        logging::Debug("[TTS] 合成開始: bytes=%zu text=%.60s",
                       text.size(), text.c_str());

        // 言語判定
        Lang lang = Lang::EN;
        if (g_ttsLanguage == "auto") {
            lang = DetectLanguage(text.c_str());
        } else if (g_ttsLanguage == "ja") { lang = Lang::JA;
        } else if (g_ttsLanguage == "en") { lang = Lang::EN;
        } else if (g_ttsLanguage == "ru") { lang = Lang::RU;
        } else if (g_ttsLanguage == "zh") { lang = Lang::ZH;
        } else if (g_ttsLanguage == "ko") { lang = Lang::KO;
        } else {
            lang = DetectLanguage(text.c_str());
        }

        static const char* kLangName[] = {"EN", "RU", "KO", "ZH", "JA"};
        const char* langStr = kLangName[static_cast<int>(lang)];

        // VOICEVOX (日本語) — JA/JAZ モード共通。JAZ=ずんだもん (style=3)、JA=青山龍星 (style=13)
        TranslationMode tlMode = config::GetTranslationMode();
        // 純漢字テキスト(了解・南下・防衛等)が ZH と誤判定される問題を補正
        if (g_ttsLanguage == "auto" && lang == Lang::ZH &&
            (tlMode == TranslationMode::JA || tlMode == TranslationMode::JAZ))
            lang = Lang::JA;

        // TTS 読み辞書を適用 (英単語 → 各言語の読み)
        static const char* kLangKey[] = {"en", "ru", "ko", "zh", "ja"};
        const char* langKey = kLangKey[static_cast<int>(lang)];
        std::string processedText = ApplyTtsReadings(text, langKey);
        if (processedText != text)
            logging::Debug("[TTS] 読み辞書適用: %s → %s", text.c_str(), processedText.c_str());

        bool useVoicevox = (lang == Lang::JA && g_vvReady);
        if (config::Get().ttsVerboseLog)
            logging::Debug("[TTS] エンジン選択: %s (lang=%s vvReady=%d tlMode=%d)",
                           useVoicevox ? "VOICEVOX" : "Sherpa-ONNX",
                           langStr, (int)g_vvReady, (int)tlMode);

        PcmData pcm;
        if (useVoicevox) {
            // JAZ=ずんだもん / JA=青山龍星 (15.vvm 未ロード時はずんだもんにフォールバック)
            uint32_t vvStyle = (tlMode == TranslationMode::JAZ || !g_vv15Ready) ? g_vvStyleId : g_vvJaStyleId;
            if (!SynthesizeVoicevox(processedText, pcm, vvStyle)) continue;
        } else {
            // モデル取得 (遅延初期化)
            SherpaOnnxOfflineTts* model = GetModel(lang);
            if (!model) {
                // espeak-ng VITS (EN/RU/KO) がスキップ済みの場合、JA Supertonic-3 で代替する
                logging::Debug("[TTS] lang=%s モデルなし、JA Supertonic-3 (多言語) でフォールバック", langStr);
                model = GetModel(Lang::JA);
            }
            if (!model) {
                logging::Debug("[TTS] 利用可能なモデルなし");
                continue;
            }

            // VITS モデルはアテンション機構の限界を超えた長文で同一箇所ループが発生する
            // 文境界を優先して 180 文字以内に切り詰める
            static const size_t kMaxTtsChars = 180;
            std::string ttsText = processedText;
            if (ttsText.size() > kMaxTtsChars) {
                static const char kBoundary[] = ".!?;,\n";
                size_t cut = std::string::npos;
                for (size_t i = kMaxTtsChars; i > kMaxTtsChars / 2; --i) {
                    if (strchr(kBoundary, ttsText[i])) { cut = i + 1; break; }
                }
                ttsText = ttsText.substr(0, cut != std::string::npos ? cut : kMaxTtsChars);
                logging::Debug("[TTS] テキスト切り詰め: %zu -> %zu bytes",
                               text.size(), ttsText.size());
            }

            if (config::Get().ttsVerboseLog)
                logging::Debug("[TTS] Sherpa合成開始: bytes=%zu lang=%s",
                               ttsText.size(), langStr);

            DWORD tGenStart = GetTickCount();
            DWORD excCode2  = 0;
            const SherpaOnnxGeneratedAudio* audio = TryGenerate(model, ttsText.c_str(), &excCode2);
            DWORD tGenMs = GetTickCount() - tGenStart;
            if (excCode2) {
                logging::Debug("[TTS] Sherpa合成例外 code=0x%08X lang=%s text=%.40s",
                               excCode2, langStr, ttsText.c_str());
                continue;
            }
            if (!audio || audio->n <= 0) {
                if (audio) g_fnFreeAudio(audio);
                logging::Debug("[TTS] 合成失敗またはサンプルなし (lang=%s)", langStr);
                continue;
            }
            if (config::Get().ttsVerboseLog)
                logging::Debug("[TTS] Sherpa合成完了: samples=%d rate=%d time=%lums",
                               audio->n, audio->sample_rate, (unsigned long)tGenMs);

            // float32 PCM [-1,1] → int16
            pcm.sampleRate = audio->sample_rate;
            pcm.samples.resize(audio->n);
            for (int i = 0; i < audio->n; i++) {
                float v = audio->samples[i];
                if (v >  1.0f) v =  1.0f;
                if (v < -1.0f) v = -1.0f;
                pcm.samples[i] = static_cast<int16_t>(v * 32767.0f);
            }
            g_fnFreeAudio(audio);
        }

        if (!g_ttsRunning.load()) break;

        // ReadyQueue が満杯なら PlayWorker がひとつ消化するまで待機
        {
            std::unique_lock<std::mutex> lk(g_readyMutex);
            g_readyCv.wait(lk, [] {
                return g_readyQueue.size() < kReadyQueueMax || !g_ttsRunning.load();
            });
            if (!g_ttsRunning.load()) break;
            g_readyQueue.push({text, std::move(pcm)});
            g_readyCv.notify_one();
        }
    }

    logging::Debug("[TTS] SynthWorker 終了");
}

// ============================================================
// 公開 API
// ============================================================

void tts::Init(const char* language, uint32_t voicevoxStyleId, uint32_t voicevoxJaStyleId) {
    if (g_ttsRunning.load()) return;

    g_ttsLanguage  = (language && *language) ? language : "auto";
    g_vvStyleId    = voicevoxStyleId;
    g_vvJaStyleId  = voicevoxJaStyleId;

    g_ttsDir = GetTtsToolDir();
    logging::Debug("[TTS] Init (language=%s, vvStyleId=%u, vvJaStyleId=%u, dir=%s)",
                   g_ttsLanguage.c_str(), voicevoxStyleId, voicevoxJaStyleId, g_ttsDir.c_str());

    // g_ttsDir は "path\tools\tts\" — "tools\tts\" を除いて DLL ディレクトリを得る
    static const char kSuffix[] = "tools\\tts\\";
    std::string baseDir = g_ttsDir;
    if (baseDir.size() >= sizeof(kSuffix) - 1 &&
        baseDir.compare(baseDir.size() - (sizeof(kSuffix)-1), sizeof(kSuffix)-1, kSuffix) == 0)
        baseDir.resize(baseDir.size() - (sizeof(kSuffix) - 1));
    LoadTtsReadings(baseDir + "tts_readings.txt");

    // DLL ロードは SynthWorker 内で行う（ゲーム起動のローダーロック競合を避けるため）
    g_ttsRunning.store(true);
    g_synthThread = std::thread(SynthWorker);
    g_playThread  = std::thread(PlayWorker);
}

void tts::SetLatest(const char* textUtf8) {
    if (!textUtf8 || !*textUtf8 || !g_ttsRunning.load()) return;
    logging::Debug("[TTS] SetLatest: bytes=%zu text=%.60s", strlen(textUtf8), textUtf8);
    { std::lock_guard<std::mutex> lk(g_ttsMutex); g_latestText = textUtf8; }
    g_ttsCv.notify_one();
}

std::string tts::GetSpeakingText() {
    std::lock_guard<std::mutex> lk(g_speakingMutex);
    return g_currentSpeakingText;
}

void tts::DetachThread() {
    // DLL_PROCESS_DETACH (プロセス終了) 専用: ~std::thread が std::terminate を呼ばないよう detach する
    if (g_synthThread.joinable()) g_synthThread.detach();
    if (g_playThread.joinable())  g_playThread.detach();
}

void tts::Shutdown() {
    if (!g_ttsRunning.load()) return;
    g_ttsRunning.store(false);
    g_ttsCv.notify_one();    // SynthWorker を起床
    g_readyCv.notify_all();  // PlayWorker を起床
    if (g_synthThread.joinable()) g_synthThread.join();
    if (g_playThread.joinable())  g_playThread.join();

    // モデル解放
    {
        std::lock_guard<std::mutex> lock(g_modelMutex);
        for (auto& kv : g_models) {
            if (kv.second.handle && g_fnDestroy)
                g_fnDestroy(kv.second.handle);
        }
        g_models.clear();
    }

    if (g_sherpaLib) {
        FreeLibrary(g_sherpaLib);
        g_sherpaLib   = nullptr;
        g_fnCreate    = nullptr;
        g_fnDestroy   = nullptr;
        g_fnGenerate  = nullptr;
        g_fnFreeAudio = nullptr;
    }

    // VOICEVOX クリーンアップ
    if (g_vvReady) {
        g_vvReady = false;
        if (g_vvSynthesizer) { g_vvSynthDel(g_vvSynthesizer); g_vvSynthesizer = nullptr; }
        if (g_vvOpenJtalk)   { g_vvJtalkDel(g_vvOpenJtalk);   g_vvOpenJtalk   = nullptr; }
        if (g_vvLib)         { FreeLibrary(g_vvLib);           g_vvLib         = nullptr; }
    }

    logging::Debug("[TTS] Shutdown 完了");
}
