#define NOMINMAX
#ifndef TEST_CANDIDATE_UI
#define PY_SSIZE_T_CLEAN
#include <Python.h>
#endif
#include "TextStore.h"
#include "CandidateList.h"
#include <array>
#include <commctrl.h>
#include <cstdio>
#include <imm.h>
#include <memory>
#include <new>
#include <vector>

using Refresh = void (WINAPI*)();
using Commit = int (WINAPI*)(unsigned, const wchar_t*, int, int, int, const wchar_t*, int, int, int);

namespace {
constexpr UINT_PTR SubclassId = 1;
constexpr unsigned ImeKeyCount = 256;

struct Snapshot {
    unsigned token = 0;
    Draft draft;
    std::array<int, 8> rects{};
};

struct ReleaseCom {
    template<class T> void operator()(T* value) const { value->Release(); }
};

template<class T> using Com = std::unique_ptr<T, ReleaseCom>;

struct CandidatePage {
    Com<ITfCandidateListUIElement> list;
    unsigned serial = 0, composition = 0, first = 0;
    int selection = -1;
    bool clickable = false;
    std::vector<std::wstring> labels;
};

struct State {
    DWORD thread = 0;
    HWND window = nullptr;
    HHOOK hook = nullptr;
    Refresh refresh = nullptr;
    Commit commit = nullptr;
    ITfThreadMgr* manager = nullptr;
    ITfKeystrokeMgr* keys = nullptr;
    ITfInputProcessorProfileMgr* profiles = nullptr;
    ITfDocumentMgr* document = nullptr;
    ITfDocumentMgr* previous = nullptr;
    ITfContext* context = nullptr;
    CandidatePage candidates;
    TextStore* store = nullptr;
    TfClientId client = 0;
    bool coInitialized = false, activated = false, associated = false;
    bool busy = false, imm = false, immComposing = false;
    bool attachFailed = false;
    unsigned token = 0, attachment = 0;
    HKL keyboard = nullptr;
    Draft base;
    Snapshot sample;
    std::array<bool, ImeKeyCount> swallowed{};
};
thread_local State s;
thread_local unsigned candidateSerial = 0;
thread_local struct Diagnostics {
    TextStore::Activity store;
    unsigned blocked = 0, keyDown = 0, processKey = 0, wanted = 0, eaten = 0, noSink = 0, failed = 0;
    unsigned draftDetach = 0, uiSamples = 0, candidates = 0, shown = 0;
    unsigned overlayRows = 0, overlayClickable = 0, choices = 0;
    HRESULT choiceResult = S_FALSE;
    unsigned immOpen = 0, immChange = 0, immClose = 0, imeSetContext = 0;
    DWORD imeFlags = 0;
    RECT caret{};
} diagnostics;

struct Busy {
    Busy() { s.busy = true; }
    ~Busy() { s.busy = false; }
};

template<class T> void release(T*& value) {
    if (value) { auto* old = value; value = nullptr; old->Release(); }
}

bool activeTsfFlags(DWORD& flags) {
    ITfThreadMgrEx* managerEx = nullptr;
    const bool known = s.manager &&
        SUCCEEDED(s.manager->QueryInterface(__uuidof(ITfThreadMgrEx), reinterpret_cast<void**>(&managerEx))) &&
        SUCCEEDED(managerEx->GetActiveFlags(&flags));
    release(managerEx);
    return known;
}

ITfUIElementMgr* uiManager() {
    ITfUIElementMgr* ui = nullptr;
    s.manager->QueryInterface(__uuidof(ITfUIElementMgr), reinterpret_cast<void**>(&ui));
    return ui;
}

Com<ITfCandidateListUIElement> ownedCandidate(ITfUIElement* element) {
    ITfCandidateListUIElement* raw = nullptr;
    if (element->QueryInterface(candidateListId, reinterpret_cast<void**>(&raw)) != S_OK || !raw)
        return {};
    Com<ITfCandidateListUIElement> candidate(raw);
    ITfDocumentMgr* owner = nullptr;
    const bool ours = candidate->GetDocumentMgr(&owner) == S_OK && owner == s.document;
    release(owner);
    return ours ? std::move(candidate) : nullptr;
}

Com<ITfCandidateListUIElementBehavior> candidateBehavior(ITfCandidateListUIElement* candidate) {
    ITfCandidateListUIElementBehavior* raw = nullptr;
    candidate->QueryInterface(candidateBehaviorId, reinterpret_cast<void**>(&raw));
    return Com<ITfCandidateListUIElementBehavior>(raw);
}

constexpr unsigned MaxCandidateRows = 16;
bool readCandidatePage(CandidatePage& page) {
    auto& list = *page.list;
    UINT count = 0, pageCount = 0, current = 0;
    if (list.GetCount(&count) != S_OK || !count) return false;
    UINT last = count;
    if (list.GetPageIndex(nullptr, 0, &pageCount) == S_OK && pageCount > 1) {
        // ponytail: ignore malformed or oversized page metadata rather than guessing indices.
        if (pageCount > 1024 || list.GetCurrentPage(&current) != S_OK || current >= pageCount) return false;
        std::vector<UINT> offsets(pageCount);
        if (list.GetPageIndex(offsets.data(), pageCount, &pageCount) != S_OK ||
            pageCount != offsets.size() || offsets.front() != 0) return false;
        page.first = offsets[current];
        if (current + 1 < pageCount) last = offsets[current + 1];
        if (page.first >= last || last > count) return false;
    } else if (count > MaxCandidateRows) {
        return false;
    }
    last = page.first + std::min(last - page.first, MaxCandidateRows);
    UINT selected = 0;
    if (list.GetSelection(&selected) == S_OK && selected >= page.first && selected < last)
        page.selection = static_cast<int>(selected - page.first);
    for (UINT index = page.first; index < last; ++index) {
        BSTR text = nullptr;
        const HRESULT result = list.GetString(index, &text);
        const std::unique_ptr<wchar_t, decltype(&SysFreeString)> owned(text, SysFreeString);
        const UINT length = text ? SysStringLen(text) : 0;
        if (result != S_OK || !length || length > 256 || std::wmemchr(text, L'\0', length)) return false;
        page.labels.emplace_back(text, length);
    }
    page.clickable = bool(candidateBehavior(page.list.get()));
    return true;
}

void sampleCandidateUI() {
    CandidatePage next;
    if (s.store && s.store->composing) {
        auto* ui = uiManager();
        IEnumTfUIElements* raw = nullptr;
        if (ui) ui->EnumUIElements(&raw);
        release(ui);
        Com<IEnumTfUIElements> elements(raw);
        if (elements) {
            ++diagnostics.uiSamples;
            ITfUIElement* element = nullptr;
            ULONG fetched = 0;
            while (elements->Next(1, &element, &fetched) == S_OK && fetched == 1 && element) {
                Com<ITfUIElement> owned(element);
                auto list = ownedCandidate(element);
                element = nullptr;
                if (!list) continue;
                BOOL visible = FALSE;
                const HRESULT result = list->IsShown(&visible);
                diagnostics.candidates = 1;
                if (result != S_OK) continue;
                if (visible) {
                    diagnostics.shown = 1;
                    next = CandidatePage{};
                    break;
                }
                if (!next.list) {
                    CandidatePage page;
                    page.list = std::move(list);
                    if (readCandidatePage(page)) next = std::move(page);
                }
            }
            release(element);
        }
    }
    if (next.list) {
        const auto& old = s.candidates;
        next.composition = diagnostics.store.composition;
        if (next.list == old.list && next.composition == old.composition && next.first == old.first &&
            next.labels == old.labels && next.clickable == old.clickable) {
            next.serial = old.serial;
        } else {
            if (++candidateSerial == 0) ++candidateSerial;
            next.serial = candidateSerial;
        }
        diagnostics.overlayRows = static_cast<unsigned>(next.labels.size());
        diagnostics.overlayClickable = next.clickable;
    }
    s.candidates = std::move(next);
}

bool chooseCandidate(unsigned serial, unsigned row) {
    const auto& page = s.candidates;
    if (!serial || serial != page.serial || !page.clickable || row >= page.labels.size() ||
        !s.store || !s.store->composing) return false;
    auto behavior = candidateBehavior(page.list.get());
    if (!behavior) return false;
    const UINT index = page.first + row;
    ++diagnostics.choices;
    diagnostics.choiceResult = behavior->SetSelection(index);
    if (diagnostics.choiceResult != S_OK) return false;
    if (s.store->composing) diagnostics.choiceResult = behavior->Finalize();
    if (diagnostics.choiceResult != S_OK) return false;
    s.candidates = CandidatePage{};
    return true;
}

#ifdef TEST_CANDIDATE_UI
}

