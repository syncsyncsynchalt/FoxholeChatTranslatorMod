// ============================================================
// tts.cpp - ニューラルTTS (Sherpa-ONNX Piper VITS)
//
// sherpa-onnx.dll を tools/tts/ から動的ロード
// Piper VITS モデルで自然な発音を実現
// 再生: XAudio2, エフェクト: バンドパスDSP (変更なし)
// ============================================================

#include "tts.h"
#include "log.h"

#include <windows.h>
#include <xaudio2.h>

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

// ============================================================
// Sherpa-ONNX C API 構造体 (v1.10.x ABI 互換)
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

struct SherpaOnnxOfflineTtsModelConfig {
    SherpaOnnxOfflineTtsVitsModelConfig vits;
    int32_t     num_threads;
    int32_t     debug;
    const char* provider;
};

struct SherpaOnnxOfflineTtsConfig {
    SherpaOnnxOfflineTtsModelConfig model;
    const char* rule_fsts;
    int32_t     max_num_sentences;
    const char* rule_fars;
};

struct SherpaOnnxGeneratedAudio {
    const float* samples;
    int32_t      num_samples;
    int32_t      sample_rate;
};

struct SherpaOnnxOfflineTts; // opaque

typedef SherpaOnnxOfflineTts*           (*PFN_CreateTts)(const SherpaOnnxOfflineTtsConfig*);
typedef void                            (*PFN_DestroyTts)(SherpaOnnxOfflineTts*);
typedef const SherpaOnnxGeneratedAudio* (*PFN_Generate)(const SherpaOnnxOfflineTts*, const char*, int32_t, float);
typedef void                            (*PFN_DestroyAudio)(const SherpaOnnxGeneratedAudio*);

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
// 無線ラジオ風オーディオエフェクト (バンドパス + クリッピング)
// ============================================================

struct BiquadCoeffs { double b0, b1, b2, a1, a2; };
struct BiquadState  { double x1=0, x2=0, y1=0, y2=0; };

static BiquadCoeffs DesignHPF(double fc, double fs) {
    const double pi = 3.14159265358979323846;
    double w0    = 2.0 * pi * fc / fs;
    double alpha = sin(w0) / (2.0 * 0.7071067811865476);
    double cosw0 = cos(w0);
    double a0    = 1.0 + alpha;
    return { (1+cosw0)/2/a0, -(1+cosw0)/a0, (1+cosw0)/2/a0,
             -2*cosw0/a0, (1-alpha)/a0 };
}

static BiquadCoeffs DesignLPF(double fc, double fs) {
    const double pi = 3.14159265358979323846;
    double w0    = 2.0 * pi * fc / fs;
    double alpha = sin(w0) / (2.0 * 0.7071067811865476);
    double cosw0 = cos(w0);
    double a0    = 1.0 + alpha;
    return { (1-cosw0)/2/a0, (1-cosw0)/a0, (1-cosw0)/2/a0,
             -2*cosw0/a0, (1-alpha)/a0 };
}

