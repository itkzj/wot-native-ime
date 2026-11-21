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
#include <cstring>
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
    std::wstring preedit;
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
    CandidatePage candidates, immCandidates;
    DWORD immListMask = 0;
    bool readingImm = false;
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
struct Busy {
    Busy() { s.busy = true; }
    ~Busy() { s.busy = false; }
};

template<class T> void release(T*& value) {
    if (value) { auto* old = value; value = nullptr; old->Release(); }
}

#ifdef TEST_CANDIDATE_UI
bool activeTsfFlags(DWORD& flags) {
    ITfThreadMgrEx* managerEx = nullptr;
    const bool known = s.manager &&
        SUCCEEDED(s.manager->QueryInterface(__uuidof(ITfThreadMgrEx), reinterpret_cast<void**>(&managerEx))) &&
        SUCCEEDED(managerEx->GetActiveFlags(&flags));
    release(managerEx);
    return known;
}
#endif

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
constexpr DWORD MaxCandidateBytes = 64 * 1024;

bool readImmPage(const void* buffer, DWORD bytes, CandidatePage& page) {
    constexpr auto header = offsetof(CANDIDATELIST, dwOffset);
    CANDIDATELIST list{};
    if (bytes < sizeof list) return false;
    std::memcpy(&list, buffer, sizeof list);
    if (list.dwSize > bytes || list.dwSize < sizeof list ||
        list.dwCount > (list.dwSize - header) / sizeof(DWORD) ||
        list.dwPageStart >= list.dwCount || !list.dwPageSize) return false;
    const auto* data = static_cast<const unsigned char*>(buffer);
    const auto strings = header + list.dwCount * sizeof(DWORD);
    page.first = list.dwPageStart;
    const auto rows = std::min({list.dwCount - list.dwPageStart, list.dwPageSize,
                                static_cast<DWORD>(MaxCandidateRows)});
    for (DWORD row = 0; row < rows; ++row) {
        DWORD offset = 0;
        std::memcpy(&offset, data + header + (page.first + row) * sizeof(DWORD), sizeof offset);
        if (offset < strings || offset >= list.dwSize || offset % sizeof(wchar_t)) return false;
        std::wstring text;
        bool terminated = false;
        while (list.dwSize - offset >= sizeof(wchar_t) && text.size() <= 256) {
            wchar_t character;
            std::memcpy(&character, data + offset, sizeof character);
            offset += sizeof character;
            if (!character) { terminated = true; break; }
            text.push_back(character);
        }
        if (!terminated || text.empty()) return false;
        page.labels.push_back(std::move(text));
    }
    if (list.dwSelection >= page.first && list.dwSelection - page.first < rows)
        page.selection = static_cast<int>(list.dwSelection - page.first);
    return true;
}

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

std::wstring compositionText(ITfContext* context, const TextStore& store) {
    if (!context || !store.composing) return {};
    ITfContextComposition* composition = nullptr;
    if (context->QueryInterface(__uuidof(ITfContextComposition), reinterpret_cast<void**>(&composition)) != S_OK)
        return {};
    IEnumITfCompositionView* views = nullptr;
    const HRESULT enumerated = composition->EnumCompositions(&views);
    composition->Release();
    Com<IEnumITfCompositionView> ownedViews(views);
    if (enumerated != S_OK || !views) return {};

    ITfCompositionView* view = nullptr;
    ULONG fetched = 0;
    const HRESULT found = views->Next(1, &view, &fetched);
    Com<ITfCompositionView> ownedView(view);
    if (found != S_OK || fetched != 1 || !view) return {};
    ITfRange* range = nullptr;
    const HRESULT ranged = view->GetRange(&range);
    Com<ITfRange> ownedRange(range);
    if (ranged != S_OK || !range) return {};
    ITfRangeACP* acp = nullptr;
    const HRESULT queried = range->QueryInterface(__uuidof(ITfRangeACP), reinterpret_cast<void**>(&acp));
    Com<ITfRangeACP> ownedAcp(acp);
    if (queried != S_OK || !acp) return {};

    LONG first = 0, length = 0;
    const auto& text = store.draft.text;
    if (acp->GetExtent(&first, &length) != S_OK || first < 0 || length <= 0 ||
        static_cast<size_t>(first) > text.size() || static_cast<size_t>(length) > text.size() - first)
        return {};
    return text.substr(first, length);
}

struct ImmContext {
    HWND window;
    HIMC handle;
    explicit ImmContext(HWND window) : window(window), handle(ImmGetContext(window)) {}
    ~ImmContext() { if (handle) ImmReleaseContext(window, handle); }
};