bool testCandidateUI(ITfThreadMgr* manager, ITfDocumentMgr* document, TextStore* store,
                     ITfUIElement* ours, ITfUIElement* foreign) {
    s.manager = manager;
    s.document = document;
    s.store = store;
    s.token = 1;
    store->composing = true;
    DWORD flags = 0;
    auto* ui = uiManager();
    bool success = ui && activeTsfFlags(flags) && (flags & TF_TMF_UIELEMENTENABLEDONLY);
    if (success) {
        BOOL show = FALSE;
        DWORD foreignId = 0, ownId = 0;
        const bool foreignStarted = ui->BeginUIElement(foreign, &show, &foreignId) == S_OK;
        sampleCandidateUI();
        success = foreignStarted && !s.candidates.serial;
        const bool ownStarted = ui->BeginUIElement(ours, &show, &ownId) == S_OK;
        sampleCandidateUI();
        const auto serial = s.candidates.serial;
        success = success && ownStarted && serial && s.candidates.first == 2 &&
                  s.candidates.selection == 1 && s.candidates.clickable &&
                  s.candidates.labels == std::vector<std::wstring>{L"\u6d4b", L"\u8bd5"};
        sampleCandidateUI();
        success = success && s.candidates.serial == serial &&
                  !chooseCandidate(serial + 1, 0) && !chooseCandidate(serial, 2) &&
                  chooseCandidate(serial, 0) && !s.candidates.serial;
        if (ownStarted && ui->EndUIElement(ownId) != S_OK) success = false;
        if (foreignStarted && ui->EndUIElement(foreignId) != S_OK) success = false;
        sampleCandidateUI();
        success = success && !s.candidates.serial;
    }
    release(ui);
    store->composing = false;
    s = State{};
    return success;
}
#else
HWND gameWindow() {
    const HWND window = GetFocus();
    if (!window || window != GetForegroundWindow() || GetWindow(window, GW_OWNER) ||
        GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId() ||
        GetClassLongPtrW(window, GCW_ATOM) == 32770) return nullptr; // Standard dialog class.
    return window;
}

