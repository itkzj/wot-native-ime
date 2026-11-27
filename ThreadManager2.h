#pragma once
#include <msctf.h>

// The bundled MinGW headers omit the Windows 8 thread-manager interface.
#ifndef __ITfThreadMgr2_INTERFACE_DEFINED__
struct ITfThreadMgr2 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Activate(TfClientId*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Deactivate() = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateDocumentMgr(ITfDocumentMgr**) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumDocumentMgrs(IEnumTfDocumentMgrs**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFocus(ITfDocumentMgr**) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetFocus(ITfDocumentMgr*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsThreadFocus(BOOL*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetFunctionProvider(REFCLSID, ITfFunctionProvider**) = 0;
    virtual HRESULT STDMETHODCALLTYPE EnumFunctionProviders(IEnumTfFunctionProviders**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetGlobalCompartment(ITfCompartmentMgr**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ActivateEx(TfClientId*, DWORD) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetActiveFlags(DWORD*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SuspendKeystrokeHandling() = 0;
    virtual HRESULT STDMETHODCALLTYPE ResumeKeystrokeHandling() = 0;
};
__CRT_UUID_DECL(ITfThreadMgr2, 0x0ab198ef, 0x6477, 0x4ee8, 0x88, 0x12, 0x67, 0x80, 0xed, 0xb8, 0x2d, 0x5e)
#endif
