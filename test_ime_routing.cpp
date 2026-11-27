#define NOMINMAX
#include <windows.h>
#include <imm.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>

// The installed keyboard need not supply a Chinese IMM result string.
static std::wstring imeResult;
static LONG WINAPI readTestComposition(HIMC, DWORD kind, void* out, DWORD size) {
    if (kind != GCS_RESULTSTR) return 0;
    const DWORD bytes = static_cast<DWORD>(imeResult.size() * sizeof(wchar_t));
    if (out && bytes <= size) std::memcpy(out, imeResult.data(), bytes);
    return bytes;
}
static HKL WINAPI testKeyboardLayout(DWORD) { return reinterpret_cast<HKL>(0xe0010804ULL); }
static BOOL WINAPI testIsIme(HKL) { return TRUE; }
#undef NOMINMAX
#define GetKeyboardLayout testKeyboardLayout
#define ImmIsIME testIsIme
#define ImmGetCompositionStringW readTestComposition
#include "NativeIme.cpp"
#undef ImmGetCompositionStringW

int main() {
    _set_error_mode(_OUT_TO_STDERR);
    constexpr LRESULT stock = 71;
    WNDCLASSW cls{};
    cls.lpszClassName = L"WotImeRouteTest";
    cls.lpfnWndProc = [](HWND window, UINT message, WPARAM w, LPARAM l) -> LRESULT {
        if (message == WM_IME_CHAR || message == WM_KEYDOWN || message == WM_KEYUP || message == WM_IME_COMPOSITION ||
            message == WM_IME_NOTIFY || message == WM_IME_SETCONTEXT) return stock;
        return DefWindowProcW(window, message, w, l);
    };
    assert(RegisterClassW(&cls));
    HWND window = CreateWindowW(cls.lpszClassName, L"", WS_POPUP | WS_VISIBLE,
                                0, 0, 1, 1, nullptr, nullptr, nullptr, nullptr);
    SetForegroundWindow(window);
    SetFocus(window);
    assert(ImeStart([] {}, [](unsigned, const wchar_t*, int, int, int, const wchar_t*, int, int, int) { return 0; }));
    ImeTick();
    assert(ImeStatus() == 1);
    // A game window can have an IME association while keyboard focus is on
    // a different TSF document for gameplay. Restore both independently.
    ITfDocumentMgr *windowDoc = nullptr, *gameplay = nullptr, *prior = nullptr;
    ITfDocumentMgr *initial = nullptr, *afterClose = nullptr;
    assert(s.manager && SUCCEEDED(s.manager->GetFocus(&initial)));
    s.token = 1;
    assert(attachTsf());
    detach();
    assert(SUCCEEDED(s.manager->GetFocus(&afterClose)) && afterClose == initial);
    release(afterClose);
    release(initial);
    assert(s.manager && SUCCEEDED(s.manager->CreateDocumentMgr(&windowDoc)));
    assert(SUCCEEDED(s.manager->CreateDocumentMgr(&gameplay)));
    assert(SUCCEEDED(s.manager->AssociateFocus(window, windowDoc, &prior)));
    assert(SUCCEEDED(s.manager->SetFocus(gameplay)));
    s.token = 1;
    assert(attachTsf());
    // First W after chat closes runs through the production refresh/detach path.
    MSG move{window, WM_KEYDOWN, 'W', 1, 0, {0, 0}};
    handleKey(move);
    ITfDocumentMgr* restored = nullptr;
    assert(SUCCEEDED(s.manager->GetFocus(&restored)) && restored == gameplay);
    release(restored);
    assert(move.message == WM_KEYDOWN && move.wParam == 'W' && !s.token);
    for (const auto key : std::wstring(L"WASD")) {
        for (const UINT message : {WM_KEYDOWN, WM_KEYUP}) {
            MSG event{window, message, static_cast<WPARAM>(key), 1, 0, {0, 0}};
            getMessage(HC_ACTION, PM_REMOVE, reinterpret_cast<LPARAM>(&event));
            assert(event.message == message && event.wParam == static_cast<WPARAM>(key));
            assert(DispatchMessageW(&event) == stock);
        }
    }
    assert(SUCCEEDED(s.manager->AssociateFocus(window, prior, &restored)) && restored == windowDoc);
    release(restored);
    // A host can change focus itself while closing the chat.
    s.token = 1;
    assert(attachTsf() && SUCCEEDED(s.manager->SetFocus(windowDoc)));
    handleKey(move);
    assert(SUCCEEDED(s.manager->GetFocus(&restored)) && restored == windowDoc);
    release(restored);
    release(prior);
    release(gameplay);
    release(windowDoc);
    ImeStop();
    assert(ImeStatus() == 0);

    std::wstring longText(1024, L'x');
    int rects[] = {20, 20, 20, 40, 10, 10, 100, 100};
    updateSnapshot(1, longText.data(), longText.size(), 1, 2, true, rects);
    const auto* buffer = s.sample.draft.text.data();
    rects[3] = 50;
    updateSnapshot(1, longText.data(), longText.size(), 1, 2, true, rects);
    assert(s.sample.token == 1 && s.sample.draft.text.data() == buffer && s.sample.rects[3] == 50);
    updateSnapshot(1, L"\U0001f600", 2, 1, 1, false, rects);
    assert(!s.sample.token);

    s.window = window;
    assert(foreground() && SetWindowSubclass(window, wndProc, SubclassId, 0));
    s.refresh = [] {
        // Flash can synchronously dispatch a message before supplying its new snapshot.
        assert(SendMessageW(s.window, WM_IME_CHAR, 'x', 0) == stock);
    };
    for (UINT message : {WM_IME_CHAR, WM_IME_COMPOSITION, WM_IME_NOTIFY, WM_KEYDOWN, WM_IME_SETCONTEXT}) {
        s.token = s.sample.token = 1;
        s.immListMask = 1;
        WPARAM w = message == WM_IME_NOTIFY ? IMN_CLOSECANDIDATE : VK_PROCESSKEY;
        if (message != WM_KEYDOWN && message != WM_IME_SETCONTEXT) {
            MSG msg{window, message, w, 1, 0, {0, 0}};
            getMessage(HC_ACTION, PM_REMOVE, reinterpret_cast<LPARAM>(&msg));
            assert(msg.message == message && msg.wParam == w && msg.lParam == 1);
        }
        s.token = s.sample.token = 1;
        s.immListMask = 1;
        assert(SendMessageW(window, message, w, 1) == stock);
    }
    s.refresh = [] { s.sample.token = 1; };
    s.token = 1;
    MSG owned{window, WM_IME_CHAR, 'x', 0, 0, {0, 0}};
    getMessage(HC_ACTION, PM_REMOVE, reinterpret_cast<LPARAM>(&owned));
    assert(owned.message == WM_NULL && SendMessageW(window, WM_IME_CHAR, 'x', 0) == 0);
    static bool refreshing = false;
    static int commits = 0;
    s.imm = true;
    s.base = s.sample.draft = Draft{L"abc", 1, 2, true};
    s.keyboard = GetKeyboardLayout(0);
    assert(ImmContext(window).handle);
    s.commit = [](unsigned token, const wchar_t* before, int first, int last, int leading,
                  const wchar_t* after, int caret, int end, int reversed) {
        assert(!refreshing && token == 1 && std::wstring(before) == L"abc");
        assert(first == 1 && last == 2 && leading && std::wstring(after) == L"a\u4f60c");
        assert(caret == 2 && end == 2 && !reversed);
        ++commits;
        imeResult = L"\u4f60";
        assert(SendMessageW(s.window, WM_IME_COMPOSITION, 0, GCS_RESULTSTR) == 0);
        imeResult.clear();
        return 1;
    };
    s.refresh = [] {
        assert(!refreshing);
        refreshing = true;
        s.sample.token = 1;
        SendMessageW(s.window, WM_IME_STARTCOMPOSITION, 0, 0);
        assert(s.immComposing);
        s.immListMask = 1;
        imeResult = L"\u4f60";
        SendMessageW(s.window, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
        imeResult.clear(); // The HIMC bytes are only available during the message.
        SendMessageW(s.window, WM_IME_ENDCOMPOSITION, 0, 0);
        assert(!commits);
        refreshing = false;
    };
    assert(SendMessageW(window, WM_IME_CHAR, 'x', 0) == 0);
    assert(commits == 1 && !s.immComposing && !s.immListMask);
    static const Draft original{L"abc", 1, 2, true};
    s.commit = [](unsigned, const wchar_t*, int, int, int, const wchar_t*, int, int, int) {
        assert(false && "stale result committed");
        return 0;
    };
    s.refresh = [] { s.sample.token = 1; };
    s.base = s.sample.draft = original; // Old selection reappears after the commit callback returned.
    SendMessageW(window, WM_IME_CHAR, 'x', 0);
    enum class Invalidated { Text, Selection, Direction, Token, Reattach, Capacity };
    static Invalidated scenario;
    for (auto value : {Invalidated::Text, Invalidated::Selection, Invalidated::Direction,
                       Invalidated::Token, Invalidated::Reattach, Invalidated::Capacity}) {
        scenario = value;
        s.imm = true;
        s.token = 1;
        s.base = s.sample.draft = original;
        s.refresh = [] {
            s.sample.token = 1;
            imeResult.assign(scenario == Invalidated::Capacity ? TextStore::Capacity : 1, L'x');
            SendMessageW(s.window, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
            imeResult.clear();
            switch (scenario) {
            case Invalidated::Text: s.sample.draft.text = L"def"; break;
            case Invalidated::Selection: s.sample.draft.last = 3; break;
            case Invalidated::Direction: s.sample.draft.leading = false; break;
            case Invalidated::Token: s.sample.token = 2; break;
            case Invalidated::Reattach: detach(); s.imm = true; s.token = 1; break;
            case Invalidated::Capacity: break;
            }
        };
        SendMessageW(window, WM_IME_CHAR, 'x', 0);
    }
    s.imm = true;
    s.base = s.sample.draft = original;
    s.refresh = [] {
        s.sample.token = 1;
        imeResult = L"B";
        SendMessageW(s.window, WM_IME_COMPOSITION, 0, GCS_RESULTSTR);
        imeResult.clear();
    };
    s.commit = [](unsigned, const wchar_t*, int, int, int, const wchar_t* after, int, int, int) {
        assert(std::wstring(after) == L"aABc");
        ++commits;
        return 1;
    };
    imeResult = L"A";
    MSG result{window, WM_IME_COMPOSITION, 0, GCS_RESULTSTR, 0, {0, 0}};
    getMessage(HC_ACTION, PM_REMOVE, reinterpret_cast<LPARAM>(&result));
    assert(result.message == WM_NULL && commits == 2);
    DispatchMessageW(&result);
    assert(commits == 2);
    s.refresh = [] {
        s.sample.token = 1;
        s.sample.draft = s.base;
        SendMessageW(s.window, WM_IME_ENDCOMPOSITION, 0, 0);
    };
    SendMessageW(window, WM_IME_STARTCOMPOSITION, 0, 0);
    assert(!s.immComposing);
    DestroyWindow(window);
    std::puts("Queued, sent and nested IMM routing passed");
}