bool foreground() { return s.window && gameWindow() == s.window; }

void detach() {
    if (!s.token && !s.store) return;
    s.token = 0;
    s.candidates = CandidatePage{};
    ++s.attachment;
    if (s.immComposing) {
        s.immComposing = false;
        if (auto imc = ImmGetContext(s.window)) {
            ImmNotifyIME(imc, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            ImmReleaseContext(s.window, imc);
        }
    }
    s.imm = false;
    if (s.store) {
        s.store->acceptComposition = false;
        if (s.store->composing && s.context) {
            ITfContextOwnerCompositionServices* service = nullptr;
            if (SUCCEEDED(s.context->QueryInterface(__uuidof(ITfContextOwnerCompositionServices),
                                                    reinterpret_cast<void**>(&service)))) {
                service->TerminateComposition(nullptr);
                service->Release();
            }
        }
    }
    if (s.associated) {
        ITfDocumentMgr* displaced = nullptr;
        s.manager->AssociateFocus(s.window, s.previous, &displaced);
        release(displaced);
        ITfDocumentMgr* current = nullptr;
        if (SUCCEEDED(s.manager->GetFocus(&current)) && current == s.document)
            s.manager->SetFocus(s.previous);
        release(current);
        s.associated = false;
    }
    release(s.previous);
    if (s.document) s.document->Pop(TF_POPF_ALL);
    release(s.context);
    release(s.document);
    release(s.store);
}

void locate();

bool attachTsf() {
    if (!s.manager) return false;
    s.store = new (std::nothrow) TextStore(s.window, s.base, &diagnostics.store);
    if (!s.store) return false;
    locate();
    TfEditCookie cookie;
    if (FAILED(s.manager->CreateDocumentMgr(&s.document)) ||
        FAILED(s.document->CreateContext(s.client, 0, static_cast<ITextStoreACP*>(s.store), &s.context, &cookie)) ||
        FAILED(s.document->Push(s.context))) return false;
    s.associated = SUCCEEDED(s.manager->AssociateFocus(s.window, s.document, &s.previous));
    return s.associated && SUCCEEDED(s.manager->SetFocus(s.document));
}

bool legacy(HKL keyboard) {
    TF_INPUTPROCESSORPROFILE active{};
    if (s.profiles && s.profiles->GetActiveProfile(GUID_TFCAT_TIP_KEYBOARD, &active) == S_OK &&
        active.dwProfileType == TF_PROFILETYPE_INPUTPROCESSOR) return false;
    return ImmIsIME(keyboard) != FALSE;
}

void locate() {
    RECT client{};
    if (!GetClientRect(s.window, &client) || client.right <= 0 || client.bottom <= 0) return;
    const auto& r = s.sample.rects;
    POINT a{MulDiv(client.right, r[0], 10000), MulDiv(client.bottom, r[1], 10000)};
    POINT b{MulDiv(client.right, r[2], 10000), MulDiv(client.bottom, r[3], 10000)};
    b.x = std::max(b.x, a.x + 1);
    b.y = std::max(b.y, a.y + 1);
    if (s.imm) {
        if (auto imc = ImmGetContext(s.window)) {
            CANDIDATEFORM form{};
            form.dwStyle = CFS_EXCLUDE;
            form.ptCurrentPos = {a.x, b.y};
            form.rcArea = {a.x, a.y, b.x, b.y};
            ImmSetCandidateWindow(imc, &form);
            COMPOSITIONFORM composition{CFS_POINT, a, {}};
            ImmSetCompositionWindow(imc, &composition);
            ImmReleaseContext(s.window, imc);
        }
    } else if (s.store) {
        POINT first{MulDiv(client.right, r[4], 10000), MulDiv(client.bottom, r[5], 10000)};
        POINT last{MulDiv(client.right, r[6], 10000), MulDiv(client.bottom, r[7], 10000)};
        if (ClientToScreen(s.window, &a) && ClientToScreen(s.window, &b) &&
            ClientToScreen(s.window, &first) && ClientToScreen(s.window, &last)) {
            diagnostics.caret = {a.x, a.y, b.x, b.y};
            s.store->layout(diagnostics.caret, {first.x, first.y, last.x, last.y});
        }
    }
}

bool valid(const Draft& draft) {
    const auto& text = draft.text;
    if (text.size() > TextStore::Capacity || draft.first < 0 || draft.last < draft.first ||
        static_cast<size_t>(draft.last) > text.size()) return false;
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned unit = text[i];
        if (!unit || (unit >= 0xdc00 && unit <= 0xdfff)) return false;
        if (unit >= 0xd800 && unit <= 0xdbff) {
            if (++i == text.size() || text[i] < 0xdc00 || text[i] > 0xdfff ||
                i == static_cast<size_t>(draft.first) || i == static_cast<size_t>(draft.last)) return false;
        }
    }
    return true;
}

