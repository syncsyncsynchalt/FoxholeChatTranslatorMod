// ============================================================
// tts.cpp - 多言語TTS読み上げ (Windows OneCore / WinRT)
// Windows.Media.SpeechSynthesis + XAudio2 で非同期再生
// ============================================================

#include "tts.h"
#include "log.h"

#include <windows.h>
#include <roapi.h>
#include <hstring.h>
#include <winstring.h>

// WinRT Speech headers
#include <windows.media.speechsynthesis.h>
#include <windows.storage.streams.h>

// XAudio2 for playback
#include <xaudio2.h>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

using namespace Microsoft::WRL;
using namespace Microsoft::WRL::Wrappers;
using namespace ABI::Windows::Media::SpeechSynthesis;
using namespace ABI::Windows::Storage::Streams;
using namespace ABI::Windows::Foundation;

// ============================================================
// 言語判定
// ============================================================

enum class Lang { EN, RU, KO, ZH, JA, UNKNOWN };

static Lang DetectLanguage(const char* textUtf8) {
    // UTF-8 をデコードして最初の非ASCII文字の範囲で判定
    const unsigned char* p = reinterpret_cast<const unsigned char*>(textUtf8);
    int cyrillic = 0, hangul = 0, cjk = 0, hiragana = 0, katakana = 0, latin = 0;
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

        if (cp >= 0x0400 && cp <= 0x04FF) cyrillic++;
        else if (cp >= 0xAC00 && cp <= 0xD7AF) hangul++;
        else if (cp >= 0x3040 && cp <= 0x309F) hiragana++;
        else if (cp >= 0x30A0 && cp <= 0x30FF) katakana++;
        else if (cp >= 0x4E00 && cp <= 0x9FFF) cjk++;
        else if (cp >= 0x41 && cp <= 0x7A) latin++;
    }

    if (cyrillic > 0) return Lang::RU;
    if (hangul > 0) return Lang::KO;
    if (hiragana > 0 || katakana > 0) return Lang::JA;
    if (cjk > 0) return Lang::ZH;
    return Lang::EN;
}

static const wchar_t* LangToVoiceTag(Lang lang) {
    switch (lang) {
    case Lang::EN: return L"en-US";
    case Lang::RU: return L"ru-RU";
    case Lang::KO: return L"ko-KR";
    case Lang::ZH: return L"zh-CN";
    case Lang::JA: return L"ja-JP";
    default:       return L"en-US";
    }
}

// ============================================================
// UTF-8 → wstring 変換
// ============================================================

static std::wstring Utf8ToWide(const char* utf8) {
    if (!utf8 || !*utf8) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    std::wstring result(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &result[0], len);
    return result;
}

// ============================================================
// SSML 生成
// ============================================================

// SSML の XML 特殊文字をエスケープ
static std::wstring EscapeXml(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size() + 32);
    for (wchar_t c : text) {
        switch (c) {
        case L'&':  out += L"&amp;";  break;
        case L'<':  out += L"&lt;";   break;
        case L'>':  out += L"&gt;";   break;
        case L'"':  out += L"&quot;"; break;
        case L'\'': out += L"&apos;"; break;
        default:    out += c;         break;
        }
    }
    return out;
}

