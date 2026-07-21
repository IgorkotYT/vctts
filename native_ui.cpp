#include "native_ui.h"

#include "audio_devices.h"
#include "custom_tts.h"
#include "driver_setup.h"
#include "tts_winrt.h"

#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwctype>

namespace
{
    using namespace Gdiplus;

    constexpr float kPad = 24.0f;
    constexpr float kRadius = 10.0f;
    constexpr float kItemH = 38.0f;
    constexpr float kGap = 12.0f;

    Color bg() { return Color(255, 17, 19, 24); }
    Color panel() { return Color(255, 27, 31, 39); }
    Color panel2() { return Color(255, 34, 39, 49); }
    Color panel3() { return Color(255, 42, 48, 61); }
    Color border() { return Color(255, 66, 76, 94); }
    Color border_hot() { return Color(255, 92, 114, 145); }
    Color text() { return Color(255, 236, 241, 247); }
    Color muted() { return Color(255, 150, 160, 177); }
    Color dim() { return Color(255, 111, 122, 140); }
    Color accent() { return Color(255, 104, 132, 245); }
    Color accent2() { return Color(255, 72, 103, 222); }
    Color success() { return Color(255, 74, 194, 132); }
    Color danger() { return Color(255, 238, 101, 101); }

    RectF rf(const NativeUi::Rect& r)
    {
        return RectF(r.x, r.y, r.w, r.h);
    }

    bool contains(const NativeUi::Rect& r, float x, float y)
    {
        return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
    }

    NativeUi::Rect inset(NativeUi::Rect r, float dx, float dy)
    {
        r.x += dx;
        r.y += dy;
        r.w -= dx * 2.0f;
        r.h -= dy * 2.0f;
        return r;
    }

    NativeUi::Rect rect(float x, float y, float w, float h)
    {
        return NativeUi::Rect{ x, y, w, h };
    }

    void build_round_path(GraphicsPath& path, NativeUi::Rect r, float radius)
    {
        const float d = std::max(0.0f, radius * 2.0f);
        if (d <= 0.0f) {
            path.AddRectangle(rf(r));
            return;
        }
        path.AddArc(r.x, r.y, d, d, 180.0f, 90.0f);
        path.AddArc(r.x + r.w - d, r.y, d, d, 270.0f, 90.0f);
        path.AddArc(r.x + r.w - d, r.y + r.h - d, d, d, 0.0f, 90.0f);
        path.AddArc(r.x, r.y + r.h - d, d, d, 90.0f, 90.0f);
        path.CloseFigure();
    }

    void fill_round(Graphics& g, NativeUi::Rect r, float radius, Color c)
    {
        SolidBrush brush(c);
        GraphicsPath path(FillModeAlternate);
        build_round_path(path, r, radius);
        g.FillPath(&brush, &path);
    }

    void stroke_round(Graphics& g, NativeUi::Rect r, float radius, Color c, float width = 1.0f)
    {
        Pen pen(c, width);
        GraphicsPath path(FillModeAlternate);
        build_round_path(path, r, radius);
        g.DrawPath(&pen, &path);
    }

    void draw_string(Graphics& g, const std::wstring& s, NativeUi::Rect r, float size, Color c,
                     int style = FontStyleRegular, StringAlignment align = StringAlignmentNear,
                     StringAlignment lineAlign = StringAlignmentCenter)
    {
        FontFamily family(L"Segoe UI");
        Font font(&family, size, style, UnitPixel);
        SolidBrush brush(c);
        StringFormat fmt;
        fmt.SetAlignment(align);
        fmt.SetLineAlignment(lineAlign);
        fmt.SetTrimming(StringTrimmingEllipsisCharacter);
        fmt.SetFormatFlags(StringFormatFlagsNoWrap);
        g.DrawString(s.c_str(), (INT)s.size(), &font, rf(r), &fmt, &brush);
    }

    void draw_wrapped(Graphics& g, const std::wstring& s, NativeUi::Rect r, float size, Color c)
    {
        FontFamily family(L"Segoe UI");
        Font font(&family, size, FontStyleRegular, UnitPixel);
        SolidBrush brush(c);
        StringFormat fmt;
        fmt.SetTrimming(StringTrimmingEllipsisWord);
        g.DrawString(s.c_str(), (INT)s.size(), &font, rf(r), &fmt, &brush);
    }

    void draw_chevron(Graphics& g, NativeUi::Rect r, Color c)
    {
        Pen pen(c, 2.0f);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        const float cx = r.x + r.w * 0.5f;
        const float cy = r.y + r.h * 0.52f;
        g.DrawLine(&pen, cx - 5.0f, cy - 2.0f, cx, cy + 3.0f);
        g.DrawLine(&pen, cx, cy + 3.0f, cx + 5.0f, cy - 2.0f);
    }

    void draw_check(Graphics& g, NativeUi::Rect r, Color c)
    {
        Pen pen(c, 2.4f);
        pen.SetStartCap(LineCapRound);
        pen.SetEndCap(LineCapRound);
        g.DrawLine(&pen, r.x + 5.0f, r.y + r.h * 0.52f, r.x + 10.0f, r.y + r.h - 7.0f);
        g.DrawLine(&pen, r.x + 10.0f, r.y + r.h - 7.0f, r.x + r.w - 5.0f, r.y + 6.0f);
    }

    int text_vector_signature(const std::vector<std::wstring>& values)
    {
        unsigned int hash = 2166136261u;
        for (const std::wstring& value : values) {
            for (wchar_t ch : value) {
                hash ^= (unsigned int)ch;
                hash *= 16777619u;
            }
            hash ^= 0x9e3779b9u;
            hash *= 16777619u;
        }
        return (int)(hash ^ (unsigned int)values.size());
    }

    int device_vector_signature(const std::vector<AudioDevice>& values)
    {
        unsigned int hash = 2166136261u;
        for (const AudioDevice& value : values) {
            for (wchar_t ch : value.id + value.name) {
                hash ^= (unsigned int)ch;
                hash *= 16777619u;
            }
            hash ^= 0x9e3779b9u;
            hash *= 16777619u;
        }
        return (int)(hash ^ (unsigned int)values.size());
    }
}

bool NativeUi::init(HWND hwnd, AppState& state)
{
    hwnd_ = hwnd;
    state_ = &state;
    refresh_dpi();

    GdiplusStartupInput input{};
    if (GdiplusStartup(&gdiplusToken_, &input, nullptr) != Ok)
        return false;

    sync_model();
    return true;
}

