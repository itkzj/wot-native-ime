#pragma once
#include <windows.h>
#include <msctf.h>
#include <imm.h>
#include <memory>

struct ReleaseCom {
    template<class T> void operator()(T* value) const { value->Release(); }
};
template<class T> using Com = std::unique_ptr<T, ReleaseCom>;

template<class T> void release(T*& value) {
    if (value) { auto* old = value; value = nullptr; old->Release(); }
}

inline bool sameCom(IUnknown* a, IUnknown* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    // A provider can return distinct interface pointers for the same COM object.
    IUnknown *left = nullptr, *right = nullptr;
    const bool same = SUCCEEDED(a->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&left))) &&
                      SUCCEEDED(b->QueryInterface(IID_IUnknown, reinterpret_cast<void**>(&right))) && left && left == right;
    release(left);
    release(right);
    return same;
}

struct ImmContext {
    HWND window;
    HIMC handle;
    explicit ImmContext(HWND window) : window(window), handle(ImmGetContext(window)) {}
    ~ImmContext() { if (handle) ImmReleaseContext(window, handle); }
};

inline HWND gameWindow() {
    const HWND window = GetFocus();
    if (!window || window != GetForegroundWindow() || GetWindow(window, GW_OWNER) ||
        GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId() ||
        GetClassLongPtrW(window, GCW_ATOM) == 32770) return nullptr; // Standard dialog class.
    return window;
}
