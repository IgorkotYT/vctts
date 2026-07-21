#include "app_state.h"
#include "win32_window.h"
#include "native_ui.h"
#include "keyboard_hook.h"
#include "controller.h"
#include "overlay_window.h"
#include "audio_devices.h"
#include "audio_playback.h"
#include "driver_setup.h"
#include "incoming_translator.h"
#include "mic_bridge.h"
#include "tts_keyless.h"
#include "tts_winrt.h"
#include "translator.h"
#include "custom_tts.h"
#include <windows.h>
#include <objbase.h>
#include <thread>
#include <vector>
#include <string>

static Controller* g_ctrl = nullptr;
static AppState*   g_state = nullptr;
static NativeUi*   g_ui = nullptr;
static constexpr int kToggleHotkeyId = 1;
static constexpr int kOpenConfigHotkeyId = 2;
static std::wstring SanitizeForSapi(const std::wstring& in)
{
    std::wstring out;
    for (wchar_t c : in) {
        if (c < 0x20 && c != L' ' && c != L'\n') continue;
        if (c >= 0xD800 && c <= 0xDFFF) continue;
        out.push_back(c);
    }
    return out;
}

static std::vector<std::uint8_t> SpeakWithFallback(const std::wstring& text, bool preferKeyless, const std::string& voiceLanguage = {})
{
    if (g_state &&
        g_state->sapiVoiceIndex >= 0 &&
        g_state->sapiVoiceIndex < (int)g_state->sapiVoices.size() &&
        g_state->sapiVoices[g_state->sapiVoiceIndex] == L"Custom")
    {
        return custom_tts::SpeakCustomCommand(text, g_state->customTtsCommand);
    }

    if (preferKeyless)
    {
        auto audio = tts_keyless::speak_to_audio_memory(text);
        if (!audio.empty()) return audio;
        return tts_winrt::speak_wav_with_language(text, voiceLanguage);
    }

    auto audio = tts_winrt::speak_wav_with_language(text, voiceLanguage);
    if (!audio.empty()) return audio;
    return tts_keyless::speak_to_audio_memory(text);
}

static void OnCommittedText(const std::wstring& text)
{
    if (!g_state || text.empty()) return;

    std::wstring copy = text;
    const bool preferKeyless = g_state->useKeylessBackup.load();
    const bool translate = g_state->translatorMode.load();
    const std::string targetLang = g_state->translatorTargetLang;
    std::thread([copy, preferKeyless, translate, targetLang]() {
        std::wstring textToSpeak = SanitizeForSapi(copy);
        std::string voiceLang;
        if (translate)
        {
            auto translated = translator::translate_auto(textToSpeak, targetLang);
            if (!translated.text.empty())
            {
                textToSpeak = SanitizeForSapi(translated.text);
                voiceLang = targetLang;
            }
        }

        auto wav = SpeakWithFallback(textToSpeak, preferKeyless, voiceLang);
        if (wav.empty()) {
            MessageBoxW(nullptr, L"TTS produced an invalid/empty audio buffer.", L"TTS Error", MB_ICONERROR);
            return;
        }
        if (g_state->micBridgeEnabled.load()) {
            if (!mic_bridge::submit_tts(wav, *g_state)) {
                OutputDebugStringW(L"[MicBridge] Failed to submit TTS; falling back to selected playback devices.\n");
                audio_playback::play_wav_to_selected_async(wav, *g_state);
                return;
            }
            audio_playback::play_wav_to_device_async(wav, g_state->devB);
        } else {
            audio_playback::play_wav_to_selected_async(wav, *g_state);
        }
    }).detach();
}