static void ApplyRadioEffect(BYTE* dataChunk, DWORD dataSize, const WAVEFORMATEX& wfx) {
    if (wfx.wFormatTag != WAVE_FORMAT_PCM || wfx.wBitsPerSample != 16 || !wfx.nSamplesPerSec) return;

    int16_t* s16    = reinterpret_cast<int16_t*>(dataChunk);
    size_t   n      = dataSize / 2;
    int      ch     = wfx.nChannels;
    size_t   frames = n / ch;
    double   fs     = static_cast<double>(wfx.nSamplesPerSec);

    std::vector<float> buf(n);
    for (size_t i = 0; i < n; i++) buf[i] = s16[i] / 32768.0f;

    BiquadCoeffs hpf = DesignHPF(300.0,  fs);
    BiquadCoeffs lpf = DesignLPF(3400.0, fs);

    for (int c = 0; c < ch; c++) {
        BiquadState hs, ls;
        for (size_t f = 0; f < frames; f++) {
            float* p = &buf[f * ch + c];
            double y = hpf.b0 * *p + hpf.b1*hs.x1 + hpf.b2*hs.x2 - hpf.a1*hs.y1 - hpf.a2*hs.y2;
            hs.x2=hs.x1; hs.x1=*p;  hs.y2=hs.y1; hs.y1=y;
            double z = lpf.b0 * y   + lpf.b1*ls.x1 + lpf.b2*ls.x2 - lpf.a1*ls.y1 - lpf.a2*ls.y2;
            ls.x2=ls.x1; ls.x1=y;   ls.y2=ls.y1; ls.y1=z;
            *p = static_cast<float>(z);
        }
    }

    const float kClip = 0.85f;
    for (size_t i = 0; i < n; i++) {
        float v = buf[i] * 1.4f;
        v = v > kClip ? kClip : (v < -kClip ? -kClip : v);
        buf[i] = v / kClip;
    }
    for (size_t i = 0; i < n; i++) {
        float v = buf[i];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        s16[i] = static_cast<int16_t>(v * 32767.0f);
    }
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

static bool LoadSherpaLib(const std::string& ttsDir) {
    std::string dllPath = ttsDir + "sherpa-onnx.dll";

    // DLL とその依存 (onnxruntime.dll 等) を同じディレクトリから探す
    g_sherpaLib = LoadLibraryExA(dllPath.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_sherpaLib) {
        logging::Debug("[TTS] sherpa-onnx.dll ロード失敗: %s (err=%lu)", dllPath.c_str(), GetLastError());
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

static SherpaOnnxOfflineTts* CreateModel(Lang lang) {
    std::string modelDir  = g_ttsDir + "models\\" + LangSubdir(lang) + "\\";
    std::string modelPath = modelDir + "model.onnx";
    std::string tokensPath = modelDir + "tokens.txt";
    std::string espeakDir = g_ttsDir + "espeak-ng-data";

    if (GetFileAttributesA(modelPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        logging::Debug("[TTS] モデルなし: %s", modelPath.c_str());
        return nullptr;
    }

    // 文字列をローカル変数で保持 (CreateOfflineTts が内部でコピーする)
    std::string lexiconPath;
    std::string dictDir;

    // Chinese: jieba 辞書があれば指定
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
    cfg.max_num_sentences        = 1;

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
    std::string sender;
};

static constexpr size_t MAX_TTS_QUEUE_SIZE = 8;

static std::thread             g_ttsThread;
static std::mutex              g_ttsMutex;
static std::condition_variable g_ttsCv;
static std::queue<TtsRequest>  g_ttsQueue;
static std::atomic<bool>       g_ttsRunning{false};
static std::atomic<bool>       g_ttsStop{false};
static std::string             g_ttsLanguage = "auto";
static float                   g_speakingRate = 1.0f;
static bool                    g_radioEffect  = true;

static void TtsWorker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    IXAudio2* xaudio = nullptr;
    HRESULT hr = XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        logging::Debug("[TTS] XAudio2Create 失敗: 0x%08X", hr);
        return;
    }

    IXAudio2MasteringVoice* masterVoice = nullptr;
    xaudio->CreateMasteringVoice(&masterVoice);

    logging::Debug("[TTS] ワーカー起動");

    while (g_ttsRunning.load()) {
        TtsRequest req;
        {
            std::unique_lock<std::mutex> lock(g_ttsMutex);
            g_ttsCv.wait(lock, [] { return !g_ttsQueue.empty() || !g_ttsRunning.load(); });
            if (!g_ttsRunning.load()) break;
            req = g_ttsQueue.front();
            g_ttsQueue.pop();
        }

        g_ttsStop.store(false);

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

        // モデル取得 (遅延初期化)
        SherpaOnnxOfflineTts* model = GetModel(lang);
        if (!model && lang != Lang::EN)
            model = GetModel(Lang::EN); // 英語でフォールバック
        if (!model) {
            logging::Debug("[TTS] 利用可能なモデルなし");
            continue;
        }

        if (g_ttsStop.load()) continue;

        // 音声合成: speed=1.0 は標準速度
        const SherpaOnnxGeneratedAudio* audio = g_fnGenerate(model, req.text.c_str(), 0, g_speakingRate);
        if (!audio || audio->num_samples <= 0) {
            if (audio) g_fnFreeAudio(audio);
            logging::Debug("[TTS] 合成失敗またはサンプルなし");
            continue;
        }

        if (g_ttsStop.load()) {
            g_fnFreeAudio(audio);
            continue;
        }

        // float32 PCM [-1,1] → int16
        int sampleRate  = audio->sample_rate;
        int numSamples  = audio->num_samples;
        std::vector<int16_t> pcm16(numSamples);
        for (int i = 0; i < numSamples; i++) {
            float v = audio->samples[i];
            if (v >  1.0f) v =  1.0f;
            if (v < -1.0f) v = -1.0f;
            pcm16[i] = static_cast<int16_t>(v * 32767.0f);
        }
        g_fnFreeAudio(audio);

        // 無線ラジオ風エフェクト
        WAVEFORMATEX wfx = {};
        wfx.wFormatTag      = WAVE_FORMAT_PCM;
        wfx.nChannels       = 1;
        wfx.nSamplesPerSec  = static_cast<DWORD>(sampleRate);
        wfx.wBitsPerSample  = 16;
        wfx.nBlockAlign     = 2; // 1ch * 16bit / 8
        wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;

        DWORD dataSize = static_cast<DWORD>(pcm16.size() * 2);
        if (g_radioEffect)
            ApplyRadioEffect(reinterpret_cast<BYTE*>(pcm16.data()), dataSize, wfx);

        // XAudio2 再生
        IXAudio2SourceVoice* sourceVoice = nullptr;
        hr = xaudio->CreateSourceVoice(&sourceVoice, &wfx);
        if (FAILED(hr) || !sourceVoice) continue;

        XAUDIO2_BUFFER buf = {};
        buf.AudioBytes = dataSize;
        buf.pAudioData = reinterpret_cast<const BYTE*>(pcm16.data());
        buf.Flags      = XAUDIO2_END_OF_STREAM;

        sourceVoice->SubmitSourceBuffer(&buf);
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
}

// ============================================================
// 公開 API
// ============================================================

void tts::Init(const char* language, double speakingRate, bool radioEffect) {
    if (g_ttsRunning.load()) return;

    g_ttsLanguage  = (language && *language) ? language : "auto";
    g_speakingRate = (speakingRate >= 0.5 && speakingRate <= 2.0)
                     ? static_cast<float>(speakingRate) : 1.0f;
    g_radioEffect  = radioEffect;

    g_ttsDir = GetTtsToolDir();
    logging::Debug("[TTS] Init (language=%s, rate=%.2f, radio=%d, dir=%s)",
                   g_ttsLanguage.c_str(), g_speakingRate, (int)radioEffect, g_ttsDir.c_str());

    if (!LoadSherpaLib(g_ttsDir)) {
        logging::Debug("[TTS] sherpa-onnx.dll が見つかりません。setup_tts.ps1 を実行してください。");
        return;
    }

    g_ttsRunning.store(true);
    g_ttsThread = std::thread(TtsWorker);
}

void tts::Speak(const char* textUtf8, const char* /*senderUtf8*/) {
    if (!textUtf8 || !*textUtf8 || !g_ttsRunning.load()) return;
    {
        std::lock_guard<std::mutex> lock(g_ttsMutex);
        if (g_ttsQueue.size() >= MAX_TTS_QUEUE_SIZE) {
            logging::Debug("[TTS] キュー満杯: 最古を破棄");
            g_ttsQueue.pop();
        }
        TtsRequest req;
        req.text = textUtf8;
        g_ttsQueue.push(std::move(req));
    }
    g_ttsCv.notify_one();
}

void tts::Stop() {
    g_ttsStop.store(true);
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
            if (kv.second.handle)
                g_fnDestroy(kv.second.handle);
        }
        g_models.clear();
    }

    if (g_sherpaLib) {
        FreeLibrary(g_sherpaLib);
        g_sherpaLib  = nullptr;
        g_fnCreate   = nullptr;
        g_fnDestroy  = nullptr;
        g_fnGenerate = nullptr;
        g_fnFreeAudio = nullptr;
    }

    logging::Debug("[TTS] Shutdown 完了");
}