void NativeUi::shutdown()
{
    if (gdiplusToken_) {
        GdiplusShutdown(gdiplusToken_);
        gdiplusToken_ = 0;
    }
    hwnd_ = nullptr;
    state_ = nullptr;
}

UiAction NativeUi::tick()
{
    if (!hwnd_ || !state_) return UiAction::None;

    const bool wantConfig = !state_->configDone.load();
    if (wantConfig != showingConfig_) {
        showingConfig_ = wantConfig;
        reset_interaction();
        SetWindowTextW(hwnd_, showingConfig_ ? L"TTS Voice Typing - Config" : L"Voice Typing");
    }

    sync_model();
    invalidate();

    UiAction action = pendingAction_;
    pendingAction_ = UiAction::None;
    return action;
}

bool NativeUi::handle_message(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LRESULT& outResult)
{
    if (hwnd != hwnd_ || !state_) return false;

    switch (msg)
    {
    case WM_SIZE:
        invalidate();
        outResult = 0;
        return true;

    case WM_DPICHANGED:
    {
        refresh_dpi();
        RECT* suggested = reinterpret_cast<RECT*>(lParam);
        if (suggested) {
            SetWindowPos(hwnd_, nullptr,
                         suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        invalidate();
        outResult = 0;
        return true;
    }

    case WM_MOUSEMOVE:
    {
        mouse_.x = GET_X_LPARAM(lParam);
        mouse_.y = GET_Y_LPARAM(lParam);
        hot_ = hit_test((float)mouse_.x, (float)mouse_.y);
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&tme);
            trackingMouse_ = true;
        }
        invalidate();
        outResult = 0;
        return true;
    }

    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        mouse_ = { -10000, -10000 };
        hot_ = Hit::None;
        invalidate();
        outResult = 0;
        return true;

    case WM_LBUTTONDOWN:
        SetCapture(hwnd_);
        SetFocus(hwnd_);
        mouseDown_ = true;
        active_ = hit_test((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
        invalidate();
        outResult = 0;
        return true;

    case WM_LBUTTONUP:
    {
        if (mouseDown_) ReleaseCapture();
        mouseDown_ = false;
        Hit released = hit_test((float)GET_X_LPARAM(lParam), (float)GET_Y_LPARAM(lParam));
        if (released == active_) activate_hit(released);
        active_ = Hit::None;
        invalidate();
        outResult = 0;
        return true;
    }

    case WM_MOUSEWHEEL:
        if (dropdown_.open && dropdown_.itemCount > 0) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            dropdown_.selected = std::clamp(dropdown_.selected - (delta / WHEEL_DELTA), 0, dropdown_.itemCount - 1);
            invalidate();
            outResult = 0;
            return true;
        }
        break;

    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            POINT pt{};
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            Hit hit = hit_test((float)pt.x, (float)pt.y);
            if (hit == Hit::CustomCommand || hit == Hit::TargetLanguage || hit == Hit::IncomingApp) {
                SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
                outResult = TRUE;
                return true;
            }
            if (is_clickable(hit)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                outResult = TRUE;
                return true;
            }
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            outResult = TRUE;
            return true;
        }
        break;

    case WM_KEYDOWN:
        if (showingConfig_) {
            handle_keydown(wParam);
            outResult = 0;
            return true;
        }
        break;

    case WM_CHAR:
        if (showingConfig_) {
            handle_char((wchar_t)wParam);
            outResult = 0;
            return true;
        }
        break;

    case WM_ERASEBKGND:
        outResult = 1;
        return true;

    case WM_PAINT:
    {
        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd_, &ps);
        paint(hdc);
        EndPaint(hwnd_, &ps);
        outResult = 0;
        return true;
    }

    default:
        break;
    }

    return false;
}

int NativeUi::dropdown_visible_start() const
{
    if (!dropdown_.open || dropdown_.itemCount <= 7) return 0;
    return std::clamp(dropdown_.selected - 3, 0, dropdown_.itemCount - 7);
}

bool NativeUi::is_clickable(Hit hit) const
{
    return hit != Hit::None;
}

void NativeUi::refresh_dpi()
{
    if (!hwnd_) {
        dpiScale_ = 1.0f;
        return;
    }
    const UINT dpi = GetDpiForWindow(hwnd_);
    dpiScale_ = std::max(1.0f, dpi / 96.0f);
}

void NativeUi::sync_model()
{
    if (!state_) return;

    if (state_->sapiVoices.empty()) {
        state_->sapiVoices = tts_winrt::list_voices();
        state_->sapiVoices.push_back(L"Custom");
    }

    if (state_->devA < 0) state_->devA = 0;
    if (state_->devB < 0) state_->devB = 0;
    if (!state_->outDevices.empty()) {
        if (state_->devA >= (int)state_->outDevices.size()) state_->devA = 0;
        if (state_->devB >= (int)state_->outDevices.size()) state_->devB = 0;
        if (state_->bridgeVirtualOutDev >= (int)state_->outDevices.size()) state_->bridgeVirtualOutDev = 0;
    }
    if (state_->micDev < 0) state_->micDev = 0;
    if (state_->bridgeVirtualInDev < 0) state_->bridgeVirtualInDev = 0;
    if (!state_->inDevices.empty()) {
        if (state_->micDev >= (int)state_->inDevices.size()) state_->micDev = 0;
        if (state_->bridgeVirtualInDev >= (int)state_->inDevices.size()) state_->bridgeVirtualInDev = 0;
    }
    if (state_->sapiVoiceIndex < 0) state_->sapiVoiceIndex = 0;
    if (!state_->sapiVoices.empty() && state_->sapiVoiceIndex >= (int)state_->sapiVoices.size())
        state_->sapiVoiceIndex = 0;

    int devSig = device_vector_signature(state_->outDevices) ^ (device_vector_signature(state_->inDevices) << 1);
    int voiceSig = text_vector_signature(state_->sapiVoices);
    if (devSig != deviceSignature_ || voiceSig != voiceSignature_) {
        deviceSignature_ = devSig;
        voiceSignature_ = voiceSig;
        close_dropdown();
    }

    std::wstring fromState = custom_tts::Utf8ToWide(state_->customTtsCommand);
    if (!customInputFocused_ && customCommandWide_ != fromState)
        customCommandWide_ = fromState;

    std::wstring targetFromState = custom_tts::Utf8ToWide(state_->translatorTargetLang);
    if (!targetInputFocused_ && targetLangWide_ != targetFromState)
        targetLangWide_ = targetFromState;

    std::wstring appFromState = custom_tts::Utf8ToWide(state_->incomingAppExe);
    if (!incomingAppInputFocused_ && incomingAppWide_ != appFromState)
        incomingAppWide_ = appFromState;
}

