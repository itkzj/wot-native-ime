#define NOMINMAX
#include "TextStore.h"
#include "CandidateList.h"
#include <cassert>
#include <cstdio>

std::wstring testPreedit(ITfThreadMgr*, ITfContext*, TextStore*);
bool testCandidateUI(ITfThreadMgr*, ITfDocumentMgr*, TextStore*, ITfUIElement*, ITfUIElement*);
bool testImmCandidatePage(const void*, DWORD, bool);
bool testImmPreview();

struct Candidate final : ITfCandidateListUIElementBehavior {
    ITfDocumentMgr* document;
    ULONG refs = 1;
    UINT selection = 3, finalizations = 0;
    bool shown = false;
    explicit Candidate(ITfDocumentMgr* owner) : document(owner) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        *out = nullptr;
        if (iid != IID_IUnknown && iid != __uuidof(ITfUIElement) && iid != candidateListId &&
            iid != candidateBehaviorId) return E_NOINTERFACE;
        *out = static_cast<ITfCandidateListUIElementBehavior*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { return --refs; }
    HRESULT STDMETHODCALLTYPE GetDescription(BSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetGUID(GUID* guid) override { *guid = candidateListId; return S_OK; }
    HRESULT STDMETHODCALLTYPE Show(BOOL show) override { shown = show != FALSE; return S_OK; }
    HRESULT STDMETHODCALLTYPE IsShown(BOOL* show) override { *show = shown; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr** owner) override {
        document->AddRef();
        *owner = document;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCount(UINT* count) override { *count = 4; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetSelection(UINT* index) override { *index = selection; return S_OK; }
    HRESULT STDMETHODCALLTYPE GetString(UINT index, BSTR* value) override {
        const wchar_t* rows[] = {L"first", L"second", L"\u6d4b", L"\u8bd5"};
        if (index >= 4) return E_INVALIDARG;
        *value = SysAllocString(rows[index]);
        return *value ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetPageIndex(UINT* offsets, UINT size, UINT* count) override {
        *count = 2;
        if (offsets) {
            if (size < 2) return E_INVALIDARG;
            offsets[0] = 0;
            offsets[1] = 2;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetPageIndex(UINT*, UINT) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT* page) override { *page = 1; return S_OK; }
    HRESULT STDMETHODCALLTYPE SetSelection(UINT index) override { selection = index; return S_OK; }
    HRESULT STDMETHODCALLTYPE Finalize() override { ++finalizations; return S_OK; }
    HRESULT STDMETHODCALLTYPE Abort() override { return E_NOTIMPL; }
};

int main() {
    // CANDIDATELIST with three UTF-16 entries and a second page starting at index 1.
    DWORD imm[] = {48, 0, 3, 2, 1, 2, 36, 40, 44, 'a', 0x4f60, 0x597d};
    assert(testImmCandidatePage(imm, sizeof imm, true));
    assert(testImmCandidatePage(imm, sizeof imm - 1, false));
    imm[8] = 46;
    imm[11] = 0x597d597d;
    assert(testImmCandidatePage(imm, sizeof imm, false));
    assert(testImmPreview());

    assert(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)));
    ITfThreadMgr* manager = nullptr;
    assert(SUCCEEDED(CoCreateInstance(CLSID_TF_ThreadMgr, nullptr, CLSCTX_INPROC_SERVER,
                                     __uuidof(ITfThreadMgr), reinterpret_cast<void**>(&manager))));
    TfClientId id = 0;
    ITfThreadMgrEx* extended = nullptr;
    assert(manager->QueryInterface(__uuidof(ITfThreadMgrEx), reinterpret_cast<void**>(&extended)) == S_OK);
    assert(SUCCEEDED(extended->ActivateEx(&id, TF_TMAE_UIELEMENTENABLEDONLY)));
    extended->Release();
    ITfDocumentMgr* document = nullptr;
    assert(SUCCEEDED(manager->CreateDocumentMgr(&document)));
    auto* store = new TextStore(nullptr, {L"abc", 1, 2, false});
    ITfContext* context = nullptr;
    TfEditCookie cookie = 0;
    assert(SUCCEEDED(document->CreateContext(id, 0, static_cast<ITextStoreACP*>(store), &context, &cookie)));
    assert(SUCCEEDED(document->Push(context)));
    LONG end = 0;
    assert(store->GetEndACP(&end) == TS_E_NOLOCK);
    LONG first = 0, last = 0;
    assert(store->QueryInsert(1, 2, 2, &first, &last) == S_OK && first == 1 && last == 3);
    assert(store->QueryInsert(2, 4, 1, &first, &last) == TS_E_INVALIDPOS);

    // Run an actual TSF edit session so the store's selection, write lock and
    // replacement are exercised by the same COM context used by the mod.
    struct Insert final : ITfEditSession, ITfCompositionSink {
        ITfContext* ctx;
        TextStore* store;
        ITfComposition* composition = nullptr;
        Insert(ITfContext* context, TextStore* textStore) : ctx(context), store(textStore) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
            if (iid == __uuidof(IUnknown) || iid == __uuidof(ITfEditSession)) *out = static_cast<ITfEditSession*>(this);
            else if (iid == __uuidof(ITfCompositionSink)) *out = static_cast<ITfCompositionSink*>(this);
            else { *out = nullptr; return E_NOINTERFACE; }
            AddRef();
            return S_OK;
        }
        ULONG refs = 1;
        ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
        ULONG STDMETHODCALLTYPE Release() override { return --refs; }
        HRESULT STDMETHODCALLTYPE OnCompositionTerminated(TfEditCookie, ITfComposition*) override { return S_OK; }
        HRESULT STDMETHODCALLTYPE DoEditSession(TfEditCookie token) override {
            if (composition) {
                ITfRange* range = nullptr;
                HRESULT result = composition->GetRange(&range);
                if (SUCCEEDED(result)) {
                    result = range->SetText(token, 0, L"\u4f60\u597d", 2);
                    range->Release();
                }
                if (SUCCEEDED(result)) result = composition->EndComposition(token);
                composition->Release();
                composition = nullptr;
                return result;
            }
            ITfInsertAtSelection* insertion = nullptr;
            HRESULT result = ctx->QueryInterface(__uuidof(ITfInsertAtSelection), reinterpret_cast<void**>(&insertion));
            if (FAILED(result)) return result;
            ITfRange* range = nullptr;
            result = insertion->InsertTextAtSelection(token, 0, L"nihao", 5, &range);
            insertion->Release();
            if (FAILED(result)) {
                std::fprintf(stderr, "TSF insertion: 0x%08lx\n", static_cast<unsigned long>(result));
                return result;
            }
            ITfContextComposition* owner = nullptr;
            result = ctx->QueryInterface(__uuidof(ITfContextComposition), reinterpret_cast<void**>(&owner));
            if (SUCCEEDED(result)) {
                result = owner->StartComposition(token, range, this, &composition);
                if (FAILED(result)) std::fprintf(stderr, "TSF StartComposition: 0x%08lx\n", static_cast<unsigned long>(result));
                if (SUCCEEDED(result)) assert(composition && store->composing);
                owner->Release();
            }
            range->Release();
            return result;
        }
    } insert(context, store);
    HRESULT outcome = E_FAIL;
    assert(SUCCEEDED(context->RequestEditSession(id, &insert, TF_ES_SYNC | TF_ES_READWRITE, &outcome)));
    if (FAILED(outcome)) std::fprintf(stderr, "TSF edit session result: 0x%08lx\n", static_cast<unsigned long>(outcome));
    assert(SUCCEEDED(outcome));
    assert(testPreedit(manager, context, store) == L"nihao");
    assert(SUCCEEDED(context->RequestEditSession(id, &insert, TF_ES_SYNC | TF_ES_READWRITE, &outcome)));
    assert(SUCCEEDED(outcome));
    assert(testPreedit(manager, context, store).empty());
    assert(store->draft.text == L"a\u4f60\u597dc" && store->draft.first == 3 && store->draft.last == 3);
    assert(!store->composing && store->finishedComposition);

    // The test candidate lives in a separate translation unit from the mod's COM client.
    ITfDocumentMgr* other = nullptr;
    assert(manager->CreateDocumentMgr(&other) == S_OK);
    Candidate candidate(document), foreign(other);
    candidate.shown = foreign.shown = true;
    assert(testCandidateUI(manager, document, store, &candidate, &foreign));
    assert(!candidate.shown && foreign.shown);
    assert(candidate.refs == 1 && foreign.refs == 1);
    assert(candidate.selection == 2 && candidate.finalizations == 1 && !foreign.finalizations);
    other->Release();

    document->Pop(TF_POPF_ALL);
    context->Release();
    document->Release();
    store->Release();
    manager->Deactivate();
    manager->Release();
    CoUninitialize();
    std::puts("TSF document, selection replacement, composition lifecycle and candidate UI passed");
}