bool commit(const Draft& after) {
    if (!s.token || !foreground() || !valid(after)) return false;
    const auto& before = s.base;
    if (after == before) return true;
    if (!s.commit(s.token, before.text.c_str(), before.first, before.last, before.leading,
                  after.text.c_str(), after.first, after.last, after.leading)) return false;
    s.base = after;
    return true;
}

void flushTsf() {
    if (!s.store || s.store->composing) return;
    const auto& after = s.store->draft;
    const auto& old = s.base;
    // An empty result that only removes the pre-composition selection is a cancellation.
    const auto removed = static_cast<size_t>(old.last - old.first);
    const bool restoredEmpty = s.store->finishedComposition && removed &&
        after.text.size() + removed == old.text.size() &&
        after.text.compare(0, old.first, old.text, 0, old.first) == 0 &&
        after.text.compare(old.first, std::wstring::npos, old.text, old.last, std::wstring::npos) == 0;
    s.store->finishedComposition = false;
    if (s.store->cancelled || restoredEmpty) {
        s.store->cancelled = false;
        s.store->updateDraft(old);
    } else if (!commit(after)) {
        detach();
    }
}

void sync() {
    if (!foreground()) { detach(); return; }
    s.sample.token = 0;
    s.refresh();
    if (!foreground()) { detach(); return; }
    const auto keyboard = GetKeyboardLayout(0);
    const bool imm = legacy(keyboard);
    const bool draftChangedDuringComposition = s.token && (s.immComposing || (s.store && s.store->composing)) &&
                                               !(s.base == s.sample.draft) && s.token == s.sample.token;
    if (draftChangedDuringComposition) ++diagnostics.draftDetach;
    if (s.token && (s.token != s.sample.token || s.keyboard != keyboard || s.imm != imm ||
                    draftChangedDuringComposition)) detach();
    if (!s.sample.token) return;
    const bool newField = !s.token;
    if (newField) {
        s.base = s.sample.draft;
        s.keyboard = keyboard;
        s.imm = imm;
        s.token = s.sample.token;
        if (!imm && !attachTsf()) {
            s.attachFailed = true;
            detach();
            return;
        }
        s.attachFailed = false;
        if (imm) DefWindowProcW(s.window, WM_IME_SETCONTEXT, TRUE, ISC_SHOWUIALL);
    } else if (!(s.base == s.sample.draft)) {
        s.base = s.sample.draft;
        if (s.store) s.store->updateDraft(s.base);
    }
    locate();
}

