#pragma once
#include <windows.h>
#include <msctf.h>
#include <textstor.h>
#include <olectl.h>
#include <algorithm>
#include <string>

// All document positions are UTF-16 offsets, like the Scaleform TextField.
struct Draft {
    std::wstring text;
    LONG first = 0, last = 0;
    bool leading = false;
    bool operator==(const Draft& rhs) const {
        return text == rhs.text && first == rhs.first && last == rhs.last && leading == rhs.leading;
    }
};

class TextStore final : public ITextStoreACP, public ITfContextOwnerCompositionSink {
    ULONG refs = 1;
    ITextStoreACPSink* sink = nullptr;
    DWORD mask = 0, lock = 0, pending = 0;
    bool range(LONG first, LONG last) const { return first >= 0 && last >= first && last <= end(); }
    LONG end() const { return static_cast<LONG>(draft.text.size()); }
    bool read() const { return (lock & TS_LF_READ) != 0; }
    bool write() const { return (lock & TS_LF_READWRITE) == TS_LF_READWRITE; }

public:
    static constexpr LONG Capacity = 8192; // Includes transient preedit.
    HWND window;
    Draft draft;
    RECT caret{}, field{}; // Screen coordinates in the window's DPI context.
    bool composing = false, acceptComposition = true;
    unsigned composition = 0;
    bool cancelled = false, finishedComposition = false;
    explicit TextStore(HWND hwnd, const Draft& initial) : window(hwnd), draft(initial) {}
    ~TextStore() { if (sink) sink->Release(); }