void NativeUi::queue_action(UiAction action)
{
    pendingAction_ = action;
}

void NativeUi::reset_interaction()
{
    active_ = Hit::None;
    hot_ = Hit::None;
    mouseDown_ = false;
    customInputFocused_ = false;
    targetInputFocused_ = false;
    incomingAppInputFocused_ = false;
    close_dropdown();
}

void NativeUi::invalidate()
{
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void NativeUi::paint(HDC hdc)
{
    RECT wr{};
    GetClientRect(hwnd_, &wr);
    const int w = std::max(1L, wr.right - wr.left);
    const int h = std::max(1L, wr.bottom - wr.top);

    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    HGDIOBJ old = SelectObject(mem, bmp);

    Graphics g(mem);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);
    g.SetPixelOffsetMode(PixelOffsetModeDefault);
    g.ScaleTransform(dpiScale_, dpiScale_);

    Rect bounds = rect(0, 0, (float)w / dpiScale_, (float)h / dpiScale_);
    if (showingConfig_) paint_config(g, bounds);
    else paint_recording(g, bounds);

    BitBlt(hdc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void NativeUi::paint_config(Graphics& g, const Rect& bounds)
{
    fill_round(g, bounds, 0, bg());

    const float wide = bounds.w >= 720.0f ? 1.0f : 0.0f;
    Rect shell = inset(bounds, 18.0f, 16.0f);
    fill_round(g, shell, 18.0f, panel());
    stroke_round(g, shell, 18.0f, border());

    Rect header = rect(shell.x + kPad, shell.y + 18.0f, shell.w - kPad * 2.0f, 58.0f);
    draw_string(g, L"TTS Voice Typing", rect(header.x, header.y, header.w - 96.0f, 30.0f),
                25.0f, text(), FontStyleBold);
    draw_string(g, L"Fast dual-output speech for games and voice chat.",
                rect(header.x, header.y + 31.0f, header.w - 96.0f, 23.0f), 14.0f, muted());

    auto button = [&](Hit id, const std::wstring& label, Rect r, bool primary = false, bool enabled = true) {
        bool hot = enabled && hot_ == id;
        bool down = enabled && active_ == id && mouseDown_;
        Color fill = !enabled ? Color(255, 31, 35, 44)
                   : primary ? (down ? accent2() : hot ? Color(255, 118, 145, 250) : accent())
                   : down ? panel3() : hot ? Color(255, 47, 54, 68) : panel2();
        Color edge = primary ? Color(255, 129, 153, 252) : (hot ? border_hot() : border());
        fill_round(g, r, 8.0f, fill);
        stroke_round(g, r, 8.0f, edge);
        draw_string(g, label, r, 14.0f, enabled ? text() : dim(), FontStyleRegular,
                    StringAlignmentCenter, StringAlignmentCenter);
    };

    Rect quitR = rect(shell.x + shell.w - kPad - 78.0f, shell.y + 22.0f, 78.0f, 32.0f);
    button(Hit::Quit, L"Quit", quitR);

    Rect card = rect(shell.x + kPad, shell.y + 92.0f, shell.w - kPad * 2.0f, shell.h - 190.0f);
    fill_round(g, card, 14.0f, Color(255, 22, 25, 32));
    stroke_round(g, card, 14.0f, Color(255, 48, 56, 70));

    const float labelW = wide > 0.0f ? 112.0f : 0.0f;
    const float fieldX = card.x + 20.0f + labelW;
    const float fieldW = card.w - 40.0f - labelW;
    float y = card.y + 22.0f;

    auto label = [&](const std::wstring& s, float ly) {
        draw_string(g, s, rect(card.x + 20.0f, ly, std::max(90.0f, labelW - 14.0f), kItemH),
                    13.0f, muted());
    };

    auto dropdown = [&](Hit id, const std::wstring& labelText, const std::wstring& value, Rect r, bool enabled) {
        bool hot = enabled && hot_ == id;
        bool down = enabled && active_ == id && mouseDown_;
        Color fill = !enabled ? Color(255, 25, 28, 36) : down ? panel3() : hot ? Color(255, 37, 43, 55) : Color(255, 20, 23, 30);
        fill_round(g, r, 8.0f, fill);
        stroke_round(g, r, 8.0f, hot ? border_hot() : border());
        draw_string(g, value.empty() ? labelText : value, rect(r.x + 13.0f, r.y, r.w - 48.0f, r.h),
                    14.0f, enabled ? text() : dim());
        draw_chevron(g, rect(r.x + r.w - 36.0f, r.y + 8.0f, 22.0f, 22.0f), enabled ? muted() : dim());
    };

    auto checkbox = [&](Hit id, const std::wstring& labelText, Rect r, bool value) {
        bool hot = hot_ == id;
        Rect box = rect(r.x, r.y + 9.0f, 20.0f, 20.0f);
        fill_round(g, box, 5.0f, value ? accent() : Color(255, 20, 23, 30));
        stroke_round(g, box, 5.0f, hot ? border_hot() : border());
        if (value) draw_check(g, box, Color(255, 255, 255, 255));
        draw_string(g, labelText, rect(r.x + 30.0f, r.y, r.w - 30.0f, r.h), 14.0f, text());
    };

    std::vector<std::wstring> devices = device_names();
    std::vector<std::wstring> inputs = input_device_names();
    const bool hasDevices = !devices.empty();
    const bool hasInputs = !inputs.empty();
    std::wstring devA = hasDevices ? devices[state_->devA] : L"No output devices found";
    std::wstring devB = hasDevices ? devices[state_->devB] : L"No output devices found";

    label(L"Device A", y);
    Rect devAR = rect(fieldX, y, fieldW, kItemH);
    dropdown(Hit::DeviceA, L"Device A", devA, devAR, hasDevices);
    y += kItemH + kGap;

    label(L"Device B", y);
    Rect devBR = rect(fieldX, y, fieldW, kItemH);
    dropdown(Hit::DeviceB, L"Device B", devB, devBR, hasDevices);
    y += kItemH + 16.0f;

    button(Hit::Refresh, L"Refresh devices", rect(fieldX, y, 146.0f, 34.0f));
    checkbox(Hit::Keyless, L"Use online keyless fallback", rect(fieldX + 166.0f, y - 2.0f, fieldW - 166.0f, 38.0f),
             state_->useKeylessBackup.load());
    y += 48.0f;

    checkbox(Hit::TranslatorMode, L"Translator mode", rect(fieldX, y - 2.0f, 170.0f, 38.0f),
             state_->translatorMode.load());
    Rect langR = rect(fieldX + 188.0f, y, 118.0f, 34.0f);
    bool langHot = hot_ == Hit::TargetLanguage;
    fill_round(g, langR, 8.0f, Color(255, 20, 23, 30));
    stroke_round(g, langR, 8.0f, targetInputFocused_ ? accent() : langHot ? border_hot() : border(),
                 targetInputFocused_ ? 1.6f : 1.0f);
    draw_string(g, targetLangWide_.empty() ? L"es" : targetLangWide_,
                rect(langR.x + 12.0f, langR.y, langR.w - 24.0f, langR.h), 14.0f,
                targetLangWide_.empty() ? dim() : text());
    draw_string(g, L"target language", rect(langR.x + langR.w + 12.0f, langR.y, fieldW - 318.0f, langR.h),
                13.0f, muted());
    y += 50.0f;

    checkbox(Hit::IncomingMode, L"Incoming listen", rect(fieldX, y - 2.0f, 170.0f, 38.0f),
             state_->incomingTranslatorMode.load());
    Rect appR = rect(fieldX + 188.0f, y, std::min(210.0f, fieldW - 360.0f), 34.0f);
    bool appHot = hot_ == Hit::IncomingApp;
    fill_round(g, appR, 8.0f, Color(255, 20, 23, 30));
    stroke_round(g, appR, 8.0f, incomingAppInputFocused_ ? accent() : appHot ? border_hot() : border(),
                 incomingAppInputFocused_ ? 1.6f : 1.0f);
    draw_string(g, incomingAppWide_.empty() ? L"discord.exe" : incomingAppWide_,
                rect(appR.x + 12.0f, appR.y, appR.w - 24.0f, appR.h), 14.0f,
                incomingAppWide_.empty() ? dim() : text());
    checkbox(Hit::MuteIncoming, L"mute app", rect(appR.x + appR.w + 18.0f, y - 2.0f, 126.0f, 38.0f),
             state_->muteIncomingApp.load());
    y += 50.0f;

    label(L"Mic bridge", y);
    checkbox(Hit::MicBridge, L"enable", rect(fieldX, y - 2.0f, 100.0f, 38.0f),
             state_->micBridgeEnabled.load());
    checkbox(Hit::MicMute, L"mute mic", rect(fieldX + 112.0f, y - 2.0f, 116.0f, 38.0f),
             state_->micMuted.load());
    std::wstring bridgeStatus = state_->driverSetup.virtualPlaybackFound && state_->driverSetup.virtualCaptureFound
        ? L"virtual endpoints ready"
        : L"virtual endpoints missing";
    draw_string(g, bridgeStatus, rect(fieldX + 246.0f, y, fieldW - 246.0f, 34.0f), 13.0f,
                state_->driverSetup.virtualPlaybackFound && state_->driverSetup.virtualCaptureFound ? success() : danger());
    y += 42.0f;

    std::wstring mic = hasInputs ? inputs[state_->micDev] : L"No input devices found";
    std::wstring vout = hasDevices ? devices[state_->bridgeVirtualOutDev] : L"No output devices found";
    std::wstring vin = hasInputs ? inputs[state_->bridgeVirtualInDev] : L"No input devices found";
    dropdown(Hit::MicDevice, L"physical mic", mic, rect(fieldX, y, fieldW * 0.50f - 6.0f, kItemH), hasInputs);
    dropdown(Hit::BridgeVirtualOut, L"virtual output", vout, rect(fieldX + fieldW * 0.50f + 6.0f, y, fieldW * 0.50f - 6.0f, kItemH), hasDevices);
    y += kItemH + 8.0f;

    dropdown(Hit::BridgeVirtualIn, L"virtual mic", vin, rect(fieldX, y, fieldW * 0.50f - 6.0f, kItemH), hasInputs);
    button(Hit::InstallDriver, L"Install/Repair Driver", rect(fieldX + fieldW * 0.50f + 6.0f, y, 156.0f, 34.0f));
    button(Hit::SetDefaultMic, L"Set comms mic", rect(fieldX + fieldW * 0.50f + 174.0f, y, 126.0f, 34.0f), false, hasInputs);
    y += kItemH + 8.0f;

    std::wstring gains = L"mic " + std::to_wstring((int)(state_->micGain * 100.0f)) +
                         L"%   tts " + std::to_wstring((int)(state_->ttsGain * 100.0f)) + L"%";
    draw_string(g, gains, rect(fieldX, y, 154.0f, 30.0f), 13.0f, muted());
    button(Hit::MicGainDown, L"Mic -", rect(fieldX + 164.0f, y, 58.0f, 30.0f));
    button(Hit::MicGainUp, L"Mic +", rect(fieldX + 230.0f, y, 58.0f, 30.0f));
    button(Hit::TtsGainDown, L"TTS -", rect(fieldX + 304.0f, y, 58.0f, 30.0f));
    button(Hit::TtsGainUp, L"TTS +", rect(fieldX + 370.0f, y, 58.0f, 30.0f));
    y += 40.0f;

    std::vector<std::wstring> voices = voice_names();
    std::wstring voice = voices.empty() ? L"No voices found" : voices[state_->sapiVoiceIndex];
    label(L"SAPI voice", y);
    Rect voiceR = rect(fieldX, y, fieldW, kItemH);
    dropdown(Hit::Voice, L"SAPI voice", voice, voiceR, !voices.empty());
    y += kItemH + kGap;

    const bool custom = voice == L"Custom";
    if (custom) {
        label(L"Command", y);
        Rect inputR = rect(fieldX, y, fieldW, kItemH);
        bool hot = hot_ == Hit::CustomCommand;
        fill_round(g, inputR, 8.0f, Color(255, 20, 23, 30));
        stroke_round(g, inputR, 8.0f, customInputFocused_ ? accent() : hot ? border_hot() : border(),
                     customInputFocused_ ? 1.6f : 1.0f);
        std::wstring value = customCommandWide_.empty() ? L"C:\\path\\customtts.exe {text}" : customCommandWide_;
        draw_string(g, value, rect(inputR.x + 13.0f, inputR.y, inputR.w - 24.0f, inputR.h), 14.0f,
                    customCommandWide_.empty() ? dim() : text());
        y += kItemH + 4.0f;
        draw_string(g, L"Stdout must be playable WAV/MP3 bytes.", rect(fieldX, y, fieldW, 22.0f), 12.0f, dim());
    }

    if (!hasDevices) {
        draw_string(g, L"No output devices found. Press Refresh devices.",
                    rect(fieldX, card.y + card.h - 34.0f, fieldW, 24.0f), 13.0f, danger());
    }

    Rect footer = rect(shell.x + kPad, shell.y + shell.h - 58.0f, shell.w - kPad * 2.0f, 38.0f);
    draw_string(g, L"Ctrl+Backspace toggles  /  Enter stops  /  Ctrl+Shift+Tab+E exits",
                rect(footer.x, footer.y - 26.0f, footer.w, 22.0f), 13.0f, muted());
    button(Hit::Start, L"Start", rect(footer.x, footer.y, 96.0f, 36.0f), true, hasDevices);
    button(Hit::TestTone, L"Test Tone", rect(footer.x + 110.0f, footer.y, 116.0f, 36.0f));
    button(Hit::TestTts, L"Test TTS", rect(footer.x + 240.0f, footer.y, 108.0f, 36.0f));

    if (dropdown_.open) {
        std::vector<std::wstring> items =
            (dropdown_.owner == Hit::Voice) ? voices :
            (dropdown_.owner == Hit::MicDevice || dropdown_.owner == Hit::BridgeVirtualIn) ? inputs :
            devices;
        Rect pop = dropdown_.rect;
        pop.y += pop.h + 6.0f;
        pop.h = std::min(7, dropdown_.itemCount) * 34.0f + 8.0f;
        fill_round(g, pop, 10.0f, Color(255, 29, 33, 43));
        stroke_round(g, pop, 10.0f, border_hot());
        const int start = dropdown_visible_start();
        const int visible = std::min(7, dropdown_.itemCount);
        for (int row = 0; row < visible; ++row) {
            const int i = start + row;
            Rect ir = rect(pop.x + 5.0f, pop.y + 4.0f + row * 34.0f, pop.w - 10.0f, 32.0f);
            Hit itemHit = (Hit)((int)Hit::DropdownItemBase + i);
            if (hot_ == itemHit) fill_round(g, ir, 7.0f, panel3());
            if (i == dropdown_.selected) fill_round(g, rect(ir.x + 4.0f, ir.y + 7.0f, 3.0f, 18.0f), 2.0f, accent());
            draw_string(g, items[i], rect(ir.x + 14.0f, ir.y, ir.w - 18.0f, ir.h), 13.0f, text());
        }
        if (dropdown_.itemCount > 7) {
            std::wstring more = std::to_wstring(dropdown_.selected + 1) + L" / " + std::to_wstring(dropdown_.itemCount);
            draw_string(g, more, rect(pop.x + pop.w - 70.0f, pop.y + pop.h - 25.0f, 56.0f, 18.0f), 11.0f, dim(),
                        FontStyleRegular, StringAlignmentFar, StringAlignmentCenter);
        }
    }
}

void NativeUi::paint_recording(Graphics& g, const Rect& bounds)
{
    fill_round(g, bounds, 0, bg());
    Rect shell = inset(bounds, 18.0f, 16.0f);
    fill_round(g, shell, 18.0f, panel());
    stroke_round(g, shell, 18.0f, border());

    draw_string(g, L"Voice Typing", rect(shell.x + kPad, shell.y + 18.0f, shell.w - kPad * 2.0f, 30.0f),
                24.0f, text(), FontStyleBold);

    const bool recording = state_ && state_->recording.load();
    std::wstring status = recording ? L"Recording active" : L"Waiting";
    status += state_ && state_->useKeylessBackup.load() ? L"  /  keyless fallback on" : L"  /  keyless fallback off";
    draw_string(g, status, rect(shell.x + kPad, shell.y + 52.0f, shell.w - kPad * 2.0f, 24.0f), 13.0f,
                recording ? success() : muted());

    Rect preview = rect(shell.x + kPad, shell.y + 92.0f, shell.w - kPad * 2.0f, shell.h - 158.0f);
    fill_round(g, preview, 14.0f, Color(255, 20, 23, 30));
    stroke_round(g, preview, 14.0f, border());
    draw_string(g, L"Preview", rect(preview.x + 16.0f, preview.y + 12.0f, preview.w - 32.0f, 22.0f),
                13.0f, muted(), FontStyleBold);

    std::wstring body = state_ ? state_->copyBuffer() : std::wstring{};
    if (body.empty()) body = L"Start typing...";
    draw_wrapped(g, body, rect(preview.x + 16.0f, preview.y + 42.0f, preview.w - 32.0f, preview.h - 58.0f),
                 18.0f, body == L"Start typing..." ? dim() : text());

    Rect stopR = rect(shell.x + kPad, shell.y + shell.h - 50.0f, 146.0f, 36.0f);
    bool hot = hot_ == Hit::StopSpeak;
    bool down = active_ == Hit::StopSpeak && mouseDown_;
    fill_round(g, stopR, 9.0f, down ? accent2() : hot ? Color(255, 118, 145, 250) : accent());
    stroke_round(g, stopR, 9.0f, Color(255, 129, 153, 252));
    draw_string(g, L"Stop && Speak", stopR, 14.0f, Color(255, 255, 255, 255), FontStyleRegular,
                StringAlignmentCenter, StringAlignmentCenter);

    Rect configR = rect(stopR.x + stopR.w + 14.0f, stopR.y, 96.0f, 36.0f);
    bool configHot = hot_ == Hit::Config;
    bool configDown = active_ == Hit::Config && mouseDown_;
    fill_round(g, configR, 9.0f, configDown ? panel3() : configHot ? Color(255, 47, 54, 68) : panel2());
    stroke_round(g, configR, 9.0f, configHot ? border_hot() : border());
    draw_string(g, L"Config", configR, 14.0f, text(), FontStyleRegular,
                StringAlignmentCenter, StringAlignmentCenter);

    draw_string(g, L"Ctrl+Backspace toggles  /  Enter stops  /  Ctrl+Shift+Tab+E exits",
                rect(configR.x + configR.w + 18.0f, stopR.y, shell.w - stopR.w - configR.w - 88.0f, stopR.h), 13.0f, muted());
}

NativeUi::Hit NativeUi::hit_test(float x, float y) const
{
    if (!state_) return Hit::None;
    x /= dpiScale_;
    y /= dpiScale_;

    RECT cr{};
    GetClientRect(hwnd_, &cr);
    Rect bounds = rect(0, 0,
                       (float)(cr.right - cr.left) / dpiScale_,
                       (float)(cr.bottom - cr.top) / dpiScale_);
    Rect shell = inset(bounds, 18.0f, 16.0f);

    if (!showingConfig_) {
        Rect stopR = rect(shell.x + kPad, shell.y + shell.h - 50.0f, 146.0f, 36.0f);
        if (contains(stopR, x, y)) return Hit::StopSpeak;
        Rect configR = rect(stopR.x + stopR.w + 14.0f, stopR.y, 96.0f, 36.0f);
        if (contains(configR, x, y)) return Hit::Config;
        return Hit::None;
    }

    if (dropdown_.open) {
        Rect pop = dropdown_.rect;
        pop.y += pop.h + 6.0f;
        pop.h = std::min(7, dropdown_.itemCount) * 34.0f + 8.0f;
        if (contains(pop, x, y)) {
            const int row = (int)((y - pop.y - 4.0f) / 34.0f);
            const int idx = dropdown_visible_start() + row;
            if (row >= 0 && row < 7 && idx >= 0 && idx < dropdown_.itemCount)
                return (Hit)((int)Hit::DropdownItemBase + idx);
        }
    }

    Rect quitR = rect(shell.x + shell.w - kPad - 78.0f, shell.y + 22.0f, 78.0f, 32.0f);
    if (contains(quitR, x, y)) return Hit::Quit;

    Rect card = rect(shell.x + kPad, shell.y + 92.0f, shell.w - kPad * 2.0f, shell.h - 190.0f);
    const float labelW = bounds.w >= 720.0f ? 112.0f : 0.0f;
    const float fieldX = card.x + 20.0f + labelW;
    const float fieldW = card.w - 40.0f - labelW;
    float cy = card.y + 22.0f;

    if (contains(rect(fieldX, cy, fieldW, kItemH), x, y)) return Hit::DeviceA;
    cy += kItemH + kGap;
    if (contains(rect(fieldX, cy, fieldW, kItemH), x, y)) return Hit::DeviceB;
    cy += kItemH + 16.0f;
    if (contains(rect(fieldX, cy, 146.0f, 34.0f), x, y)) return Hit::Refresh;
    if (contains(rect(fieldX + 166.0f, cy - 2.0f, fieldW - 166.0f, 38.0f), x, y)) return Hit::Keyless;
    cy += 48.0f;
    if (contains(rect(fieldX, cy - 2.0f, 170.0f, 38.0f), x, y)) return Hit::TranslatorMode;
    if (contains(rect(fieldX + 188.0f, cy, 118.0f, 34.0f), x, y)) return Hit::TargetLanguage;
    cy += 50.0f;
    float appW = std::min(210.0f, fieldW - 360.0f);
    if (contains(rect(fieldX, cy - 2.0f, 170.0f, 38.0f), x, y)) return Hit::IncomingMode;
    if (contains(rect(fieldX + 188.0f, cy, appW, 34.0f), x, y)) return Hit::IncomingApp;
    if (contains(rect(fieldX + 188.0f + appW + 18.0f, cy - 2.0f, 126.0f, 38.0f), x, y)) return Hit::MuteIncoming;
    cy += 50.0f;

    if (contains(rect(fieldX, cy - 2.0f, 100.0f, 38.0f), x, y)) return Hit::MicBridge;
    if (contains(rect(fieldX + 112.0f, cy - 2.0f, 116.0f, 38.0f), x, y)) return Hit::MicMute;
    cy += 42.0f;
    if (contains(rect(fieldX, cy, fieldW * 0.50f - 6.0f, kItemH), x, y)) return Hit::MicDevice;
    if (contains(rect(fieldX + fieldW * 0.50f + 6.0f, cy, fieldW * 0.50f - 6.0f, kItemH), x, y)) return Hit::BridgeVirtualOut;
    cy += kItemH + 8.0f;
    if (contains(rect(fieldX, cy, fieldW * 0.50f - 6.0f, kItemH), x, y)) return Hit::BridgeVirtualIn;
    if (contains(rect(fieldX + fieldW * 0.50f + 6.0f, cy, 156.0f, 34.0f), x, y)) return Hit::InstallDriver;
    if (contains(rect(fieldX + fieldW * 0.50f + 174.0f, cy, 126.0f, 34.0f), x, y)) return Hit::SetDefaultMic;
    cy += kItemH + 8.0f;
    if (contains(rect(fieldX + 164.0f, cy, 58.0f, 30.0f), x, y)) return Hit::MicGainDown;
    if (contains(rect(fieldX + 230.0f, cy, 58.0f, 30.0f), x, y)) return Hit::MicGainUp;
    if (contains(rect(fieldX + 304.0f, cy, 58.0f, 30.0f), x, y)) return Hit::TtsGainDown;
    if (contains(rect(fieldX + 370.0f, cy, 58.0f, 30.0f), x, y)) return Hit::TtsGainUp;
    cy += 40.0f;

    if (contains(rect(fieldX, cy, fieldW, kItemH), x, y)) return Hit::Voice;
    cy += kItemH + kGap;

    bool custom = state_->sapiVoiceIndex >= 0 &&
                  state_->sapiVoiceIndex < (int)state_->sapiVoices.size() &&
                  state_->sapiVoices[state_->sapiVoiceIndex] == L"Custom";
    if (custom && contains(rect(fieldX, cy, fieldW, kItemH), x, y)) return Hit::CustomCommand;

    Rect footer = rect(shell.x + kPad, shell.y + shell.h - 58.0f, shell.w - kPad * 2.0f, 38.0f);
    if (contains(rect(footer.x, footer.y, 96.0f, 36.0f), x, y)) return Hit::Start;
    if (contains(rect(footer.x + 110.0f, footer.y, 116.0f, 36.0f), x, y)) return Hit::TestTone;
    if (contains(rect(footer.x + 240.0f, footer.y, 108.0f, 36.0f), x, y)) return Hit::TestTts;

    return Hit::None;
}

void NativeUi::activate_hit(Hit hit)
{
    if (!state_) return;

    if ((int)hit >= (int)Hit::DropdownItemBase) {
        commit_dropdown_item((int)hit - (int)Hit::DropdownItemBase);
        return;
    }

    if (hit != Hit::CustomCommand) customInputFocused_ = false;
    if (hit != Hit::TargetLanguage) targetInputFocused_ = false;
    if (hit != Hit::IncomingApp) incomingAppInputFocused_ = false;

    switch (hit)
    {
    case Hit::Quit:
        queue_action(UiAction::Quit);
        break;
    case Hit::Refresh:
        close_dropdown();
        queue_action(UiAction::RefreshDevices);
        break;
    case Hit::DeviceA:
        if (dropdown_.open && dropdown_.owner == Hit::DeviceA) {
            close_dropdown();
        } else if (!state_->outDevices.empty()) {
            open_dropdown(Hit::DeviceA, rect(0, 0, 0, 0), (int)state_->outDevices.size(), state_->devA);
        }
        break;
    case Hit::DeviceB:
        if (dropdown_.open && dropdown_.owner == Hit::DeviceB) {
            close_dropdown();
        } else if (!state_->outDevices.empty()) {
            open_dropdown(Hit::DeviceB, rect(0, 0, 0, 0), (int)state_->outDevices.size(), state_->devB);
        }
        break;
    case Hit::Keyless:
        close_dropdown();
        state_->useKeylessBackup.store(!state_->useKeylessBackup.load());
        break;
    case Hit::TranslatorMode:
        close_dropdown();
        state_->translatorMode.store(!state_->translatorMode.load());
        break;
    case Hit::TargetLanguage:
        close_dropdown();
        targetInputFocused_ = true;
        break;
    case Hit::IncomingMode:
        close_dropdown();
        state_->incomingTranslatorMode.store(!state_->incomingTranslatorMode.load());
        break;
    case Hit::IncomingApp:
        close_dropdown();
        incomingAppInputFocused_ = true;
        break;
    case Hit::MuteIncoming:
        close_dropdown();
        state_->muteIncomingApp.store(!state_->muteIncomingApp.load());
        break;
    case Hit::MicBridge:
        close_dropdown();
        state_->micBridgeEnabled.store(!state_->micBridgeEnabled.load());
        break;
    case Hit::MicDevice:
        if (dropdown_.open && dropdown_.owner == Hit::MicDevice) {
            close_dropdown();
        } else if (!state_->inDevices.empty()) {
            open_dropdown(Hit::MicDevice, rect(0, 0, 0, 0), (int)state_->inDevices.size(), state_->micDev);
        }
        break;
    case Hit::BridgeVirtualOut:
        if (dropdown_.open && dropdown_.owner == Hit::BridgeVirtualOut) {
            close_dropdown();
        } else if (!state_->outDevices.empty()) {
            open_dropdown(Hit::BridgeVirtualOut, rect(0, 0, 0, 0), (int)state_->outDevices.size(), state_->bridgeVirtualOutDev);
        }
        break;
    case Hit::BridgeVirtualIn:
        if (dropdown_.open && dropdown_.owner == Hit::BridgeVirtualIn) {
            close_dropdown();
        } else if (!state_->inDevices.empty()) {
            open_dropdown(Hit::BridgeVirtualIn, rect(0, 0, 0, 0), (int)state_->inDevices.size(), state_->bridgeVirtualInDev);
        }
        break;
    case Hit::MicMute:
        close_dropdown();
        state_->micMuted.store(!state_->micMuted.load());
        break;
    case Hit::MicGainDown:
        close_dropdown();
        state_->micGain = std::max(0.0f, state_->micGain - 0.1f);
        break;
    case Hit::MicGainUp:
        close_dropdown();
        state_->micGain = std::min(3.0f, state_->micGain + 0.1f);
        break;
    case Hit::TtsGainDown:
        close_dropdown();
        state_->ttsGain = std::max(0.0f, state_->ttsGain - 0.1f);
        break;
    case Hit::TtsGainUp:
        close_dropdown();
        state_->ttsGain = std::min(3.0f, state_->ttsGain + 0.1f);
        break;
    case Hit::InstallDriver:
        close_dropdown();
        if (MessageBoxW(hwnd_, L"Install or repair the bundled virtual audio driver? This will ask for administrator permission.",
                        L"Mic Bridge", MB_YESNO | MB_ICONQUESTION) == IDYES) {
            bool ok = driver_setup::install_or_repair(hwnd_);
            if (ok) {
                for (int attempt = 0; attempt < 10; ++attempt) {
                    RefreshOutputDevices(*state_);
                    DriverSetupStatus status = driver_setup::refresh_status(*state_);
                    if (status.virtualPlaybackFound && status.virtualCaptureFound)
                        break;
                    Sleep(200);
                }

                const bool endpointsReady = state_->driverSetup.virtualPlaybackFound &&
                                            state_->driverSetup.virtualCaptureFound;
                MessageBoxW(hwnd_,
                            endpointsReady
                                ? L"Driver setup finished and the virtual audio endpoints are ready."
                                : L"Driver setup finished, but Windows has not exposed the endpoints yet. Restart Windows or press Refresh devices.",
                            L"Mic Bridge",
                            endpointsReady ? MB_OK | MB_ICONINFORMATION : MB_OK | MB_ICONWARNING);
            } else {
                driver_setup::refresh_status(*state_);
            }
        }
        break;
    case Hit::SetDefaultMic:
        close_dropdown();
        if (MessageBoxW(hwnd_, L"Set the selected virtual microphone as the Windows communications microphone?",
                        L"Mic Bridge", MB_YESNO | MB_ICONQUESTION) == IDYES)
            driver_setup::set_default_communications_capture(*state_, hwnd_);
        break;
    case Hit::Voice:
        if (dropdown_.open && dropdown_.owner == Hit::Voice) {
            close_dropdown();
        } else if (!state_->sapiVoices.empty()) {
            open_dropdown(Hit::Voice, rect(0, 0, 0, 0), (int)state_->sapiVoices.size(), state_->sapiVoiceIndex);
        }
        break;
    case Hit::CustomCommand:
        close_dropdown();
        customInputFocused_ = true;
        break;
    case Hit::Start:
        close_dropdown();
        if (!state_->outDevices.empty()) {
            state_->configDone.store(true);
            queue_action(UiAction::StartFromConfig);
        }
        break;
    case Hit::TestTone:
        close_dropdown();
        queue_action(UiAction::TestTone);
        break;
    case Hit::TestTts:
        close_dropdown();
        queue_action(UiAction::TestTts);
        break;
    case Hit::StopSpeak:
        queue_action(UiAction::StopRecording);
        break;
    case Hit::Config:
        queue_action(UiAction::OpenConfig);
        break;
    default:
        close_dropdown();
        break;
    }

    if (dropdown_.open && dropdown_.owner == hit &&
        (hit == Hit::DeviceA || hit == Hit::DeviceB || hit == Hit::MicDevice ||
         hit == Hit::BridgeVirtualOut || hit == Hit::BridgeVirtualIn || hit == Hit::Voice)) {
        RECT cr{};
        GetClientRect(hwnd_, &cr);
        Rect bounds = rect(0, 0,
                           (float)(cr.right - cr.left) / dpiScale_,
                           (float)(cr.bottom - cr.top) / dpiScale_);
        Rect shell = inset(bounds, 18.0f, 16.0f);
        Rect card = rect(shell.x + kPad, shell.y + 92.0f, shell.w - kPad * 2.0f, shell.h - 190.0f);
        const float labelW = bounds.w >= 720.0f ? 112.0f : 0.0f;
        const float fieldX = card.x + 20.0f + labelW;
        const float fieldW = card.w - 40.0f - labelW;
        float cy = card.y + 22.0f;
        if (hit == Hit::DeviceB) cy += kItemH + kGap;
        if (hit == Hit::MicDevice || hit == Hit::BridgeVirtualOut || hit == Hit::BridgeVirtualIn || hit == Hit::Voice)
            cy += (kItemH + kGap) * 2.0f + 16.0f + 48.0f + 50.0f + 50.0f;
        if (hit == Hit::MicDevice || hit == Hit::BridgeVirtualOut) cy += 42.0f;
        if (hit == Hit::BridgeVirtualIn) cy += 42.0f + kItemH + 8.0f;
        if (hit == Hit::Voice) cy += 42.0f + (kItemH + 8.0f) * 2.0f + 40.0f;
        if (hit == Hit::MicDevice || hit == Hit::BridgeVirtualIn)
            dropdown_.rect = rect(fieldX, cy, fieldW * 0.50f - 6.0f, kItemH);
        else if (hit == Hit::BridgeVirtualOut)
            dropdown_.rect = rect(fieldX + fieldW * 0.50f + 6.0f, cy, fieldW * 0.50f - 6.0f, kItemH);
        else
            dropdown_.rect = rect(fieldX, cy, fieldW, kItemH);
    }
}

void NativeUi::open_dropdown(Hit owner, const Rect& r, int count, int selected)
{
    dropdown_.owner = owner;
    dropdown_.rect = r;
    dropdown_.itemCount = count;
    dropdown_.selected = selected;
    dropdown_.open = true;
    customInputFocused_ = false;
}

void NativeUi::close_dropdown()
{
    dropdown_ = DropdownState{};
}

void NativeUi::commit_dropdown_item(int index)
{
    if (!dropdown_.open || !state_) return;
    if (index < 0 || index >= dropdown_.itemCount) {
        close_dropdown();
        return;
    }

    if (dropdown_.owner == Hit::DeviceA) state_->devA = index;
    else if (dropdown_.owner == Hit::DeviceB) state_->devB = index;
    else if (dropdown_.owner == Hit::MicDevice) state_->micDev = index;
    else if (dropdown_.owner == Hit::BridgeVirtualOut) state_->bridgeVirtualOutDev = index;
    else if (dropdown_.owner == Hit::BridgeVirtualIn) state_->bridgeVirtualInDev = index;
    else if (dropdown_.owner == Hit::Voice) {
        state_->sapiVoiceIndex = index;
        tts_winrt::set_voice_index(index);
    }

    close_dropdown();
}

void NativeUi::handle_char(wchar_t ch)
{
    if (!state_) return;
    if (incomingAppInputFocused_) {
        if (ch == L'\r' || ch == L'\n') {
            incomingAppInputFocused_ = false;
            return;
        }
        if (ch == L'\b') {
            if (!incomingAppWide_.empty()) incomingAppWide_.pop_back();
        } else if (ch >= 32 && incomingAppWide_.size() < 259) {
            incomingAppWide_.push_back(ch);
        }

        std::string u8 = custom_tts::WideToUtf8(incomingAppWide_);
        std::strncpy(state_->incomingAppExe, u8.c_str(), sizeof(state_->incomingAppExe) - 1);
        state_->incomingAppExe[sizeof(state_->incomingAppExe) - 1] = '\0';
        return;
    }

    if (targetInputFocused_) {
        if (ch == L'\r' || ch == L'\n') {
            targetInputFocused_ = false;
            return;
        }
        if (ch == L'\b') {
            if (!targetLangWide_.empty()) targetLangWide_.pop_back();
        } else if (((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'-') &&
                   targetLangWide_.size() < 15) {
            targetLangWide_.push_back((wchar_t)towlower(ch));
        }

        std::string u8 = custom_tts::WideToUtf8(targetLangWide_);
        std::strncpy(state_->translatorTargetLang, u8.c_str(), sizeof(state_->translatorTargetLang) - 1);
        state_->translatorTargetLang[sizeof(state_->translatorTargetLang) - 1] = '\0';
        return;
    }

    if (!customInputFocused_) return;
    if (ch == L'\r' || ch == L'\n') {
        customInputFocused_ = false;
        return;
    }
    if (ch == L'\b') {
        if (!customCommandWide_.empty()) customCommandWide_.pop_back();
    } else if (ch >= 32 && customCommandWide_.size() < 1023) {
        customCommandWide_.push_back(ch);
    }

    std::string u8 = custom_tts::WideToUtf8(customCommandWide_);
    std::strncpy(state_->customTtsCommand, u8.c_str(), sizeof(state_->customTtsCommand) - 1);
    state_->customTtsCommand[sizeof(state_->customTtsCommand) - 1] = '\0';
}

void NativeUi::handle_keydown(WPARAM key)
{
    if (key == VK_ESCAPE) {
        close_dropdown();
        customInputFocused_ = false;
        targetInputFocused_ = false;
        incomingAppInputFocused_ = false;
    } else if (dropdown_.open && key == VK_RETURN) {
        commit_dropdown_item(dropdown_.selected);
    } else if (dropdown_.open && key == VK_UP) {
        dropdown_.selected = std::max(0, dropdown_.selected - 1);
    } else if (dropdown_.open && key == VK_DOWN) {
        dropdown_.selected = std::min(dropdown_.itemCount - 1, dropdown_.selected + 1);
    }
}

std::vector<std::wstring> NativeUi::device_names() const
{
    std::vector<std::wstring> out;
    if (!state_) return out;
    out.reserve(state_->outDevices.size());
    for (const AudioDevice& dev : state_->outDevices)
        out.push_back(dev.name);
    return out;
}

std::vector<std::wstring> NativeUi::input_device_names() const
{
    std::vector<std::wstring> out;
    if (!state_) return out;
    out.reserve(state_->inDevices.size());
    for (const AudioDevice& dev : state_->inDevices)
        out.push_back(dev.name);
    return out;
}

std::vector<std::wstring> NativeUi::voice_names() const
{
    if (!state_) return {};
    return state_->sapiVoices;
}