void handleKey(MSG& msg) {
    const bool down = msg.message == WM_KEYDOWN || msg.message == WM_SYSKEYDOWN;
    const bool up = msg.message == WM_KEYUP || msg.message == WM_SYSKEYUP;
    if (!up && !down) return;
    if (msg.hwnd != s.window || !foreground()) {
        if (down && s.store) ++diagnostics.blocked;
        return;
    }
    const auto originalKey = msg.wParam;
    const auto key = originalKey == VK_PROCESSKEY ? ImmGetVirtualKey(s.window) : originalKey;
    const bool tracked = key < ImeKeyCount;
    if (tracked && down && !(msg.lParam & (1L << 30))) s.swallowed[key] = false;
    const bool follow = tracked && s.swallowed[key];
    if (tracked && up) s.swallowed[key] = false;

    // Flush out-of-band edits (such as selecting a candidate with the mouse) before the next key.
    flushTsf();
    sync();
    bool eaten = false;
    if (s.store && s.keys) {
        if (down) {
            ++diagnostics.keyDown;
            if (originalKey == VK_PROCESSKEY) ++diagnostics.processKey;
        }
        const bool wasComposing = s.store->composing;
        BOOL wants = FALSE, handled = FALSE;
        const HRESULT test = down ? s.keys->TestKeyDown(msg.wParam, msg.lParam, &wants)
                                  : s.keys->TestKeyUp(msg.wParam, msg.lParam, &wants);
        if (down) {
            if (test == S_FALSE) ++diagnostics.noSink;
            if (FAILED(test)) ++diagnostics.failed;
        }
        if (test == S_OK && wants) {
            const HRESULT result = down ? s.keys->KeyDown(msg.wParam, msg.lParam, &handled)
                                        : s.keys->KeyUp(msg.wParam, msg.lParam, &handled);
            eaten = result == S_OK && handled;
            if (down) {
                ++diagnostics.wanted;
                if (eaten) ++diagnostics.eaten;
                if (FAILED(result)) ++diagnostics.failed;
            }
        }
        if (down && key == VK_RETURN && wasComposing) eaten = true;
        if (down && key == VK_ESCAPE && wasComposing) {
            s.store->cancelled = true;
            if (s.store->composing) detach();
            eaten = true;
        }
        flushTsf();
    } else if (s.imm && originalKey == VK_PROCESSKEY) {
        // TranslateMessage still needs PROCESSKEY to produce IMM messages.
        if (down && tracked) s.swallowed[key] = true;
        if (down && key == VK_ESCAPE && s.immComposing) {
            detach();
            eaten = true;
        } else return;
    } else if (s.imm && down && key == VK_ESCAPE && s.immComposing) {
        detach();
        eaten = true;
    }
    if (down && eaten && tracked) s.swallowed[key] = true;
    if (eaten || follow) {
        msg.message = WM_NULL;
        msg.wParam = 0;
        msg.lParam = 0;
    }
}

LRESULT CALLBACK getMessage(int code, WPARAM kind, LPARAM param) {
    if (code == HC_ACTION && kind == PM_REMOVE && !s.busy) {
        Busy busy;
        try { handleKey(*reinterpret_cast<MSG*>(param)); }
        catch (...) { detach(); }
    }
    return CallNextHookEx(nullptr, code, kind, param);
}

void immResult(LPARAM flags) {
    if (!(flags & GCS_RESULTSTR)) return;
    std::wstring text;
    if (auto imc = ImmGetContext(s.window)) {
        const LONG bytes = ImmGetCompositionStringW(imc, GCS_RESULTSTR, nullptr, 0);
        if (bytes > 0 && bytes <= TextStore::Capacity * 2 && bytes % 2 == 0) {
            text.resize(bytes / 2);
            if (ImmGetCompositionStringW(imc, GCS_RESULTSTR, text.data(), bytes) != bytes) text.clear();
        }
        ImmReleaseContext(s.window, imc);
    }
    if (text.empty()) return;
    Draft next = s.base;
    next.text.replace(next.first, next.last - next.first, text);
    next.first += static_cast<LONG>(text.size());
    next.last = next.first;
    next.leading = false;
    if (next.text.size() <= TextStore::Capacity && !commit(next)) detach();
}