std::wstring immText(HIMC handle, DWORD kind) {
    if (!handle) return {};
    const LONG bytes = ImmGetCompositionStringW(handle, kind, nullptr, 0);
    if (bytes <= 0 || bytes > TextStore::Capacity * 2 || bytes % 2 != 0) return {};
    std::wstring text(bytes / 2, L'\0');
    if (ImmGetCompositionStringW(handle, kind, text.data(), bytes) != bytes ||
        text.find(L'\0') != std::wstring::npos) return {};
    return text;
}

bool composing() { return s.store ? s.store->composing : s.immComposing; }

void sampleCandidateUI() {
    CandidatePage next;
    bool nativeVisible = false;
    if (s.store && s.store->composing) {
        auto* ui = uiManager();
        IEnumTfUIElements* raw = nullptr;
        if (ui) ui->EnumUIElements(&raw);
        release(ui);
        Com<IEnumTfUIElements> elements(raw);
        if (elements) {
            ITfUIElement* element = nullptr;
            ULONG fetched = 0;
            while (elements->Next(1, &element, &fetched) == S_OK && fetched == 1 && element) {
                Com<ITfUIElement> owned(element);
                auto list = ownedCandidate(element);
                element = nullptr;
                if (!list) continue;
                BOOL visible = FALSE;
                const HRESULT result = list->IsShown(&visible);
                if (result != S_OK) continue;
                if (!next.list) {
                    CandidatePage page;
                    page.list = std::move(list);
                    BOOL stillVisible = TRUE;
                    if (readCandidatePage(page) && (!visible ||
                        (page.list->Show(FALSE) == S_OK && page.list->IsShown(&stillVisible) == S_OK && !stillVisible))) {
                        next = std::move(page);
                        continue;
                    }
                }
                if (visible) {
                    nativeVisible = true;
                    next = CandidatePage{};
                    break;
                }
            }
            release(element);
        }
    }
    if (!next.list && !nativeVisible && s.immListMask && composing()) {
        next.first = s.immCandidates.first;
        next.selection = s.immCandidates.selection;
        next.labels = s.immCandidates.labels;
    }
    if (!nativeVisible) {
        if (s.store) next.preedit = compositionText(s.context, *s.store);
        else if (s.immComposing) {
            const ImmContext imc(s.window);
            next.preedit = immText(imc.handle, GCS_COMPSTR);
        }
    }
    if (!next.labels.empty() || !next.preedit.empty()) {
        const auto& old = s.candidates;
        next.composition = s.store ? s.store->composition : 0;
        if (next.list == old.list && next.composition == old.composition && next.first == old.first &&
            next.labels == old.labels && next.clickable == old.clickable && next.preedit == old.preedit) {
            next.serial = old.serial;
        } else {
            if (++candidateSerial == 0) ++candidateSerial;
            next.serial = candidateSerial;
        }
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
    // Offer the displayed page-relative accelerator to its owner. A partial
    // choice can then advance the composition just as a physical digit does.
    const char* route = "candidate behavior";
    BOOL handled = FALSE;
    if (row < 9) {
        NumberedCandidateUI* raw = nullptr;
        if (page.list->QueryInterface(numberedCandidateId, reinterpret_cast<void**>(&raw)) == S_OK && raw) {
            Com<NumberedCandidateUI> numbered(raw);
            BOOL numbers = FALSE;
            if (numbered->ShowCandidateNumbers(&numbers) == S_OK && numbers) {
                const WPARAM key = '1' + row;
                const LPARAM flags = 1 | (MapVirtualKeyW(static_cast<UINT>(key), MAPVK_VK_TO_VSC) << 16);
                if (FAILED(numbered->OnKeyDown(key, flags, &handled))) return false;
                if (handled) route = "numbered candidate";
            }
        }
    }
#ifndef TEST_CANDIDATE_UI
    static thread_local unsigned reports = 0;
    if (reports < 16) { ++reports; PySys_WriteStdout("[WotNativeIme] mouse route: %s\n", route); }
#else
    (void)route;
#endif
    if (handled) {
        s.candidates = CandidatePage{};
        return true;
    }
    if (!s.store || !s.store->composing || s.candidates.serial != serial ||
        s.store->composition != page.composition) return false;
    if (behavior->SetSelection(index) != S_OK) return false;
    // SetSelection can end the text-store composition synchronously. The selected
    // candidate behavior still needs its own confirmation to consume the reading.
    if (behavior->Finalize() != S_OK) return false;
    s.candidates = CandidatePage{};
    return true;
}

#ifdef TEST_CANDIDATE_UI
}

