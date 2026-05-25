// ============================================================
// tts.cpp - 多言語TTS (Sherpa-ONNX / VOICEVOX Core)
//
// sherpa-onnx.dll / voicevox_core.dll を tools/tts/ から動的ロード
// 日本語: VOICEVOX (ずんだもん), その他: Sherpa-ONNX Piper/Supertonic
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
#include <string>
#include <thread>
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
static uint32_t              g_vvStyleId    = 3;

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
    // パッケージによって sherpa-onnx.dll か sherpa-onnx-c-api.dll の場合がある
    const char* candidates[] = { "sherpa-onnx.dll", "sherpa-onnx-c-api.dll" };
    std::string dllPath;
    for (auto* name : candidates) {
        dllPath = ttsDir + name;
        g_sherpaLib = LoadLibraryExA(dllPath.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (g_sherpaLib) break;
        logging::Debug("[TTS] %s ロード失敗 (err=%lu)", name, GetLastError());
    }
    if (!g_sherpaLib) {
        logging::Debug("[TTS] sherpa-onnx DLL が見つかりません: %s", ttsDir.c_str());
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

    logging::Debug("[TTS] sherpa-onnx.dll ロード完了");
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
    initOpts.acceleration_mode = 1; // CPU
    initOpts.cpu_num_threads   = 2;
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

    logging::Debug("[TTS-VV] 初期化完了: %d VVM, styleId=%u", loaded, g_vvStyleId);
    g_vvReady = true;
    return true;
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

static bool SynthesizeVoicevox(const std::string& text, PcmData& out) {
    VoicevoxTtsOptions opts = g_vvDefTtsOpt ? g_vvDefTtsOpt() : VoicevoxTtsOptions{false};
    opts.enable_interrogative_upspeak = false;

    uintptr_t wavLen = 0;
    uint8_t*  wavBuf = nullptr;
    auto rc = g_vvTts(g_vvSynthesizer, text.c_str(), g_vvStyleId, opts, &wavLen, &wavBuf);
    if (rc != VOICEVOX_RESULT_OK || !wavBuf) {
        logging::Debug("[TTS-VV] 合成失敗 rc=%d", rc);
        return false;
    }
    bool ok = ParseWavPcm(wavBuf, wavLen, out);
    g_vvWavFree(wavBuf);
    if (!ok) logging::Debug("[TTS-VV] WAV 解析失敗");
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
    cfg.model.num_threads   = 2;
    cfg.model.debug         = 0;
    cfg.model.provider      = "cpu";
    cfg.max_num_sentences   = 1;

    SherpaOnnxOfflineTts* handle = g_fnCreate(&cfg);
    if (!handle)
        logging::Debug("[TTS] Supertonic モデル作成失敗: %s", modelDir.c_str());
    else
        logging::Debug("[TTS] Supertonic モデル作成完了: %s", modelDir.c_str());
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

    SherpaOnnxOfflineTtsConfig cfg = {};
    cfg.model.vits.model         = modelPath.c_str();
    cfg.model.vits.tokens        = tokensPath.c_str();
    cfg.model.vits.lexicon       = lexiconPath.empty() ? nullptr : lexiconPath.c_str();
    cfg.model.vits.dict_dir      = dictDir.empty() ? nullptr : dictDir.c_str();
    cfg.model.vits.data_dir      = (hasEspeak && lang != Lang::ZH) ? espeakDir.c_str() : nullptr;
    cfg.model.vits.noise_scale   = 0.667f;
    cfg.model.vits.noise_scale_w = 0.8f;
    cfg.model.vits.length_scale  = 1.0f;
    cfg.model.num_threads        = 2;
    cfg.model.debug              = 0;
    cfg.model.provider           = "cpu";
    cfg.max_num_sentences        = 5;

    SherpaOnnxOfflineTts* handle = g_fnCreate(&cfg);
    if (!handle)
        logging::Debug("[TTS] モデル作成失敗 (lang=%s)", LangSubdir(lang));
    else
        logging::Debug("[TTS] モデル作成完了 (lang=%s)", LangSubdir(lang));
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

struct TtsRequest {
    std::string text;
};

static constexpr size_t MAX_TTS_QUEUE_SIZE = 2;
static constexpr float  kPlaybackSpeed     = 1.05f;  // 再生速度 (1.0=等速)

static std::thread             g_ttsThread;
static std::mutex              g_ttsMutex;
static std::condition_variable g_ttsCv;
static std::queue<TtsRequest>  g_ttsQueue;
static std::atomic<bool>       g_ttsRunning{false};
static std::atomic<bool>       g_ttsStop{false};
static std::string             g_ttsLanguage = "auto";

static void TtsWorker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IXAudio2* xaudio = nullptr;
    HRESULT hr = XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        logging::Debug("[TTS] XAudio2Create 失敗: 0x%08X", hr);
        g_ttsRunning.store(false);
        return;
    }

    IXAudio2MasteringVoice* masterVoice = nullptr;
    xaudio->CreateMasteringVoice(&masterVoice);

    logging::Debug("[TTS] ワーカー起動");

    while (g_ttsRunning.load()) {
        TtsRequest req;
        {
            std::unique_lock<std::mutex> lock(g_ttsMutex);
            bool hasReq = g_ttsCv.wait_for(lock, std::chrono::seconds(30),
                [] { return !g_ttsQueue.empty() || !g_ttsRunning.load(); });
            if (!g_ttsRunning.load()) break;
            if (!hasReq) {
                // タイムアウト: インストール完了後に各エンジンを再試行
                if (!g_sherpaLib) {
                    if (LoadSherpaLib(g_ttsDir))
                        logging::Debug("[TTS] Sherpa-ONNX ロード完了");
                }
                if (!g_vvReady) {
                    if (InitVoicevox(g_ttsDir))
                        logging::Debug("[TTS] VOICEVOX 初期化完了、ずんだもんに切り替えます");
                }
                continue;
            }
            req = g_ttsQueue.front();
            g_ttsQueue.pop();
        }

        g_ttsStop.store(false);
        logging::Debug("[TTS] ワーカー: 処理開始 text=%.60s", req.text.c_str());

        // 言語判定
        Lang lang = Lang::EN;
        if (g_ttsLanguage == "auto") {
            lang = DetectLanguage(req.text.c_str());
        } else if (g_ttsLanguage == "ja") { lang = Lang::JA;
        } else if (g_ttsLanguage == "en") { lang = Lang::EN;
        } else if (g_ttsLanguage == "ru") { lang = Lang::RU;
        } else if (g_ttsLanguage == "zh") { lang = Lang::ZH;
        } else if (g_ttsLanguage == "ko") { lang = Lang::KO;
        } else {
            lang = DetectLanguage(req.text.c_str());
        }

        // VOICEVOX (日本語ずんだもん) — JAZモード時のみ使用。JAモードはSherpa-ONNXを使う
        TranslationMode tlMode = config::GetTranslationMode();
        bool useVoicevox = (lang == Lang::JA && g_vvReady && tlMode == TranslationMode::JAZ);
        if (useVoicevox) {
            PcmData vvPcm;
            if (!SynthesizeVoicevox(req.text, vvPcm)) continue;
            logging::Debug("[TTS-VV] 合成完了: samples=%zu rate=%d",
                           vvPcm.samples.size(), vvPcm.sampleRate);
            if (g_ttsStop.load()) continue;

            WAVEFORMATEX wfx = {};
            wfx.wFormatTag      = WAVE_FORMAT_PCM;
            wfx.nChannels       = 1;
            wfx.nSamplesPerSec  = static_cast<DWORD>(vvPcm.sampleRate);
            wfx.wBitsPerSample  = 16;
            wfx.nBlockAlign     = 2;
            wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * 2;

            DWORD dataSize = static_cast<DWORD>(vvPcm.samples.size() * 2);

            IXAudio2SourceVoice* sourceVoice = nullptr;
            hr = xaudio->CreateSourceVoice(&sourceVoice, &wfx);
            if (FAILED(hr) || !sourceVoice) {
                logging::Debug("[TTS-VV] CreateSourceVoice 失敗: hr=0x%08X", hr);
                continue;
            }

            XAUDIO2_BUFFER buf = {};
            buf.AudioBytes = dataSize;
            buf.pAudioData = reinterpret_cast<const BYTE*>(vvPcm.samples.data());
            buf.Flags      = XAUDIO2_END_OF_STREAM;

            sourceVoice->SubmitSourceBuffer(&buf);
            sourceVoice->SetVolume(config::Get().ttsVolume);
            sourceVoice->SetFrequencyRatio(kPlaybackSpeed);
            sourceVoice->Start(0);
            logging::Debug("[TTS-VV] 再生開始: %u bytes", dataSize);

            for (;;) {
                if (g_ttsStop.load() || !g_ttsRunning.load()) {
                    sourceVoice->Stop(0);
                    break;
                }
                XAUDIO2_VOICE_STATE state;
                sourceVoice->GetState(&state);
                if (state.BuffersQueued == 0) break;
                Sleep(50);
            }
            sourceVoice->DestroyVoice();
            continue;
        }

        // モデル取得 (遅延初期化)
        SherpaOnnxOfflineTts* model = GetModel(lang);
        if (!model && lang != Lang::EN)
            model = GetModel(Lang::EN); // 英語でフォールバック
        if (!model) {
            logging::Debug("[TTS] 利用可能なモデルなし");
            continue;
        }

        if (g_ttsStop.load()) continue;

        // VITS モデルはアテンション機構の限界を超えた長文で同一箇所ループが発生する
        // 文境界を優先して 180 文字以内に切り詰める
        static const size_t kMaxTtsChars = 180;
        std::string ttsText = req.text;
        if (ttsText.size() > kMaxTtsChars) {
            static const char kBoundary[] = ".!?;,\n";
            size_t cut = std::string::npos;
            for (size_t i = kMaxTtsChars; i > kMaxTtsChars / 2; --i) {
                if (strchr(kBoundary, ttsText[i])) { cut = i + 1; break; }
            }
            ttsText = ttsText.substr(0, cut != std::string::npos ? cut : kMaxTtsChars);
        }

        const SherpaOnnxGeneratedAudio* audio = g_fnGenerate(model, ttsText.c_str(), 0, 1.0f);
        if (!audio || audio->n <= 0) {
            if (audio) g_fnFreeAudio(audio);
            logging::Debug("[TTS] 合成失敗またはサンプルなし");
            continue;
        }

        if (g_ttsStop.load()) {
            g_fnFreeAudio(audio);
            continue;
        }

        // float32 PCM [-1,1] → int16
        int32_t sampleRate = audio->sample_rate;
        int     numSamples = audio->n;
        std::vector<int16_t> pcm16(numSamples);
        for (int i = 0; i < numSamples; i++) {
            float v = audio->samples[i];
            if (v >  1.0f) v =  1.0f;
            if (v < -1.0f) v = -1.0f;
            pcm16[i] = static_cast<int16_t>(v * 32767.0f);
        }
        g_fnFreeAudio(audio);

        WAVEFORMATEX wfx = {};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = 1;
        wfx.nSamplesPerSec  = static_cast<DWORD>(sampleRate);
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = 2; // 1ch * 16bit / 8
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

        DWORD dataSize = static_cast<DWORD>(pcm16.size() * 2);

        // XAudio2 再生
        IXAudio2SourceVoice* sourceVoice = nullptr;
        hr = xaudio->CreateSourceVoice(&sourceVoice, &wfx);
        if (FAILED(hr) || !sourceVoice) continue;

        XAUDIO2_BUFFER buf = {};
        buf.AudioBytes = dataSize;
        buf.pAudioData = reinterpret_cast<const BYTE*>(pcm16.data());
        buf.Flags      = XAUDIO2_END_OF_STREAM;

        sourceVoice->SubmitSourceBuffer(&buf);
        sourceVoice->SetVolume(config::Get().ttsVolume);
        sourceVoice->SetFrequencyRatio(kPlaybackSpeed);
        sourceVoice->Start(0);

        for (;;) {
            if (g_ttsStop.load() || !g_ttsRunning.load()) {
                sourceVoice->Stop(0);
                break;
            }
            XAUDIO2_VOICE_STATE state;
            sourceVoice->GetState(&state);
            if (state.BuffersQueued == 0) break;
            Sleep(50);
        }

        sourceVoice->DestroyVoice();
    }

    if (masterVoice) masterVoice->DestroyVoice();
    if (xaudio)      xaudio->Release();
    logging::Debug("[TTS] ワーカー終了");
    CoUninitialize();
}

// ============================================================
// 公開 API
// ============================================================

void tts::Init(const char* language, uint32_t voicevoxStyleId) {
    if (g_ttsRunning.load()) return;

    g_ttsLanguage = (language && *language) ? language : "auto";
    g_vvStyleId   = voicevoxStyleId;

    g_ttsDir = GetTtsToolDir();
    logging::Debug("[TTS] Init (language=%s, vvStyleId=%u, dir=%s)",
                   g_ttsLanguage.c_str(), voicevoxStyleId, g_ttsDir.c_str());

    bool sherpaOk = LoadSherpaLib(g_ttsDir);
    InitVoicevox(g_ttsDir);

    // 不足コンポーネントをバックグラウンドで自動インストール
    tts_install::StartIfNeeded(g_ttsDir, g_ttsLanguage);

    if (!sherpaOk && !g_vvReady) {
        logging::Debug("[TTS] TTS エンジン未検出 - インストール完了後に自動ロード");
        // fall through: スレッドを起動して hot-reload ループに任せる
    }

    g_ttsRunning.store(true);
    g_ttsThread = std::thread(TtsWorker);
}

void tts::Speak(const char* textUtf8, const char* /*senderUtf8*/) {
    logging::Debug("[TTS] Speak: running=%d text=%.60s",
                   (int)g_ttsRunning.load(), textUtf8 ? textUtf8 : "(null)");
    if (!textUtf8 || !*textUtf8 || !g_ttsRunning.load()) return;
    {
        std::lock_guard<std::mutex> lock(g_ttsMutex);
        if (g_ttsQueue.size() >= MAX_TTS_QUEUE_SIZE) {
            logging::Debug("[TTS] キュー満杯: 最古を破棄");
            g_ttsQueue.pop();
        }
        g_ttsQueue.push({textUtf8});
    }
    g_ttsCv.notify_one();
}

void tts::Stop() {
    g_ttsStop.store(true);
}

void tts::DetachThread() {
    // DLL_PROCESS_DETACH (プロセス終了) 専用: ~std::thread が std::terminate を呼ばないよう detach する
    if (g_ttsThread.joinable()) g_ttsThread.detach();
}

void tts::Shutdown() {
    if (!g_ttsRunning.load()) return;
    g_ttsRunning.store(false);
    g_ttsCv.notify_one();
    if (g_ttsThread.joinable()) g_ttsThread.join();

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