LRESULT CALLBACK wndProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (w && message == WM_IME_SETCONTEXT) {
        ++diagnostics.imeSetContext;
        diagnostics.imeFlags = static_cast<DWORD>(l);
    } else if (message == WM_IME_NOTIFY) {
        if (w == IMN_OPENCANDIDATE) ++diagnostics.immOpen;
        if (w == IMN_CHANGECANDIDATE) ++diagnostics.immChange;
        if (w == IMN_CLOSECANDIDATE) ++diagnostics.immClose;
    }
    if (message == WM_NCDESTROY) {
        if (!s.busy) { Busy busy; detach(); }
        RemoveWindowSubclass(window, wndProc, id);
        s.window = nullptr;
    } else if (!s.busy && (message == WM_KILLFOCUS || message == WM_INPUTLANGCHANGE)) {
        Busy busy;
        detach();
    } else if (s.token || s.imm) {
        switch (message) {
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP:
            if (w == VK_PROCESSKEY) return 0;
            break;
        case WM_IME_CHAR:
            return 0; // GCS_RESULTSTR and TSF are the sole writers.
        case WM_IME_STARTCOMPOSITION: case WM_IME_COMPOSITION: case WM_IME_ENDCOMPOSITION:
            if (!s.busy && s.imm) {
                Busy busy;
                try {
                    const unsigned attachment = s.attachment;
                    sync();
                    if (message != WM_IME_STARTCOMPOSITION && attachment != s.attachment) return 0;
                    if (s.imm) {
                        if (message == WM_IME_STARTCOMPOSITION) s.immComposing = true;
                        if (message == WM_IME_COMPOSITION) immResult(l);
                        if (message == WM_IME_ENDCOMPOSITION) s.immComposing = false;
                    }
                } catch (...) { detach(); }
            }
            // IMM shows composition and candidates via the default IME window.
            // Do not give result text to DefWindowProc, which can synthesize WM_IME_CHAR.
            if (s.imm && !(message == WM_IME_COMPOSITION && (l & GCS_RESULTSTR)))
                return DefWindowProcW(window, message, w, l);
            if (s.imm && (l & GCS_COMPSTR))
                return DefWindowProcW(window, message, w, l & ~static_cast<LPARAM>(GCS_RESULTSTR));
            return 0;
        case WM_IME_SETCONTEXT: case WM_IME_NOTIFY:
            return DefWindowProcW(window, message, w, l);
        }
    }
    return DefSubclassProc(window, message, w, l);
}

void stop() {
    if (s.hook) UnhookWindowsHookEx(s.hook);
    s.hook = nullptr;
    detach();
    if (s.window) RemoveWindowSubclass(s.window, wndProc, SubclassId);
    release(s.keys);
    release(s.profiles);
    if (s.activated) s.manager->Deactivate();
    release(s.manager);
    if (s.coInitialized) CoUninitialize();
    const bool busy = s.busy;
    s = State{};
    s.busy = busy;
}

int ImeStart(Refresh refresh, Commit commitCallback) {
    if (s.busy) return 0;
    HWND window = gameWindow();
    if (s.hook && s.window && (!window || window == s.window)) return 1;
    Busy busy;
    if (s.hook) stop();
    if (!window || !refresh || !commitCallback) return 0;
    const DWORD thread = GetCurrentThreadId();
    s.window = window;
    s.thread = thread;
    s.refresh = refresh;
    s.commit = commitCallback;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    s.coInitialized = SUCCEEDED(co);
    if (s.coInitialized || co == RPC_E_CHANGED_MODE) {
        CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER, __uuidof(ITfThreadMgr),
                         reinterpret_cast<void**>(&s.manager));
        s.activated = s.manager && SUCCEEDED(s.manager->Activate(&s.client));
        if (s.activated)
            s.manager->QueryInterface(__uuidof(ITfKeystrokeMgr), reinterpret_cast<void**>(&s.keys));
        if (!s.keys) {
            if (s.activated) s.manager->Deactivate();
            s.activated = false;
            release(s.manager);
        }
        CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr, CLSCTX_INPROC_SERVER,
                         __uuidof(ITfInputProcessorProfileMgr), reinterpret_cast<void**>(&s.profiles));
    }
    if (!SetWindowSubclass(window, wndProc, SubclassId, 0) ||
        !(s.hook = SetWindowsHookExW(WH_GETMESSAGE, getMessage, nullptr, thread))) {
        stop();
        return 0;
    }
    return 1;
}

