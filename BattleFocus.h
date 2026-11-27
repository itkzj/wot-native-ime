#pragma once
#include "ImePlatform.h"
#include <utility>

// Only an observed empty focus before stock chat opened permits a later restore.
// Each attachment keeps document and context identities alive across native stop().
class BattleFocus {
    HWND baselineWindow = nullptr;
    unsigned chatGeneration = 0;
    enum class Retry { None, Chat, Plugin };
    struct Round {
        Com<ITfDocumentMgr> plugin, chat;
        Com<ITfContext> chatTop, pluginBase;
        Retry retry = Retry::None;
    } round;

    static bool sameTop(ITfDocumentMgr* document, ITfContext* expected) {
        ITfContext* top = nullptr;
        const HRESULT result = document->GetTop(&top);
        const Com<ITfContext> context(top);
        return SUCCEEDED(result) && sameCom(top, expected);
    }

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
    void beginChat() { ++chatGeneration; round = {}; }
    void endBattle() { baselineWindow = nullptr; beginChat(); }
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
        auto closed = std::exchange(round, Round{});
        const auto generation = chatGeneration;
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
        const Retry target = ours ? Retry::Plugin : Retry::Chat;
        if (closed.retry != Retry::None && closed.retry != target) return "foreign";
        auto* expectedTop = ours ? closed.pluginBase.get() : closed.chatTop.get();
        if (!sameTop(current, expectedTop)) return "foreign-context";
        if (generation != chatGeneration) return "round-changed";
        if (FAILED(manager->SetFocus(nullptr))) return "restore-failed";
#ifdef TEST_AFTER_CLEAR
        TEST_AFTER_CLEAR();
#endif
        ITfDocumentMgr* after = nullptr;
        const HRESULT readBack = manager->GetFocus(&after);
        const Com<ITfDocumentMgr> observed(after);
        if (FAILED(readBack)) return "restore-unconfirmed";
        if (generation != chatGeneration) return "round-changed";
        if (!after) return ours ? "restored-plugin" : "restored-chat";
        if (!sameCom(current, after)) return "replaced-foreign";
        if (!sameTop(after, expectedTop)) return "replaced-context";
        if (generation != chatGeneration) return "round-changed";
        if (closed.retry != Retry::None) return ours ? "plugin-reclaimed" : "chat-reclaimed";
        closed.retry = target;
        round = std::move(closed);
        return "retry-chat";
    }
};