// テキストを SSML でラップする
// sender のハッシュ値からピッチとレートを変化させて人ごとに個性を付ける
static std::wstring BuildSsml(const std::wstring& text, const wchar_t* langTag, const std::string& sender) {
    // ピッチ: -20% ~ +20% の 9 段階
    static const wchar_t* kPitches[] = {
        L"-20%", L"-15%", L"-10%", L"-5%", L"0%", L"+5%", L"+10%", L"+15%", L"+20%"
    };
    // レート: 95% ~ 115% の 5 段階。kSpeakingRate を乗算して最終レートを決定する。
    // SSML の <prosody rate> が WinRT の SpeakingRate プロパティより優先されるため、
    // SpeakingRate は SSML 側に直接反映させる (tts_test.py の apply_speaking_rate と同一ロジック)。
    static const int kRates[] = { 95, 100, 105, 110, 115 };
    static const double kSpeakingRate = 1.1;

    const wchar_t* pitch = L"0%";
    wchar_t rateBuf[16];
    if (!sender.empty()) {
        std::hash<std::string> hasher;
        size_t h = hasher(sender);
        pitch = kPitches[h % 9];
        int r = static_cast<int>(kRates[(h >> 8) % 5] * kSpeakingRate + 0.5);
        swprintf_s(rateBuf, L"%d%%", r);
    } else {
        // sender なし: 100% * kSpeakingRate
        swprintf_s(rateBuf, L"%d%%", static_cast<int>(100 * kSpeakingRate + 0.5));
    }

    std::wstring ssml;
    ssml  = L"<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='";
    ssml += langTag;
    ssml += L"'><prosody pitch='";
    ssml += pitch;
    ssml += L"' rate='";
    ssml += rateBuf;
    ssml += L"'>";
    ssml += EscapeXml(text);
    ssml += L"</prosody></speak>";
    return ssml;
}

// ============================================================
// TTS ワーカースレッド
// ============================================================

struct TtsRequest {
    std::string text;
    std::string sender; // 空の場合はランダム声色
};

static std::thread          g_ttsThread;
static std::mutex           g_ttsMutex;
static std::condition_variable g_ttsCv;
static std::queue<TtsRequest> g_ttsQueue;
static std::atomic<bool>    g_ttsRunning{false};
static std::atomic<bool>    g_ttsStop{false};
// "auto" の場合はテキストから自動判定、それ以外は固定言語
static std::string          g_ttsLanguage = "auto";