void WINAPI ImeStop() {
    if (s.busy || !s.thread) return;
    Busy busy;
    stop();
}

void ImeTick(unsigned serial, unsigned row) {
    if (s.busy || !s.hook) return;
    Busy busy;
    try {
        flushTsf();
        sync();
        sampleCandidateUI();
        if (serial && chooseCandidate(serial, row)) flushTsf();
    } catch (...) { detach(); }
}

int WINAPI ImeStatus() {
    if (!s.hook || !s.window) return 0;
    if (s.token) return s.imm ? 3 : 2;
    return s.attachFailed && s.sample.token ? 4 : 1;
}

void WINAPI ImeSnapshot(unsigned token, const wchar_t* text,
                                                       int first, int last, int leading, const int* rects) {
    s.sample.token = 0;
    if (!token || !text || !rects || first < 0 || last < first) return;
    size_t length = 0;
    while (length <= TextStore::Capacity && text[length]) ++length;
    if (length > TextStore::Capacity || static_cast<size_t>(last) > length) return;
    for (int i = 0; i < 8; ++i) if (rects[i] < 0 || rects[i] > 10000) return;
    if (rects[0] > rects[2] || rects[1] >= rects[3] || rects[4] >= rects[6] || rects[5] >= rects[7]) return;
    try {
        Draft draft{std::wstring(text, length), first, last, first != last && leading != 0};
        if (!valid(draft)) return;
        s.sample.draft = std::move(draft);
        std::copy_n(rects, 8, s.sample.rects.begin());
        s.sample.token = token;
    } catch (...) { }
}

thread_local PyObject* pyRefresh = nullptr;
thread_local PyObject* pyCommit = nullptr;

// Window callbacks can run outside a Python call, or during a nested Python call.
struct PythonCallback {
    PyGILState_STATE gil = PyGILState_Ensure();
    PyObject *type, *value, *trace;
    PythonCallback() { PyErr_Fetch(&type, &value, &trace); }
    ~PythonCallback() {
        PyErr_Restore(type, value, trace);
        PyGILState_Release(gil);
    }
};

void WINAPI refreshPython() {
    if (!pyRefresh || !Py_IsInitialized()) return;
    PythonCallback lock;
    PyObject* callback = pyRefresh;
    Py_INCREF(callback);
    PyObject* result = PyObject_CallObject(callback, nullptr);
    if (!result) PyErr_WriteUnraisable(callback);
    Py_XDECREF(result);
    Py_DECREF(callback);
}

int WINAPI commitPython(unsigned token, const wchar_t* before, int first, int last, int leading,
                        const wchar_t* after, int newFirst, int newLast, int newLeading) {
    if (!pyCommit || !Py_IsInitialized()) return 0;
    PythonCallback lock;
    PyObject* callback = pyCommit;
    Py_INCREF(callback);
    char format[] = "(Iuiiiuiii)";
    PyObject* result = PyObject_CallFunction(callback, format, token, before, first, last,
                                             leading, after, newFirst, newLast, newLeading);
    int accepted = result ? PyObject_IsTrue(result) : -1;
    if (accepted < 0) PyErr_WriteUnraisable(callback);
    Py_XDECREF(result);
    Py_DECREF(callback);
    return accepted > 0;
}

PyObject* startPython(PyObject*, PyObject* args) {
    PyObject *refresh, *commit;
    if (!PyArg_ParseTuple(args, "OO:ImeStart", &refresh, &commit)) return nullptr;
    if (!PyCallable_Check(refresh) || !PyCallable_Check(commit)) {
        PyErr_SetString(PyExc_TypeError, "ImeStart requires two callbacks");
        return nullptr;
    }
    const int attached = ImeStart(refreshPython, commitPython);
    if (attached && (refresh != pyRefresh || commit != pyCommit)) {
        Py_INCREF(refresh);
        Py_INCREF(commit);
        Py_XDECREF(pyRefresh);
        Py_XDECREF(pyCommit);
        pyRefresh = refresh;
        pyCommit = commit;
    }
    return PyBool_FromLong(attached);
}

PyObject* stopPython(PyObject*, PyObject*) {
    ImeStop();
    Py_CLEAR(pyRefresh);
    Py_CLEAR(pyCommit);
    Py_RETURN_NONE;
}

