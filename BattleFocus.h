#pragma once
#include "ImePlatform.h"
#include <utility>

// Only an observed empty focus before stock chat opened permits a later restore.
// Each attachment keeps document and context identities alive across native stop().
class BattleFocus {
    HWND baselineWindow = nullptr;
    struct Round {
        Com<ITfDocumentMgr> plugin, chat;
        Com<ITfContext> chatTop, pluginBase;
    } round;

    static Com<ITfThreadMgr> existingManager() {
        const auto module = GetModuleHandleW(L"msctf.dll");
        using GetManager = HRESULT (WINAPI *)(ITfThreadMgr**);
        const auto get = module ? reinterpret_cast<GetManager>(reinterpret_cast<void*>(
            GetProcAddress(module, "TF_GetThreadMgr"))) : nullptr;
        ITfThreadMgr* manager = nullptr;
        if (get) get(&manager);
        return Com<ITfThreadMgr>(manager);
    }

public:
    void beginChat() { round = {}; }
    void endBattle() { baselineWindow = nullptr; round = {}; }
    void windowDestroyed(HWND window) { if (window == baselineWindow) endBattle(); }

    bool observeBaseline(bool (*inputActive)()) {
        endBattle(); // Releasing old COM identities may re-enter the host.
        const HWND window = gameWindow();
        if (!window || inputActive() || ImmContext(window).handle) return false;
        const auto manager = existingManager();
        ITfDocumentMgr* current = nullptr;
        const bool empty = manager && SUCCEEDED(manager->GetFocus(&current)) && !current;
        release(current);
        if (empty) baselineWindow = window;
        return empty;
    }

    // Sample the provider's default top before the caller pushes its text context.
    Com<ITfContext> beforePush(HWND window, ITfDocumentMgr* document) const {
        ITfContext* base = nullptr;
        if (baselineWindow && baselineWindow == window) document->GetTop(&base);
        return Com<ITfContext>(base);
    }

    bool focusPlugin(HWND window, ITfThreadMgr* manager, ITfDocumentMgr* document,
                     ITfDocumentMgr* previous, Com<ITfContext> pluginBase) {
        ITfContext* top = nullptr;
        const bool identified = baselineWindow == window && !round.plugin && previous &&
                                SUCCEEDED(previous->GetTop(&top));
        Com<ITfContext> chatTop(top);
        if (FAILED(manager->SetFocus(document))) return false;
        if (baselineWindow && baselineWindow == window) {
            if (identified) {
                previous->AddRef();
                round.chat.reset(previous);
                round.chatTop = std::move(chatTop);
            }
            document->AddRef();
            round.plugin.reset(document);
            round.pluginBase = std::move(pluginBase);
        }
        return true;
    }

    const char* handoff(bool inputReleased) {
        const auto closed = std::move(round);
        if (!baselineWindow) return "no-baseline";
        if (!closed.plugin) return "no-attachment";
        if (!inputReleased) return "not-released";
        if (gameWindow() != baselineWindow) return "window-changed";
        if (ImmContext(baselineWindow).handle) return "host-himc";
        const auto manager = existingManager();
        ITfDocumentMgr* current = nullptr;
        const HRESULT focus = manager ? manager->GetFocus(&current) : E_FAIL;
        const Com<ITfDocumentMgr> focused(current);
        if (FAILED(focus)) return "focus-unavailable";
        if (!current) return "already-clear";
        const bool ours = sameCom(current, closed.plugin.get());
        if (!ours && !sameCom(current, closed.chat.get())) return "foreign";
        ITfContext* top = nullptr;
        const HRESULT result = current->GetTop(&top);
        const Com<ITfContext> context(top);
        if (FAILED(result) || !sameCom(top, ours ? closed.pluginBase.get() : closed.chatTop.get())) return "foreign-context";
        if (FAILED(manager->SetFocus(nullptr))) return "restore-failed";
        ITfDocumentMgr* after = nullptr;
        const HRESULT readBack = manager->GetFocus(&after);
        const Com<ITfDocumentMgr> observed(after);
        if (FAILED(readBack)) return "restore-unconfirmed";
        if (after) return "focus-replaced";
        return ours ? "restored-plugin" : "restored-chat";
    }
};