    void updateDraft(const Draft& value) {
        const auto oldEnd = end();
        const bool changed = value.text != draft.text;
        const bool selection = value.first != draft.first || value.last != draft.last || value.leading != draft.leading;
        draft = value;
        if (sink && changed && (mask & TS_AS_TEXT_CHANGE)) {
            TS_TEXTCHANGE change{0, oldEnd, end()};
            sink->OnTextChange(0, &change);
        }
        if (sink && selection && (mask & TS_AS_SEL_CHANGE)) sink->OnSelectionChange();
    }
    void layout(const RECT& newCaret, const RECT& newField) {
        if (EqualRect(&caret, &newCaret) && EqualRect(&field, &newField)) return;
        caret = newCaret;
        field = newField;
        if (sink && (mask & TS_AS_LAYOUT_CHANGE)) sink->OnLayoutChange(TS_LC_CHANGE, 1);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(ITextStoreACP)) *out = static_cast<ITextStoreACP*>(this);
        else if (iid == __uuidof(ITfContextOwnerCompositionSink)) *out = static_cast<ITfContextOwnerCompositionSink*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs; if (!n) delete this; return n; }

    HRESULT STDMETHODCALLTYPE AdviseSink(REFIID iid, IUnknown* object, DWORD flags) override {
        if (iid != __uuidof(ITextStoreACPSink) || !object) return E_INVALIDARG;
        ITextStoreACPSink* next = nullptr;
        const HRESULT result = object->QueryInterface(__uuidof(ITextStoreACPSink), reinterpret_cast<void**>(&next));
        if (FAILED(result)) return result;
        if (sink && sink != next) { next->Release(); return CONNECT_E_ADVISELIMIT; }
        if (sink) sink->Release();
        sink = next;
        mask = flags;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnadviseSink(IUnknown* object) override {
        if (!sink || !object) return CONNECT_E_NOCONNECTION;
        ITextStoreACPSink* current = nullptr;
        if (FAILED(object->QueryInterface(__uuidof(ITextStoreACPSink), reinterpret_cast<void**>(&current))))
            return CONNECT_E_NOCONNECTION;
        current->Release();
        if (current != sink) return CONNECT_E_NOCONNECTION;
        current = sink;
        sink = nullptr;
        mask = 0;
        current->Release();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RequestLock(DWORD flags, HRESULT* result) override {
        if (!sink || !result) return E_UNEXPECTED;
        if (lock) {
            if (flags & TS_LF_SYNC) *result = TS_E_SYNCHRONOUS;
            else { pending |= flags & TS_LF_READWRITE; *result = TS_S_ASYNC; }
            return S_OK;
        }
        auto grant = [this](DWORD kind) {
            lock = kind & TS_LF_READWRITE;
            auto* current = sink;
            current->AddRef();
            const HRESULT outcome = current->OnLockGranted(lock);
            lock = 0;
            current->Release();
            return outcome;
        };
        *result = grant(flags);
        while (sink && pending) {
            const DWORD kind = pending;
            pending = 0;
            grant(kind);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStatus(TS_STATUS* status) override {
        *status = {0, TS_SS_NOHIDDENTEXT};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE QueryInsert(LONG first, LONG last, ULONG count, LONG* start, LONG* finish) override {
        if (!range(first, last)) return TS_E_INVALIDPOS;
        if (count > static_cast<ULONG>(Capacity - end() + last - first)) return E_INVALIDARG;
        *start = first;
        *finish = first + static_cast<LONG>(count);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetSelection(ULONG index, ULONG count, TS_SELECTION_ACP* sel, ULONG* fetched) override {
        if (!read()) return TS_E_NOLOCK;
        if (index != TS_DEFAULT_SELECTION && index != 0) return TS_E_NOSELECTION;
        *fetched = 0;
        if (count) {
            *sel = {draft.first, draft.last, {draft.leading ? TS_AE_START : TS_AE_END, FALSE}};
            *fetched = 1;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSelection(ULONG count, const TS_SELECTION_ACP* sel) override {
        if (!write()) return TS_E_NOLOCK;
        if (count != 1 || !range(sel->acpStart, sel->acpEnd)) return TS_E_INVALIDPOS;
        draft.first = sel->acpStart;
        draft.last = sel->acpEnd;
        draft.leading = sel->style.ase == TS_AE_START;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetText(LONG first, LONG last, WCHAR* out, ULONG size, ULONG* copied,
                                      TS_RUNINFO* runs, ULONG runSize, ULONG* runCount, LONG* next) override {
        if (!read()) return TS_E_NOLOCK;
        if (last == -1) last = end();
        if (!range(first, last)) return TS_E_INVALIDPOS;
        const ULONG count = size ? std::min(size, static_cast<ULONG>(last - first)) : static_cast<ULONG>(last - first);
        *copied = size ? count : 0;
        if (*copied) std::copy_n(draft.text.data() + first, *copied, out);
        *runCount = runSize && count ? 1U : 0U;
        if (*runCount) *runs = {count, TS_RT_PLAIN};
        *next = first + ((*copied || *runCount) ? count : 0);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetText(DWORD, LONG first, LONG last, const WCHAR* value, ULONG count,
                                      TS_TEXTCHANGE* change) override {
        if (!write()) return TS_E_NOLOCK;
        if (count && !value) return E_INVALIDARG;
        LONG start, finish;
        const HRESULT result = QueryInsert(first, last, count, &start, &finish);
        if (FAILED(result)) return result;
        try { draft.text.replace(first, last - first, value ? value : L"", count); }
        catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        draft.first = draft.last = finish;
        draft.leading = false;
        *change = {first, last, finish};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE InsertTextAtSelection(DWORD flags, const WCHAR* value, ULONG count,
                                                     LONG* first, LONG* last, TS_TEXTCHANGE* change) override {
        if (flags & TS_IAS_QUERYONLY) {
            if (!read()) return TS_E_NOLOCK;
            LONG unused, finish;
            const HRESULT result = QueryInsert(draft.first, draft.last, count, &unused, &finish);
            if (SUCCEEDED(result)) { *first = draft.first; *last = draft.last; }
            return result;
        }
        TS_TEXTCHANGE applied{};
        const HRESULT result = SetText(0, draft.first, draft.last, value, count, &applied);
        if (SUCCEEDED(result)) {
            if (!(flags & TS_IAS_NOQUERY)) { *first = applied.acpStart; *last = applied.acpNewEnd; }
            if (change) *change = applied;
        }
        return result;
    }
    HRESULT STDMETHODCALLTYPE GetEndACP(LONG* pos) override {
        if (!read()) return TS_E_NOLOCK;
        *pos = end();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetActiveView(TsViewCookie* view) override { *view = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetWnd(TsViewCookie view, HWND* hwnd) override {
        if (view != 1) return E_INVALIDARG;
        *hwnd = window;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetScreenExt(TsViewCookie view, RECT* bounds) override {
        if (view != 1) return E_INVALIDARG;
        if (IsRectEmpty(&field)) return TS_E_NOLAYOUT;
        *bounds = field;
        return S_OK;
    }
    // ponytail: this adapter has caret geometry only; add per-character layout for reconversion.
    HRESULT STDMETHODCALLTYPE GetTextExt(TsViewCookie view, LONG first, LONG last, RECT* bounds, BOOL* clipped) override {
        if (!read()) return TS_E_NOLOCK;
        if (view != 1 || !range(first, last)) return TS_E_INVALIDPOS;
        if (IsRectEmpty(&caret)) return TS_E_NOLAYOUT;
        *bounds = caret;
        *clipped = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetACPFromPoint(TsViewCookie view, const POINT* point, DWORD flags, LONG* pos) override {
        if (!read()) return TS_E_NOLOCK;
        if (view != 1) return E_INVALIDARG;
        if (!(flags & GXFPF_NEAREST) && !PtInRect(&caret, *point)) return TS_E_INVALIDPOINT;
        *pos = draft.leading ? draft.first : draft.last;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetFormattedText(LONG, LONG, IDataObject**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetEmbedded(LONG, REFGUID, REFIID, IUnknown**) override { return TS_E_NOOBJECT; }
    HRESULT STDMETHODCALLTYPE QueryInsertEmbedded(const GUID*, const FORMATETC*, BOOL* yes) override { *yes = FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE InsertEmbedded(DWORD, LONG, LONG, IDataObject*, TS_TEXTCHANGE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE InsertEmbeddedAtSelection(DWORD, IDataObject*, LONG*, LONG*, TS_TEXTCHANGE*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RequestSupportedAttrs(DWORD, ULONG, const TS_ATTRID*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RequestAttrsAtPosition(LONG, ULONG, const TS_ATTRID*, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE RequestAttrsTransitioningAtPosition(LONG, ULONG, const TS_ATTRID*, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE FindNextAttrTransition(LONG, LONG halt, ULONG, const TS_ATTRID*, DWORD,
                                                     LONG* next, BOOL* found, LONG* offset) override {
        *next = halt;
        *found = FALSE;
        *offset = 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE RetrieveRequestedAttrs(ULONG, TS_ATTRVAL*, ULONG* found) override { *found = 0; return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStartComposition(ITfCompositionView*, BOOL* yes) override {
        *yes = acceptComposition && !composing;
        if (*yes) {
            composing = true;
            ++composition;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnUpdateComposition(ITfCompositionView*, ITfRange*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnEndComposition(ITfCompositionView*) override {
        finishedComposition = composing;
        composing = false;
        return S_OK;
    }
};