PyObject* tickPython(PyObject*, PyObject* args) {
    unsigned serial = 0, row = 0;
    if (!PyArg_ParseTuple(args, "|II:ImeTick", &serial, &row)) return nullptr;
    ImeTick(serial, row);
    Py_RETURN_NONE;
}

PyObject* candidatesPython(PyObject*, PyObject*) {
    const auto& page = s.candidates;
    PyObject* labels = PyList_New(page.labels.size());
    if (!labels) return nullptr;
    for (size_t i = 0; i < page.labels.size(); ++i) {
        const auto& text = page.labels[i];
        PyObject* label = PyUnicode_FromUnicode(text.data(), text.size());
        if (!label) { Py_DECREF(labels); return nullptr; }
        PyList_SET_ITEM(labels, i, label);
    }
    return Py_BuildValue("(IiON)", page.serial, page.selection, page.clickable ? Py_True : Py_False, labels);
}

PyObject* statusPython(PyObject*, PyObject*) { return PyInt_FromLong(ImeStatus()); }

PyObject* diagnosticsPython(PyObject*, PyObject*) {
    ITfDocumentMgr* focused = nullptr;
    const bool ownsFocus = s.document && s.manager->GetFocus(&focused) == S_OK && focused == s.document;
    release(focused);
    CLSID tip{};
    const bool hasTip = s.keys && s.keys->GetForeground(&tip) == S_OK && !IsEqualGUID(tip, GUID_NULL);
    DWORD flags = 0;
    const bool flagsKnown = activeTsfFlags(flags);
    const auto& d = diagnostics;
    char report[640];
    std::snprintf(report, sizeof report,
                  "focus=%d tip=%d blocked=%u down=%u processKey=%u wanted=%u handled=%u noSink=%u failed=%u "
                  "composition=%u caret=%u/%u draftDetach=%u tsfFlags=%d/0x%lx uiSamples=%u candidates=%u shown=%u "
                  "overlay=%u/%u choices=%u/0x%lx imeContext=%u/0x%lx immCandidate=%u/%u/%u "
                  "caretScreen=%ld,%ld,%ld,%ld",
                  ownsFocus, hasTip, d.blocked, d.keyDown, d.processKey, d.wanted, d.eaten, d.noSink, d.failed,
                  d.store.composition, d.store.textExtOk, d.store.textExt, d.draftDetach, flagsKnown, flags,
                  d.uiSamples, d.candidates, d.shown, d.overlayRows, d.overlayClickable, d.choices,
                  static_cast<DWORD>(d.choiceResult), d.imeSetContext, d.imeFlags,
                  d.immOpen, d.immChange, d.immClose, d.caret.left, d.caret.top, d.caret.right, d.caret.bottom);
    return PyString_FromString(report);
}

PyObject* clearPython(PyObject*, PyObject*) {
    s.sample.token = 0;
    Py_RETURN_NONE;
}

PyObject* snapshotPython(PyObject*, PyObject* args) {
    unsigned token;
    const Py_UNICODE* text;
    Py_ssize_t length;
    int first, last, leading, r[8];
    if (!PyArg_ParseTuple(args, "Iu#iii(iiiiiiii):ImeSnapshot", &token, &text, &length, &first, &last,
                          &leading, &r[0], &r[1], &r[2], &r[3], &r[4], &r[5], &r[6], &r[7])) return nullptr;
    if (length > TextStore::Capacity || std::wmemchr(text, L'\0', length)) {
        PyErr_SetString(PyExc_ValueError, "Invalid UTF-16 snapshot");
        return nullptr;
    }
    ImeSnapshot(token, text, first, last, leading, r);
    Py_RETURN_NONE;
}

PyMethodDef methods[] = {
    {"ImeStart", startPython, METH_VARARGS, nullptr},
    {"ImeStop", stopPython, METH_NOARGS, nullptr},
    {"ImeTick", tickPython, METH_VARARGS, nullptr},
    {"ImeCandidates", candidatesPython, METH_NOARGS, nullptr},
    {"ImeStatus", statusPython, METH_NOARGS, nullptr},
    {"ImeDiagnostics", diagnosticsPython, METH_NOARGS, nullptr},
    {"ImeClear", clearPython, METH_NOARGS, nullptr},
    {"ImeSnapshot", snapshotPython, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};
}

PyMODINIT_FUNC init_wot_native_ime() { Py_InitModule("_wot_native_ime", methods); }
#endif