static void TtsWorker() {
    // ゲームのレンダリングを邪魔しないよう優先度を下げる
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);

    // COM / WinRT 初期化
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(hr)) {
        logging::Debug("[TTS] RoInitialize 失敗: 0x%08X", hr);
        return;
    }

    // SpeechSynthesizer 作成
    ComPtr<ISpeechSynthesizer> synth;
    {
        HStringReference className(RuntimeClass_Windows_Media_SpeechSynthesis_SpeechSynthesizer);
        ComPtr<IInspectable> inspectable;
        hr = RoActivateInstance(className.Get(), &inspectable);
        if (FAILED(hr)) {
            logging::Debug("[TTS] SpeechSynthesizer 作成失敗: 0x%08X", hr);
            RoUninitialize();
            return;
        }
        inspectable.As(&synth);
    }

    // 利用可能な音声一覧を取得
    ComPtr<IInstalledVoicesStatic> voicesStatic;
    {
        HStringReference className(RuntimeClass_Windows_Media_SpeechSynthesis_SpeechSynthesizer);
        ComPtr<IActivationFactory> factory;
        RoGetActivationFactory(className.Get(), IID_PPV_ARGS(&factory));
        factory.As(&voicesStatic);
    }

    ComPtr<__FIVectorView_1_Windows__CMedia__CSpeechSynthesis__CVoiceInformation> voices;
    if (voicesStatic) {
        voicesStatic->get_AllVoices(&voices);
        if (voices) {
            unsigned count = 0;
            voices->get_Size(&count);
            for (unsigned i = 0; i < count; i++) {
                ComPtr<IVoiceInformation> vi;
                voices->GetAt(i, &vi);
                HSTRING name;
                vi->get_DisplayName(&name);
                HSTRING lang;
                vi->get_Language(&lang);
                logging::Debug("[TTS] 音声: %ls (%ls)",
                    WindowsGetStringRawBuffer(name, nullptr),
                    WindowsGetStringRawBuffer(lang, nullptr));
            }
        }
    }

    // XAudio2 初期化
    ComPtr<IXAudio2> xaudio;
    hr = XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
        logging::Debug("[TTS] XAudio2Create 失敗: 0x%08X", hr);
        RoUninitialize();
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
        const std::string& text = req.text;

        g_ttsStop.store(false);

        // 言語判定 (設定で強制指定されている場合はそちらを優先)
        Lang lang;
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
        const wchar_t* langTag = LangToVoiceTag(lang);

        // 対応する音声を選択 (sender指定時はハッシュで固定、なければランダム)
        // Natural (ニューラル) 音声を優先的に選択する
        if (voices) {
            std::vector<unsigned> candidates;
            std::vector<unsigned> naturalCandidates;
            unsigned count = 0;
            voices->get_Size(&count);
            for (unsigned i = 0; i < count; i++) {
                ComPtr<IVoiceInformation> vi;
                voices->GetAt(i, &vi);
                HSTRING voiceLang;
                vi->get_Language(&voiceLang);
                const wchar_t* voiceLangStr = WindowsGetStringRawBuffer(voiceLang, nullptr);
                if (voiceLangStr && _wcsnicmp(voiceLangStr, langTag, wcslen(langTag)) == 0) {
                    candidates.push_back(i);
                    // "Natural" を含む音声はニューラルTTS (高品質)
                    HSTRING name;
                    vi->get_DisplayName(&name);
                    const wchar_t* nameStr = WindowsGetStringRawBuffer(name, nullptr);
                    if (nameStr && wcsstr(nameStr, L"Natural")) {
                        naturalCandidates.push_back(i);
                    }
                }
            }
            // Natural音声が利用可能ならそちらを優先、なければ全候補にフォールバック
            const std::vector<unsigned>& pool = naturalCandidates.empty() ? candidates : naturalCandidates;
            if (!pool.empty()) {
                unsigned pick;
                if (!req.sender.empty()) {
                    // sender名のハッシュで声色を決定論的に選択
                    std::hash<std::string> hasher;
                    pick = pool[hasher(req.sender) % pool.size()];
                } else {
                    static std::mt19937 rng(std::random_device{}());
                    pick = pool[rng() % pool.size()];
                }
                ComPtr<IVoiceInformation> vi;
                voices->GetAt(pick, &vi);
                synth->put_Voice(vi.Get());
                HSTRING name;
                vi->get_DisplayName(&name);
                logging::Debug("[TTS] 音声選択: %ls (sender=%s, natural=%s)",
                    WindowsGetStringRawBuffer(name, nullptr),
                    req.sender.empty() ? "(none)" : req.sender.c_str(),
                    naturalCandidates.empty() ? "no" : "yes");
            }
        }

        // SSML で合成 (xml:lang 明示によりニューラル音声の韻律処理精度が向上)
        std::wstring wtext = Utf8ToWide(text.c_str());
        std::wstring ssml = BuildSsml(wtext, langTag, req.sender);
        HStringReference ssmlRef(ssml.c_str());

        ComPtr<IAsyncOperation<SpeechSynthesisStream*>> asyncOp;
        hr = synth->SynthesizeSsmlToStreamAsync(ssmlRef.Get(), &asyncOp);
        if (FAILED(hr)) {
            logging::Debug("[TTS] SynthesizeSsmlToStreamAsync 失敗: 0x%08X", hr);
            continue;
        }

        // 合成完了を待つ
        ComPtr<ISpeechSynthesisStream> synthStream;
        {
            ComPtr<IAsyncInfo> asyncInfo;
            asyncOp->QueryInterface(IID_PPV_ARGS(&asyncInfo));
            DWORD start = GetTickCount();
            bool ok = false;
            while (GetTickCount() - start < 15000) {
                AsyncStatus status;
                asyncInfo->get_Status(&status);
                if (status == AsyncStatus::Completed) {
                    asyncOp->GetResults(&synthStream);
                    ok = true;
                    break;
                }
                if (status != AsyncStatus::Started) break;
                Sleep(10);
            }
            if (!ok || !synthStream) {
                logging::Debug("[TTS] 合成待ち失敗");
                continue;
            }
        }

        if (g_ttsStop.load()) continue;

        // ストリームからPCMデータ読み取り
        ComPtr<IInputStream> inputStream;
        synthStream->QueryInterface(IID_PPV_ARGS(&inputStream));

        ComPtr<IRandomAccessStream> ras;
        synthStream.As(&ras);
        UINT64 streamSize = 0;
        ras->get_Size(&streamSize);

        if (streamSize == 0) continue;

        // DataReader で読み取り
        ComPtr<IDataReaderFactory> drFactory;
        {
            HStringReference className(RuntimeClass_Windows_Storage_Streams_DataReader);
            ComPtr<IActivationFactory> factory;
            RoGetActivationFactory(className.Get(), IID_PPV_ARGS(&factory));
            factory.As(&drFactory);
        }

        ComPtr<IDataReader> reader;
        drFactory->CreateDataReader(inputStream.Get(), &reader);

        ComPtr<IAsyncOperation<UINT32>> loadOp;
        reader->LoadAsync(static_cast<UINT32>(streamSize), &loadOp);

        UINT32 bytesLoaded = 0;
        {
            ComPtr<IAsyncInfo> asyncInfo;
            loadOp->QueryInterface(IID_PPV_ARGS(&asyncInfo));
            DWORD start = GetTickCount();
            for (;;) {
                AsyncStatus status;
                asyncInfo->get_Status(&status);
                if (status == AsyncStatus::Completed) {
                    loadOp->GetResults(&bytesLoaded);
                    break;
                }
                if (status != AsyncStatus::Started || GetTickCount() - start > 15000) break;
                Sleep(10);
            }
        }

        if (bytesLoaded == 0 || g_ttsStop.load()) continue;

        std::vector<BYTE> wavData(bytesLoaded);
        reader->ReadBytes(bytesLoaded, wavData.data());

        // WAVヘッダー解析 (SpeechSynthesisStream は WAV形式)
        if (bytesLoaded < 44) continue;
        const BYTE* data = wavData.data();

        // "RIFF" チェック
        if (memcmp(data, "RIFF", 4) != 0) continue;

        // "fmt " チャンク探索
        const BYTE* fmt = nullptr;
        const BYTE* dataChunk = nullptr;
        DWORD dataSize = 0;

        size_t pos = 12; // skip RIFF header
        while (pos + 8 <= bytesLoaded) {
            DWORD chunkSize = *reinterpret_cast<const DWORD*>(data + pos + 4);
            if (memcmp(data + pos, "fmt ", 4) == 0) {
                fmt = data + pos + 8;
            }
            if (memcmp(data + pos, "data", 4) == 0) {
                dataChunk = data + pos + 8;
                dataSize = chunkSize;
                break;
            }
            pos += 8 + chunkSize;
            if (pos % 2) pos++; // word align
        }

        if (!fmt || !dataChunk || dataSize == 0) continue;

        // WAVEFORMATEX from fmt chunk
        WAVEFORMATEX wfx = {};
        memcpy(&wfx, fmt, sizeof(WAVEFORMATEX) < 18 ? sizeof(WAVEFORMATEX) : 18);

        // XAudio2 で再生
        IXAudio2SourceVoice* sourceVoice = nullptr;
        hr = xaudio->CreateSourceVoice(&sourceVoice, &wfx);
        if (FAILED(hr) || !sourceVoice) continue;

        XAUDIO2_BUFFER buf = {};
        buf.AudioBytes = dataSize;
        buf.pAudioData = dataChunk;
        buf.Flags = XAUDIO2_END_OF_STREAM;

        sourceVoice->SubmitSourceBuffer(&buf);
        sourceVoice->Start(0);

        // 再生完了を待つ
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
    xaudio.Reset();
    RoUninitialize();
    logging::Debug("[TTS] ワーカー終了");
}

// ============================================================
// 公開 API
// ============================================================

void tts::Init(const char* language) {
    if (g_ttsRunning.load()) return;
    g_ttsLanguage = (language && *language) ? language : "auto";
    logging::Debug("[TTS] Init (language=%s)", g_ttsLanguage.c_str());
    g_ttsRunning.store(true);
    g_ttsThread = std::thread(TtsWorker);
}

void tts::Speak(const char* textUtf8, const char* senderUtf8) {
    if (!textUtf8 || !*textUtf8 || !g_ttsRunning.load()) return;
    {
        std::lock_guard<std::mutex> lock(g_ttsMutex);
        TtsRequest req;
        req.text = textUtf8;
        if (senderUtf8 && *senderUtf8) req.sender = senderUtf8;
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
    logging::Debug("[TTS] Shutdown");
}