static void HookToggle(AppState&) { if (g_ctrl) g_ctrl->toggle_recording(); }
static void HookStop(AppState&)   { if (g_ctrl) g_ctrl->stop_recording(); }
static void HookAppend(AppState&, const wchar_t* t, int n) { if (g_ctrl) g_ctrl->on_append_text(t, n); }
static void HookBackspace(AppState&) { if (g_ctrl) g_ctrl->on_backspace(); }
static void HookCancel(AppState&) { if (g_ctrl) g_ctrl->cancel_recording(); }
static void HookOpenConfig(AppState& s)
{
    if (g_ctrl) g_ctrl->cancel_recording();
    s.clearBuffer();
    s.configDone.store(false);
    win32_window::show(s);
    win32_window::set_topmost(s, true);
}
static void HookExit(AppState& s) { s.exitRequested.store(true); }

static bool MainMsgHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& outResult)
{
    if (msg == WM_HOTKEY && wParam == kToggleHotkeyId)
    {
        if (g_ctrl) g_ctrl->toggle_recording();
        outResult = 0;
        return true;
    }

    if (msg == WM_HOTKEY && wParam == kOpenConfigHotkeyId)
    {
        if (g_state) HookOpenConfig(*g_state);
        outResult = 0;
        return true;
    }

    if (g_ui && g_ui->handle_message(hWnd, msg, wParam, lParam, outResult))
        return true;

    return false;
}


