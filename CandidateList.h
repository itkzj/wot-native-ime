#pragma once
#include <msctf.h>

#ifndef __ITfCandidateListUIElement_INTERFACE_DEFINED__
// Windows SDK ABI missing from older MinGW headers. Keep external linkage, since
// the implementation is supplied by a Windows text service in a different DLL.
struct ITfCandidateListUIElement : ITfUIElement {
    virtual HRESULT STDMETHODCALLTYPE GetUpdatedFlags(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDocumentMgr(ITfDocumentMgr**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCount(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetSelection(UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetString(UINT, BSTR*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPageIndex(UINT*, UINT, UINT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPageIndex(UINT*, UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentPage(UINT*) = 0;
};
#endif
inline constexpr IID candidateListId{0xea1ea138, 0x19df, 0x11d7, {0xa6, 0xd2, 0x00, 0x06, 0x5b, 0x84, 0x43, 0x5c}};

#ifndef __ITfCandidateListUIElementBehavior_INTERFACE_DEFINED__
struct ITfCandidateListUIElementBehavior : ITfCandidateListUIElement {
    virtual HRESULT STDMETHODCALLTYPE SetSelection(UINT) = 0;
    virtual HRESULT STDMETHODCALLTYPE Finalize() = 0;
    virtual HRESULT STDMETHODCALLTYPE Abort() = 0;
};
#endif
inline constexpr IID candidateBehaviorId{0x85fad185, 0x58ce, 0x497a, {0x94, 0x60, 0x35, 0x53, 0x66, 0xb6, 0x4b, 0x9a}};