bool testImmCandidatePage(const void* buffer, DWORD bytes, bool valid) {
    CandidatePage page;
    if (!valid) return !readImmPage(buffer, bytes, page);
    return readImmPage(buffer, bytes, page) && page.first == 1 && page.selection == 1 &&
           page.labels == std::vector<std::wstring>{L"\u4f60", L"\u597d"};
}

bool testImmPreview() {
    s.imm = true;
    s.immComposing = true;
    s.immListMask = 1;
    s.immCandidates.labels = {L"\u4f60\u597d"};
    sampleCandidateUI();
    const bool shown = s.candidates.serial && s.candidates.labels.size() == 1;
    s.immComposing = false;
    sampleCandidateUI();
    const bool closed = !s.candidates.serial && s.candidates.labels.empty();
    s = State{};
    return shown && closed;
}

std::wstring testPreedit(ITfThreadMgr* manager, ITfContext* context, TextStore* store) {
    s.manager = manager;
    s.context = context;
    s.store = store;
    sampleCandidateUI();
    auto text = std::move(s.candidates.preedit);
    const bool visible = s.candidates.serial && s.candidates.labels.empty() && !s.candidates.clickable;
    s = State{};
    return visible ? std::move(text) : std::wstring{};
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
        success = success && s.candidates.serial == serial && !chooseCandidate(serial, 2);
        ++store->composition; // A new composition may reuse the very same list and labels.
        sampleCandidateUI();
        const auto current = s.candidates.serial;
        success = success && current && current != serial && !chooseCandidate(serial, 0) &&
                  chooseCandidate(current, 0) && !s.candidates.serial;
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

void clearImmCandidates();

void detach() {
    if (!s.token && !s.store) return;
    s.token = 0;
    s.candidates = CandidatePage{};
    clearImmCandidates();
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
    s.store = new (std::nothrow) TextStore(s.window, s.base);
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
            const RECT caret{a.x, a.y, b.x, b.y};
            s.store->layout(caret, {first.x, first.y, last.x, last.y});
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
    if (s.store->finishedComposition) clearImmCandidates();
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
        if (imm) DefWindowProcW(s.window, WM_IME_SETCONTEXT, TRUE, ISC_SHOWUIALL & ~ISC_SHOWUICOMPOSITIONWINDOW);
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
        const bool wasComposing = s.store->composing;
        BOOL wants = FALSE, handled = FALSE;
        const HRESULT test = down ? s.keys->TestKeyDown(msg.wParam, msg.lParam, &wants)
                                  : s.keys->TestKeyUp(msg.wParam, msg.lParam, &wants);
        if (test == S_OK && wants) {
            const HRESULT result = down ? s.keys->KeyDown(msg.wParam, msg.lParam, &handled)
                                        : s.keys->KeyUp(msg.wParam, msg.lParam, &handled);
            eaten = result == S_OK && handled;
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

void handleQueuedIme(MSG& msg);

LRESULT CALLBACK getMessage(int code, WPARAM kind, LPARAM param) {
    if (code == HC_ACTION && kind == PM_REMOVE && !s.busy) {
        Busy busy;
        auto& message = *reinterpret_cast<MSG*>(param);
        try {
            handleQueuedIme(message);
            handleKey(message);
        } catch (...) { detach(); }
    }
    return CallNextHookEx(nullptr, code, kind, param);
}

void clearImmCandidates() {
    s.immCandidates = CandidatePage{};
    s.immListMask = 0;
}

DWORD handleImmCandidates(WPARAM notification, DWORD mask) {
    if (notification == IMN_CLOSECANDIDATE) {
        const DWORD suppressed = mask & s.immListMask;
        if (suppressed) clearImmCandidates();
        return mask & ~suppressed;
    }
    if (!foreground() || s.readingImm) return mask;
    // A successful read owns one list until close. Keep unrelated lists on the default path.
    if (s.immListMask && !(mask & s.immListMask)) return mask;
    struct Reading {
        Reading() { s.readingImm = true; }
        ~Reading() { s.readingImm = false; }
    } reading;
    const auto attachment = s.attachment;
    const ImmContext imc(s.window);
    const DWORD wanted = s.immListMask ? s.immListMask : mask;
    for (DWORD index = 0; imc.handle && index < 32; ++index) {
        const DWORD bit = DWORD{1} << index;
        if (!(wanted & bit)) continue;
        const DWORD bytes = ImmGetCandidateListW(imc.handle, index, nullptr, 0);
        if (bytes < sizeof(CANDIDATELIST) || bytes > MaxCandidateBytes) continue;
        std::vector<DWORD> buffer((bytes + sizeof(DWORD) - 1) / sizeof(DWORD));
        const DWORD copied = ImmGetCandidateListW(imc.handle, index,
            reinterpret_cast<CANDIDATELIST*>(buffer.data()), bytes);
        CandidatePage page;
        if (copied > bytes || !readImmPage(buffer.data(), copied, page)) continue;
        if (!s.token || s.attachment != attachment || !foreground()) return mask;
        s.immCandidates = std::move(page);
        s.immListMask = bit;
        return mask & ~bit;
    }
    if (s.attachment == attachment) clearImmCandidates();
    return mask;
}

void immResult(LPARAM flags) {
    if (!(flags & GCS_RESULTSTR)) return;
    const ImmContext imc(s.window);
    const auto text = immText(imc.handle, GCS_RESULTSTR);
    if (text.empty()) return;
    Draft next = s.base;
    next.text.replace(next.first, next.last - next.first, text);
    next.first += static_cast<LONG>(text.size());
    next.last = next.first;
    next.leading = false;
    if (next.text.size() <= TextStore::Capacity && !commit(next)) detach();
}

DWORD filterCandidateNotification(WPARAM notification, DWORD mask) {
    switch (notification) {
    case IMN_OPENCANDIDATE:
    case IMN_CHANGECANDIDATE:
    case IMN_CLOSECANDIDATE:
        break;
    default: return mask;
    }
    if (!s.token || !foreground()) return mask;
    try { return handleImmCandidates(notification, mask); }
    catch (...) { clearImmCandidates(); return mask; }
}

void handleComposition(UINT message, LPARAM flags) {
    if (message == WM_IME_ENDCOMPOSITION ||
        (message == WM_IME_COMPOSITION && (flags & GCS_RESULTSTR) && !(flags & GCS_COMPSTR)))
        clearImmCandidates();
    if (!s.imm) return;
    const auto attachment = s.attachment;
    sync();
    if (!s.imm || (message != WM_IME_STARTCOMPOSITION && attachment != s.attachment)) return;
    if (message == WM_IME_STARTCOMPOSITION) s.immComposing = true;
    if (message == WM_IME_COMPOSITION) immResult(flags);
    if (message == WM_IME_ENDCOMPOSITION) s.immComposing = false;
}

void handleQueuedIme(MSG& msg) {
    if (msg.hwnd != s.window || !s.token || !foreground()) return;
    switch (msg.message) {
    case WM_IME_NOTIFY: {
        const auto original = static_cast<DWORD>(msg.lParam);
        const auto remaining = filterCandidateNotification(msg.wParam, original);
        if (remaining == original) return;
        if (remaining) { msg.lParam = remaining; return; }
        break;
    }
    case WM_IME_STARTCOMPOSITION: case WM_IME_COMPOSITION: case WM_IME_ENDCOMPOSITION:
        handleComposition(msg.message, msg.lParam);
        break;
    case WM_IME_CHAR:
        break;
    default:
        return;
    }
    // Scaleform may preprocess retrieved IME messages before dispatch reaches wndProc.
    msg.message = WM_NULL;
    msg.wParam = 0;
    msg.lParam = 0;
}

LRESULT CALLBACK wndProc(HWND window, UINT message, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
    if (message == WM_IME_NOTIFY) {
        const DWORD mask = filterCandidateNotification(w, static_cast<DWORD>(l));
        if (mask != static_cast<DWORD>(l))
            return mask ? DefWindowProcW(window, message, w, mask) : 0;
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
            if (!s.busy) {
                Busy busy;
                try { handleComposition(message, l); }
                catch (...) { detach(); }
            } else if (message == WM_IME_ENDCOMPOSITION) {
                clearImmCandidates();
            }
            return 0; // The mod renders preedit and owns confirmed text.
        case WM_IME_SETCONTEXT: {
            const DWORD hidden = (s.immListMask & ISC_SHOWUIALLCANDIDATEWINDOW) |
                                 (s.imm ? ISC_SHOWUICOMPOSITIONWINDOW : 0);
            return DefWindowProcW(window, message, w, l & ~static_cast<LPARAM>(hidden));
        }
        case WM_IME_NOTIFY:
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
    return Py_BuildValue("(IiONu#)", page.serial, page.selection, page.clickable ? Py_True : Py_False, labels,
                         page.preedit.c_str(), static_cast<Py_ssize_t>(page.preedit.size()));
}

PyObject* statusPython(PyObject*, PyObject*) { return PyInt_FromLong(ImeStatus()); }

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
    {"ImeClear", clearPython, METH_NOARGS, nullptr},
    {"ImeSnapshot", snapshotPython, METH_VARARGS, nullptr},
    {nullptr, nullptr, 0, nullptr},
};
}

PyMODINIT_FUNC init_wot_native_ime() { Py_InitModule("_wot_native_ime", methods); }
#endif