int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR commandLine, int)
{
    if (commandLine && lstrcmpW(commandLine, driver_setup::kElevatedInstallArgument) == 0)
        return static_cast<int>(driver_setup::install_driver_elevated());

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    AppState state;
    g_state = &state;

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    constexpr const wchar_t* kClassName = L"TTS_OVERLAY_NATIVE";
    constexpr const wchar_t* kTitle     = L"TTS Voice Typing";
    const int dpi = (int)GetDpiForSystem();
    const int windowW = MulDiv(880, dpi, 96);
    const int windowH = MulDiv(780, dpi, 96);

    if (!win32_window::create(state, hInstance, kClassName, kTitle, 560, 320, windowW, windowH))
    {
        MessageBoxW(nullptr, L"Failed to create window.", L"Error", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    if (!overlay_window::create(state, hInstance))
    {
        MessageBoxW(nullptr, L"Failed to create overlay window.", L"Error", MB_ICONERROR);
        win32_window::destroy(state, hInstance, kClassName);
        CoUninitialize();
        return 1;
    }

    win32_window::set_msg_handler(MainMsgHandler);

    RefreshOutputDevices(state);
    driver_setup::refresh_status(state);

    NativeUi ui;
    ui.init(state.hwnd, state);
    g_ui = &ui;

    win32_window::show(state);
    win32_window::set_topmost(state, true);

    ControllerCallbacks cbs{};
    cbs.onCommittedText = &OnCommittedText;
    Controller ctrl(state, cbs);
    g_ctrl = &ctrl;

    HookCallbacks hcb{};
    hcb.onToggleRecording = &HookToggle;
    hcb.onStopRecording   = &HookStop;
    hcb.onAppendText      = &HookAppend;
    hcb.onBackspace       = &HookBackspace;
    hcb.onCancelRecording = &HookCancel;
    hcb.onOpenConfig      = &HookOpenConfig;
    hcb.onExit            = &HookExit;

    if (!keyboard_hook::install(state, hcb))
    {
        MessageBoxW(nullptr, L"Failed to install keyboard hook.", L"Error", MB_ICONERROR);
        state.exitRequested.store(true);
    }

    if (!RegisterHotKey(state.hwnd, kToggleHotkeyId, MOD_CONTROL | MOD_NOREPEAT, VK_BACK))
    {
        OutputDebugStringW(L"[Hotkey] RegisterHotKey failed for Ctrl+Backspace; low-level hook fallback remains active.\n");
    }
    if (!RegisterHotKey(state.hwnd, kOpenConfigHotkeyId, MOD_CONTROL | MOD_SHIFT | MOD_NOREPEAT, VK_BACK))
    {
        OutputDebugStringW(L"[Hotkey] RegisterHotKey failed for Ctrl+Shift+Backspace; low-level hook fallback remains active.\n");
    }
    incoming_translator::start(state);

    MSG msg{};
    while (!state.exitRequested.load())
    {
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT)
                state.exitRequested.store(true);
        }
        keyboard_hook::poll_modifiers(state);
        if (state.exitRequested.load())
            break;

        ctrl.tick_timeout();
        overlay_window::tick(state);
        mic_bridge::sync(state);


        if (!IsWindowVisible(state.hwnd))
        {
            Sleep(10);
            continue;
        }

        UiAction action = ui.tick();

        if (action == UiAction::Quit)
        {
            state.exitRequested.store(true);
        }
        else if (action == UiAction::StartFromConfig)
        {
            win32_window::hide(state);
        }
        else if (action == UiAction::StopRecording)
        {
            ctrl.stop_recording();
        }
        else if (action == UiAction::TestTone)
        {
            audio_playback::play_test_tone_async(state);
        }
        else if (action == UiAction::RefreshDevices)
        {
            RefreshOutputDevices(state);
            driver_setup::refresh_status(state);
        }
        else if (action == UiAction::OpenConfig)
        {
            state.recording.store(false);
            state.clearBuffer();
            state.configDone.store(false);
            win32_window::show(state);
            win32_window::set_topmost(state, true);
        }
        else if (action == UiAction::TestTts)
        {
            std::thread([]{
                if (!g_state) return;

                const bool preferKeyless = g_state->useKeylessBackup.load();
                std::string targetLang = g_state->translatorTargetLang;
                std::wstring testText = L"Test text to speech.";
                std::string voiceLang;
                if (g_state->translatorMode.load())
                {
                    auto translated = translator::translate_auto(testText, targetLang);
                    if (!translated.text.empty())
                    {
                        testText = translated.text;
                        voiceLang = targetLang;
                    }
                }
                auto audio = SpeakWithFallback(testText, preferKeyless, voiceLang);

                wchar_t buf[256];
                swprintf_s(buf, L"TTS bytes: %llu (preferKeyless=%s)",
                           (unsigned long long)audio.size(),
                           preferKeyless ? L"true" : L"false");
                OutputDebugStringW(L"[TTS] ");
                OutputDebugStringW(buf);
                OutputDebugStringW(L"\n");

                // Quick signature check for common failure modes
                if (!audio.empty())
                {
                    // MP3 often starts with "ID3" or 0xFF 0xFB (frame sync)
                    const bool looksMp3 =
                        (audio.size() >= 3 && audio[0] == 'I' && audio[1] == 'D' && audio[2] == '3') ||
                        (audio.size() >= 2 && audio[0] == 0xFF && (audio[1] & 0xE0) == 0xE0);

                    // WAV starts with "RIFF"
                    const bool looksWav =
                        (audio.size() >= 4 && audio[0] == 'R' && audio[1] == 'I' && audio[2] == 'F' && audio[3] == 'F');

                    if (!looksWav && !looksMp3)
                        OutputDebugStringW(L"[TTS] Warning: audio does not look like WAV or MP3 (maybe HTML error response)\n");
                }

                if (audio.empty()) {
                    MessageBoxW(nullptr, L"TTS produced an invalid/empty audio buffer. Check DebugView output.", L"TTS Error", MB_ICONERROR);
                    return;
                }

                if (g_state->micBridgeEnabled.load()) {
                    if (mic_bridge::submit_tts(audio, *g_state))
                        audio_playback::play_wav_to_device_async(audio, g_state->devB);
                    else
                        audio_playback::play_wav_to_selected_async(audio, *g_state);
                } else {
                    audio_playback::play_wav_to_selected_async(audio, *g_state);
                }
            }).detach();
        }

        Sleep(16);
    }

    g_ui = nullptr;
    ui.shutdown();
    incoming_translator::stop();
    UnregisterHotKey(state.hwnd, kToggleHotkeyId);
    UnregisterHotKey(state.hwnd, kOpenConfigHotkeyId);
    keyboard_hook::uninstall();
    mic_bridge::stop();
    audio_playback::stop_all();
    overlay_window::destroy(hInstance);
    win32_window::destroy(state, hInstance, kClassName);

    CoUninitialize();
    return 0;
}
