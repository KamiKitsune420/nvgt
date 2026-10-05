// Runtime core: the script-context pool, the AngelScript ref-counting
// table for wx objects, the event-handler map and the trampoline that
// dispatches wxWidgets events into script callbacks.
//
// Anything in this file is concerned with the lifecycle of bridge state
// itself, not with any specific wx widget.

#include "common.h"

// ---------------------------------------------------------------------------
// Script context pool.
// ---------------------------------------------------------------------------
std::vector<asIScriptContext*> g_context_pool;

asIScriptContext* GetContextFromPool(asIScriptEngine* engine) {
    if (!g_context_pool.empty()) {
        asIScriptContext* ctx = g_context_pool.back();
        g_context_pool.pop_back();
        return ctx;
    }
    return engine->CreateContext();
}

void ReturnContextToPool(asIScriptContext* ctx) {
    if (ctx) {
        ctx->Unprepare();
        g_context_pool.push_back(ctx);
    }
}

// ---------------------------------------------------------------------------
// Reference counting + handler bookkeeping.
// ---------------------------------------------------------------------------
std::map<void*, int> g_ref_counts;
std::map<EventKey, ScriptEventData> g_event_handlers;

void CleanupHandlersFor(void* ptr) {
    for (auto it = g_event_handlers.begin(); it != g_event_handlers.end(); ) {
        if (it->first.first == ptr) {
            if (it->second.callback) {
                it->second.callback->Release();
            }
            it = g_event_handlers.erase(it);
        } else {
            ++it;
        }
    }
}

// Clean up bridge entries for a wxSizer and every wxSizerItem it owns,
// recursing into sub-sizers. The wxWidgets destruction chain runs
// (wxWindow::~wxWindow) -> (wxSizer::~wxSizer) -> (wxSizerItem::~wxSizerItem),
// but only the wxWindow step fires wxEVT_DESTROY. Without this helper,
// any AddRef'd sizer or sizer-item entry would be left behind after the
// underlying object is freed, and a later Release() (or even a stray
// asMETHOD call through a dangling AS handle) would dereference garbage.
void CleanupSizerEntries(wxSizer* sizer) {
    if (!sizer) return;
    g_ref_counts.erase(sizer);
    CleanupHandlersFor(sizer);
    wxSizerItemList& items = sizer->GetChildren();
    for (auto it = items.begin(); it != items.end(); ++it) {
        wxSizerItem* item = *it;
        if (!item) continue;
        g_ref_counts.erase(item);
        CleanupHandlersFor(item);
        if (wxSizer* sub = item->GetSizer()) {
            CleanupSizerEntries(sub);
        }
    }
}

void AddRef(void* ptr) {
    if (!ptr) return;
    if (g_ref_counts.find(ptr) == g_ref_counts.end()) {
        g_ref_counts[ptr] = 1;
    } else {
        asAtomicInc(g_ref_counts[ptr]);
    }
}

void Release(void* ptr) {
    if (!ptr) return;
    auto it = g_ref_counts.find(ptr);
    if (it == g_ref_counts.end()) return;
    if (asAtomicDec(it->second) <= 0) {
        g_ref_counts.erase(it);
        CleanupHandlersFor(ptr);
        wxObject* wx_obj = static_cast<wxObject*>(ptr);
        if (wx_obj->IsKindOf(CLASSINFO(wxWindow))) {
            wxWindow* win = static_cast<wxWindow*>(wx_obj);
            if (win && !win->GetParent()) {
                win->Destroy();
            }
        }
        else if (wx_obj->IsKindOf(CLASSINFO(wxSizer))) {
            wxSizer* sizer = static_cast<wxSizer*>(wx_obj);
            if (sizer && !sizer->GetContainingWindow()) {
                // Remove every wxSizerItem entry the bridge holds for
                // this sizer (and any sub-sizers) before the wxSizer
                // destructor invalidates them.
                CleanupSizerEntries(sizer);
                delete sizer;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Event dispatch.
// ---------------------------------------------------------------------------
void OnObjectDestroyed(wxWindowDestroyEvent& event) {
    void* ptr = event.GetEventObject();
    CleanupHandlersFor(ptr);
    g_ref_counts.erase(ptr);
    // wxWindow may carry a sizer (via SetSizer) that wxWidgets is about
    // to destroy as part of this window's destructor chain. Sizers and
    // sizer items do not fire wxEVT_DESTROY, so walk the sizer here
    // while it is still valid and clean up any g_ref_counts entries
    // we hold for it. wxDynamicCast handles the IsKindOf check and
    // the cast in one step.
    if (auto* obj = static_cast<wxObject*>(ptr)) {
        if (auto* win = wxDynamicCast(obj, wxWindow)) {
            if (wxSizer* sizer = win->GetSizer()) {
                CleanupSizerEntries(sizer);
            }
        }
    }
}

void OnWxEvent(wxEvent& event) {
    auto it = g_event_handlers.find({event.GetEventObject(), event.GetEventType()});
    if (it == g_event_handlers.end()) return;
    // Skip by default so wxWidgets can keep propagating the event after the
    // script callback returns. The script can override this by calling
    // e.skip(false) inside the callback.
    event.Skip(true);
    ContextGuard guard(it->second.engine);
    if (auto ctx = guard.get()) {
        ctx->Prepare(it->second.callback);
        ctx->SetArgObject(0, &event);
        ctx->Execute();
    }
}

void wx_window_bind(wxWindow* self, int event_type, asIScriptFunction* callback) {
    if (!self || !callback) return;
    EventKey key = { (void*)self, event_type };
    auto it = g_event_handlers.find(key);
    if (it != g_event_handlers.end()) {
        // Replace the script callback in place; the wxWidgets-side trampoline
        // is already bound for this (object, event_type), so don't bind again.
        if (it->second.callback) it->second.callback->Release();
        callback->AddRef();
        it->second = { callback, callback->GetEngine() };
        return;
    }
    callback->AddRef();
    g_event_handlers[key] = { callback, callback->GetEngine() };
    self->Bind(static_cast<wxEventType>(event_type), &OnWxEvent, wxID_ANY);
}

void wx_window_unbind(wxWindow* self, int event_type) {
    if (!self) return;
    EventKey key = { (void*)self, event_type };
    auto it = g_event_handlers.find(key);
    if (it == g_event_handlers.end()) return;
    if (it->second.callback) it->second.callback->Release();
    self->Unbind(static_cast<wxEventType>(event_type), &OnWxEvent, wxID_ANY);
    g_event_handlers.erase(it);
}
