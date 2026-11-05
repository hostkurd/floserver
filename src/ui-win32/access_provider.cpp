#include "access_provider.h"

#include "win_util.h"

#include <ole2.h>
#include <oleauto.h>
#include <oleacc.h>
#include <uiautomation.h>

#include <atomic>
#include <cstdint>
#include <map>
#include <utility>

// Newer than some SDKs' headers (Windows 10 1703): the documented values.
#ifndef UIA_PositionInSetPropertyId
#define UIA_PositionInSetPropertyId 30152
#endif
#ifndef UIA_SizeOfSetPropertyId
#define UIA_SizeOfSetPropertyId 30153
#endif

namespace floserver::ui {

namespace {

constexpr LONG kUiaRootObjectId = -25;
constexpr int kAppendRuntimeId = 3;  // UiaAppendRuntimeId
constexpr int kHwndRuntimeIdBase = 42;  // UiaHwndRuntimeIdBase: an HWND element's id is {42, hwnd}
constexpr HRESULT kNotAvailable = static_cast<HRESULT>(0x80040201UL);     // UIA_E_ELEMENTNOTAVAILABLE
constexpr HRESULT kNotEnabled = static_cast<HRESULT>(0x80040200UL);       // UIA_E_ELEMENTNOTENABLED
constexpr HRESULT kInvalidOperation = static_cast<HRESULT>(0x80131509UL);  // UIA_E_INVALIDOPERATION
// NotificationKind and NotificationProcessing (Windows 10 1709).
constexpr int kNotificationActionCompleted = 2;
constexpr int kNotificationOther = 4;
constexpr int kProcessingImportantMostRecent = 1;
constexpr int kProcessingMostRecent = 3;
constexpr auto kOptions = static_cast<ProviderOptions>(static_cast<int>(ProviderOptions_ServerSideProvider) |
                                                     static_cast<int>(ProviderOptions_UseComThreading));

// UIAutomationCore.dll's functions, looked up once: MinGW has no import
// library for it, and UiaRaiseNotificationEvent is only in Windows 10 1709
// and later. Each is null when it isn't there.
struct UiaApi {
    using ReturnFn = LRESULT(WINAPI*)(HWND, WPARAM, LPARAM, IRawElementProviderSimple*);
    using HostFn = HRESULT(WINAPI*)(HWND, IRawElementProviderSimple**);
    using EventFn = HRESULT(WINAPI*)(IRawElementProviderSimple*, int);
    using PropertyFn = HRESULT(WINAPI*)(IRawElementProviderSimple*, int, VARIANT, VARIANT);
    using StructureFn = HRESULT(WINAPI*)(IRawElementProviderSimple*, StructureChangeType, int*, int);
    using NotificationFn = HRESULT(WINAPI*)(IRawElementProviderSimple*, int, int, BSTR, BSTR);
    using ListeningFn = BOOL(WINAPI*)();
    using DisconnectFn = HRESULT(WINAPI*)(IRawElementProviderSimple*);
    ReturnFn return_provider = nullptr;
    HostFn host_from_hwnd = nullptr;
    EventFn raise_event = nullptr;
    PropertyFn raise_property = nullptr;
    StructureFn raise_structure = nullptr;
    NotificationFn raise_notification = nullptr;
    ListeningFn listening = nullptr;
    DisconnectFn disconnect = nullptr;
};

template <typename F>
F find_proc(HMODULE module, const char* name) {
    // Through void(*)(): the one function type any other may be cast from.
    return reinterpret_cast<F>(reinterpret_cast<void (*)()>(GetProcAddress(module, name)));
}

const UiaApi& uia() {
    static const UiaApi api = [] {
        UiaApi a;
        HMODULE module = LoadLibraryExW(L"UIAutomationCore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module == nullptr) {
            return a;
        }
        a.return_provider = find_proc<UiaApi::ReturnFn>(module, "UiaReturnRawElementProvider");
        a.host_from_hwnd = find_proc<UiaApi::HostFn>(module, "UiaHostProviderFromHwnd");
        a.raise_event = find_proc<UiaApi::EventFn>(module, "UiaRaiseAutomationEvent");
        a.raise_property = find_proc<UiaApi::PropertyFn>(module, "UiaRaiseAutomationPropertyChangedEvent");
        a.raise_structure = find_proc<UiaApi::StructureFn>(module, "UiaRaiseStructureChangedEvent");
        a.raise_notification = find_proc<UiaApi::NotificationFn>(module, "UiaRaiseNotificationEvent");
        a.listening = find_proc<UiaApi::ListeningFn>(module, "UiaClientsAreListening");
        a.disconnect = find_proc<UiaApi::DisconnectFn>(module, "UiaDisconnectProvider");
        return a;
    }();
    return api;
}

BSTR bstr(std::string_view s) { return SysAllocString(win::widen(s).c_str()); }

void set_bstr(VARIANT* v, std::string_view s) {
    if (s.empty()) {
        return;
    }
    v->vt = VT_BSTR;
    v->bstrVal = bstr(s);
}

void set_bool(VARIANT* v, bool b) {
    v->vt = VT_BOOL;
    v->boolVal = static_cast<VARIANT_BOOL>(b ? -1 : 0);
}

void set_int(VARIANT* v, int i) {
    v->vt = VT_I4;
    v->lVal = i;
}

int control_type(AccessRole role) {
    switch (role) {
        case AccessRole::Pane:
            return UIA_PaneControlTypeId;
        case AccessRole::Group:
            return UIA_GroupControlTypeId;
        case AccessRole::Text:
            return UIA_TextControlTypeId;
        case AccessRole::Button:
        case AccessRole::Switch:
            return UIA_ButtonControlTypeId;
        case AccessRole::Link:
            return UIA_HyperlinkControlTypeId;
        case AccessRole::List:
            return UIA_ListControlTypeId;
        case AccessRole::ListItem:
            return UIA_ListItemControlTypeId;
        case AccessRole::TabList:
            return UIA_TabControlTypeId;
        case AccessRole::TabItem:
            return UIA_TabItemControlTypeId;
        case AccessRole::ComboBox:
            return UIA_ComboBoxControlTypeId;
        case AccessRole::Edit:
            return UIA_EditControlTypeId;
    }
    return UIA_GroupControlTypeId;
}

bool selects_items(const AccessTree& tree, const AccessNode& n) {
    for (int c = n.first_child; c >= 0; c = tree.node(c)->next) {
        if (tree.node(c)->selected.has_value()) {
            return true;
        }
    }
    return false;
}

// The rectangle on the screen: MapWindowPoints mirrors it in a right-to-left
// window (two points are taken as a RECT and its sides swapped).
RECT to_screen(HWND hwnd, const AccessRect& r) {
    RECT out{r.left, r.top, r.right, r.bottom};
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&out), 2);
    if (out.left > out.right) {
        std::swap(out.left, out.right);
    }
    return out;
}

}  // namespace

// What every element shares: the window and what describes it, until it's detached.
struct AccessBridge {
    HWND hwnd = nullptr;
    AccessSource* source = nullptr;
    DWORD thread = 0;
    AccessRootProvider* root = nullptr;  // while attached; AccessRoot holds a reference
    std::map<std::string, int, std::less<>> runtime_ids;
    int next_runtime_id = 1;

    // Only on the window's thread (UI Automation calls arrive there through
    // COM), and never once the window is gone.
    [[nodiscard]] bool live() const {
        return GetCurrentThreadId() == thread && source != nullptr && hwnd != nullptr;
    }
    [[nodiscard]] const AccessTree* tree() const { return live() ? &source->access_tree() : nullptr; }
    int runtime_id(const std::string& id) {
        const auto [it, added] = runtime_ids.try_emplace(id, next_runtime_id);
        if (added) {
            ++next_runtime_id;
        }
        return it->second;
    }
    // The ids of elements gone from `tree` are forgotten once there are many
    // of them (one that comes back gets a new number, as a new element would).
    void prune(const AccessTree& tree) {
        if (runtime_ids.size() <= tree.nodes().size() + 256) {
            return;
        }
        std::erase_if(runtime_ids, [&](const auto& entry) { return tree.find(entry.first) < 0; });
    }
};

// COM interfaces have no virtual destructor (Release is the way out); GCC warns all the same.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
class AccessRootProvider final : public IRawElementProviderSimple,
                                 public IRawElementProviderFragment,
                                 public IRawElementProviderFragmentRoot {
public:
    explicit AccessRootProvider(std::shared_ptr<AccessBridge> bridge) : bridge_(std::move(bridge)) {}
    AccessRootProvider(const AccessRootProvider&) = delete;
    AccessRootProvider& operator=(const AccessRootProvider&) = delete;
    AccessRootProvider(AccessRootProvider&&) = delete;
    AccessRootProvider& operator=(AccessRootProvider&&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) {
            *out = static_cast<IRawElementProviderSimple*>(this);
        } else if (riid == __uuidof(IRawElementProviderFragment)) {
            *out = static_cast<IRawElementProviderFragment*>(this);
        } else if (riid == __uuidof(IRawElementProviderFragmentRoot)) {
            *out = static_cast<IRawElementProviderFragmentRoot*>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) {
            delete this;  // NOLINT(cppcoreguidelines-owning-memory): COM's own reference count
        }
        return left;
    }

    // IRawElementProviderSimple: the window's own provider says the rest (its title, its rectangle).
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = kOptions;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID /*pattern*/, IUnknown** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID /*property*/, VARIANT* out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        out->vt = VT_EMPTY;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        if (!bridge_->live() || uia().host_from_hwnd == nullptr) {
            return kNotAvailable;
        }
        return uia().host_from_hwnd(bridge_->hwnd, out);
    }

    // IRawElementProviderFragment
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** out) override;
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;  // the window's own
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = UiaRect{};  // the window's own
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override {
        if (!bridge_->live()) {
            return kNotAvailable;
        }
        ::SetFocus(bridge_->hwnd);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = this;
        AddRef();
        return S_OK;
    }

    // IRawElementProviderFragmentRoot
    HRESULT STDMETHODCALLTYPE ElementProviderFromPoint(double x, double y, IRawElementProviderFragment** out) override;
    HRESULT STDMETHODCALLTYPE GetFocus(IRawElementProviderFragment** out) override;

private:
    ~AccessRootProvider() = default;  // Release() deletes it

    std::shared_ptr<AccessBridge> bridge_;
    std::atomic<ULONG> refs_{1};
};

namespace {

class AccessElement final : public IRawElementProviderSimple,
                            public IRawElementProviderFragment,
                            public IInvokeProvider,
                            public IToggleProvider,
                            public ISelectionItemProvider,
                            public ISelectionProvider,
                            public IValueProvider,
                            public IExpandCollapseProvider {
public:
    AccessElement(std::shared_ptr<AccessBridge> bridge, std::string id) : bridge_(std::move(bridge)), id_(std::move(id)) {}
    AccessElement(const AccessElement&) = delete;
    AccessElement& operator=(const AccessElement&) = delete;
    AccessElement(AccessElement&&) = delete;
    AccessElement& operator=(AccessElement&&) = delete;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IRawElementProviderSimple)) {
            *out = static_cast<IRawElementProviderSimple*>(this);
        } else if (riid == __uuidof(IRawElementProviderFragment)) {
            *out = static_cast<IRawElementProviderFragment*>(this);
        } else if (riid == __uuidof(IInvokeProvider)) {
            *out = static_cast<IInvokeProvider*>(this);
        } else if (riid == __uuidof(IToggleProvider)) {
            *out = static_cast<IToggleProvider*>(this);
        } else if (riid == __uuidof(ISelectionItemProvider)) {
            *out = static_cast<ISelectionItemProvider*>(this);
        } else if (riid == __uuidof(ISelectionProvider)) {
            *out = static_cast<ISelectionProvider*>(this);
        } else if (riid == __uuidof(IValueProvider)) {
            *out = static_cast<IValueProvider*>(this);
        } else if (riid == __uuidof(IExpandCollapseProvider)) {
            *out = static_cast<IExpandCollapseProvider*>(this);
        } else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (left == 0) {
            delete this;  // NOLINT(cppcoreguidelines-owning-memory): COM's own reference count
        }
        return left;
    }

    // IRawElementProviderSimple
    HRESULT STDMETHODCALLTYPE get_ProviderOptions(ProviderOptions* out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = kOptions;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPatternProvider(PATTERNID pattern, IUnknown** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        const AccessTree* tree = nullptr;
        const AccessNode* n = node(tree);
        if (n == nullptr) {
            return kNotAvailable;
        }
        bool has = false;
        switch (pattern) {
            case UIA_InvokePatternId:
                has = n->invokable;
                break;
            case UIA_TogglePatternId:
                has = n->toggled.has_value();
                break;
            case UIA_SelectionItemPatternId:
                has = n->selected.has_value();
                break;
            case UIA_SelectionPatternId:
                has = selects_items(*tree, *n);
                break;
            case UIA_ValuePatternId:
                has = n->role == AccessRole::ComboBox;
                break;
            case UIA_ExpandCollapsePatternId:
                has = n->expandable;
                break;
            default:
                break;
        }
        if (has) {
            *out = static_cast<IRawElementProviderSimple*>(this);
            AddRef();
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPropertyValue(PROPERTYID property, VARIANT* out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        out->vt = VT_EMPTY;
        const AccessTree* tree = nullptr;
        const AccessNode* n = node(tree);
        if (n == nullptr) {
            return kNotAvailable;
        }
        switch (property) {
            case UIA_ControlTypePropertyId:
                set_int(out, control_type(n->role));
                break;
            case UIA_LocalizedControlTypePropertyId:
                set_bstr(out, n->kind);
                break;
            case UIA_NamePropertyId:
                set_bstr(out, n->name);
                break;
            case UIA_HelpTextPropertyId:
                set_bstr(out, n->help);
                break;
            case UIA_AcceleratorKeyPropertyId:
                set_bstr(out, n->keys);
                break;
            case UIA_AutomationIdPropertyId:
                set_bstr(out, n->id);
                break;
            case UIA_IsEnabledPropertyId:
                set_bool(out, n->enabled);
                break;
            case UIA_IsKeyboardFocusablePropertyId:
                set_bool(out, n->focusable);
                break;
            case UIA_HasKeyboardFocusPropertyId:
                set_bool(out, tree->focus == n->id && ::GetFocus() == bridge_->hwnd);
                break;
            case UIA_IsOffscreenPropertyId:
                set_bool(out, n->offscreen);
                break;
            case UIA_PositionInSetPropertyId:
                if (n->position > 0) {
                    set_int(out, n->position);
                }
                break;
            case UIA_SizeOfSetPropertyId:
                if (n->set_size > 0) {
                    set_int(out, n->set_size);
                }
                break;
            case UIA_ProviderDescriptionPropertyId:
                set_bstr(out, "FloServer");
                break;
            default:
                break;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_HostRawElementProvider(IRawElementProviderSimple** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        return S_OK;
    }

    // IRawElementProviderFragment
    HRESULT STDMETHODCALLTYPE Navigate(NavigateDirection direction, IRawElementProviderFragment** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        const AccessTree* tree = nullptr;
        const AccessNode* n = node(tree);
        if (n == nullptr) {
            return kNotAvailable;
        }
        int to = -1;
        switch (direction) {
            case NavigateDirection_Parent:
                if (n->parent < 0) {
                    return root_fragment(out);
                }
                to = n->parent;
                break;
            case NavigateDirection_NextSibling:
                to = n->next;
                break;
            case NavigateDirection_PreviousSibling:
                to = n->prev;
                break;
            case NavigateDirection_FirstChild:
                to = n->first_child;
                break;
            case NavigateDirection_LastChild:
                to = n->last_child;
                break;
        }
        if (const AccessNode* other = tree->node(to); other != nullptr) {
            *out = new AccessElement(bridge_, other->id);  // NOLINT(cppcoreguidelines-owning-memory): COM reference
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeId(SAFEARRAY** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        const AccessTree* tree = nullptr;
        if (node(tree) == nullptr) {
            return kNotAvailable;
        }
        SAFEARRAY* ids = SafeArrayCreateVector(VT_I4, 0, 2);
        if (ids == nullptr) {
            return E_OUTOFMEMORY;
        }
        LONG at = 0;
        int append = kAppendRuntimeId;
        int mine = bridge_->runtime_id(id_);
        SafeArrayPutElement(ids, &at, &append);
        at = 1;
        SafeArrayPutElement(ids, &at, &mine);
        *out = ids;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_BoundingRectangle(UiaRect* out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = UiaRect{};
        const AccessTree* tree = nullptr;
        const AccessNode* n = node(tree);
        if (n == nullptr) {
            return kNotAvailable;
        }
        if (!n->bounds.empty()) {
            const RECT r = to_screen(bridge_->hwnd, n->bounds);
            out->left = r.left;
            out->top = r.top;
            out->width = r.right - r.left;
            out->height = r.bottom - r.top;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetEmbeddedFragmentRoots(SAFEARRAY** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetFocus() override {
        const AccessTree* tree = nullptr;
        const AccessNode* n = node(tree);
        if (n == nullptr) {
            return kNotAvailable;
        }
        if (n->focusable) {
            bridge_->source->access_request(id_, AccessAction::Focus);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_FragmentRoot(IRawElementProviderFragmentRoot** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        if (!bridge_->live() || bridge_->root == nullptr) {
            return kNotAvailable;
        }
        return bridge_->root->QueryInterface(__uuidof(IRawElementProviderFragmentRoot), reinterpret_cast<void**>(out));
    }

    // IInvokeProvider
    HRESULT STDMETHODCALLTYPE Invoke() override { return act(AccessAction::Invoke); }

    // IToggleProvider
    HRESULT STDMETHODCALLTYPE Toggle() override { return act(AccessAction::Toggle); }
    HRESULT STDMETHODCALLTYPE get_ToggleState(ToggleState* out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        const AccessTree* tree = nullptr;
        const AccessNode* n = node(tree);
        if (n == nullptr) {
            return kNotAvailable;
        }
        *out = n->toggled.value_or(false) ? ToggleState_On : ToggleState_Off;
        return S_OK;
    }

    // ISelectionItemProvider
    HRESULT STDMETHODCALLTYPE Select() override { return act(AccessAction::Select); }
    HRESULT STDMETHODCALLTYPE AddToSelection() override { return act(AccessAction::Select); }
    HRESULT STDMETHODCALLTYPE RemoveFromSelection() override { return kInvalidOperation; }
    HRESULT STDMETHODCALLTYPE get_IsSelected(BOOL* out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        const AccessTree* tree = nullptr;
        const AccessNode* n = node(tree);
        if (n == nullptr) {
            return kNotAvailable;
        }
        *out = n->selected.value_or(false) ? TRUE : FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_SelectionContainer(IRawElementProviderSimple** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        const AccessTree* tree = nullptr;
        const AccessNode* n = node(tree);
        if (n == nullptr) {
            return kNotAvailable;
        }
        if (const AccessNode* parent = tree->node(n->parent); parent != nullptr) {
            *out = new AccessElement(bridge_, parent->id);  // NOLINT(cppcoreguidelines-owning-memory): COM reference
        }
        return S_OK;
    }

    // ISelectionProvider: the list's or the tab bar's selected item.
    HRESULT STDMETHODCALLTYPE GetSelection(SAFEARRAY** out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        const AccessTree* tree = nullptr;
        const AccessNode* n = node(tree);
        if (n == nullptr) {
            return kNotAvailable;
        }
        std::vector<std::string> chosen;
        for (int c = n->first_child; c >= 0; c = tree->node(c)->next) {
            if (tree->node(c)->selected.value_or(false)) {
                chosen.push_back(tree->node(c)->id);
            }
        }
        SAFEARRAY* items = SafeArrayCreateVector(VT_UNKNOWN, 0, static_cast<ULONG>(chosen.size()));
        if (items == nullptr) {
            return E_OUTOFMEMORY;
        }
        for (std::size_t i = 0; i < chosen.size(); ++i) {
            auto* item = new AccessElement(bridge_, chosen[i]);  // NOLINT(cppcoreguidelines-owning-memory): COM reference
            LONG at = static_cast<LONG>(i);
            SafeArrayPutElement(items, &at, static_cast<IUnknown*>(static_cast<IRawElementProviderSimple*>(item)));
            item->Release();  // the array holds its own
        }
        *out = items;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_CanSelectMultiple(BOOL* out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_IsSelectionRequired(BOOL* out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        const AccessTree* tree = nullptr;
        const AccessNode* n = node(tree);
        if (n == nullptr) {
            return kNotAvailable;
        }
        *out = n->selection_required ? TRUE : FALSE;
        return S_OK;
    }

    // IValueProvider: a picker's choice, changed through its menu.
    HRESULT STDMETHODCALLTYPE SetValue(LPCWSTR /*value*/) override { return kInvalidOperation; }
    HRESULT STDMETHODCALLTYPE get_Value(BSTR* out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = nullptr;
        const AccessTree* tree = nullptr;
        const AccessNode* n = node(tree);
        if (n == nullptr) {
            return kNotAvailable;
        }
        *out = bstr(n->value);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE get_IsReadOnly(BOOL* out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = TRUE;
        return S_OK;
    }

    // IExpandCollapseProvider: the menu is Windows' own and runs until it's closed.
    HRESULT STDMETHODCALLTYPE Expand() override { return act(AccessAction::Expand); }
    HRESULT STDMETHODCALLTYPE Collapse() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE get_ExpandCollapseState(ExpandCollapseState* out) override {
        if (out == nullptr) {
            return E_POINTER;
        }
        *out = ExpandCollapseState_Collapsed;
        return S_OK;
    }

private:
    ~AccessElement() = default;  // Release() deletes it

    // The element as the window is now; null when it's gone.
    const AccessNode* node(const AccessTree*& tree) const {
        tree = bridge_->tree();
        return tree != nullptr ? tree->node(tree->find(id_)) : nullptr;
    }
    HRESULT act(AccessAction action) {
        const AccessTree* tree = nullptr;
        const AccessNode* n = node(tree);
        if (n == nullptr) {
            return kNotAvailable;
        }
        if (!n->enabled) {
            return kNotEnabled;
        }
        bridge_->source->access_request(id_, action);
        return S_OK;
    }
    HRESULT root_fragment(IRawElementProviderFragment** out) const {
        if (bridge_->root == nullptr) {
            return kNotAvailable;
        }
        return bridge_->root->QueryInterface(__uuidof(IRawElementProviderFragment), reinterpret_cast<void**>(out));
    }

    std::shared_ptr<AccessBridge> bridge_;
    std::string id_;
    std::atomic<ULONG> refs_{1};
};

}  // namespace
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

HRESULT STDMETHODCALLTYPE AccessRootProvider::Navigate(NavigateDirection direction, IRawElementProviderFragment** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    const AccessTree* tree = bridge_->tree();
    if (tree == nullptr) {
        return kNotAvailable;
    }
    int to = -1;
    if (direction == NavigateDirection_FirstChild) {
        to = tree->first_top();
    } else if (direction == NavigateDirection_LastChild) {
        to = tree->last_top();
    }
    if (const AccessNode* n = tree->node(to); n != nullptr) {
        *out = new AccessElement(bridge_, n->id);  // NOLINT(cppcoreguidelines-owning-memory): COM reference
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessRootProvider::ElementProviderFromPoint(double x, double y,
                                                                       IRawElementProviderFragment** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    const AccessTree* tree = bridge_->tree();
    if (tree == nullptr) {
        return kNotAvailable;
    }
    // The window's client coordinates, logical: mirrored in a right-to-left window.
    POINT at{static_cast<LONG>(x), static_cast<LONG>(y)};
    ScreenToClient(bridge_->hwnd, &at);
    if (const AccessNode* n = tree->node(tree->at(at.x, at.y)); n != nullptr) {
        *out = new AccessElement(bridge_, n->id);  // NOLINT(cppcoreguidelines-owning-memory): COM reference
    }
    return S_OK;
}

HRESULT STDMETHODCALLTYPE AccessRootProvider::GetFocus(IRawElementProviderFragment** out) {
    if (out == nullptr) {
        return E_POINTER;
    }
    *out = nullptr;
    const AccessTree* tree = bridge_->tree();
    if (tree == nullptr) {
        return kNotAvailable;
    }
    // Only while the keys are in the window itself (not its search box).
    if (::GetFocus() == bridge_->hwnd && tree->find(tree->focus) >= 0) {
        *out = new AccessElement(bridge_, tree->focus);  // NOLINT(cppcoreguidelines-owning-memory): COM reference
    }
    return S_OK;
}

// --- AccessRoot ---------------------------------------------------------------------

AccessRoot::~AccessRoot() { detach(); }

void AccessRoot::attach(HWND hwnd, AccessSource* source) {
    detach();
    bridge_ = std::make_shared<AccessBridge>();
    bridge_->hwnd = hwnd;
    bridge_->source = source;
    bridge_->thread = GetCurrentThreadId();
}

void AccessRoot::detach() {
    if (!bridge_) {
        return;
    }
    const HWND hwnd = bridge_->hwnd;
    // Nothing reaches the window from here on: elements a client still holds answer "not available".
    bridge_->source = nullptr;
    bridge_->hwnd = nullptr;
    bridge_->root = nullptr;
    if (root_ != nullptr) {
        if (uia().disconnect != nullptr) {
            uia().disconnect(root_);
        }
        if (uia().return_provider != nullptr && hwnd != nullptr) {
            uia().return_provider(hwnd, 0, 0, nullptr);
        }
        root_->Release();
        root_ = nullptr;
    }
    bridge_.reset();
}

bool AccessRoot::on_get_object(WPARAM wparam, LPARAM lparam, LRESULT& result) {
    if (static_cast<LONG>(lparam) != kUiaRootObjectId || !bridge_ || !bridge_->live() ||
        uia().return_provider == nullptr) {
        return false;
    }
    if (root_ == nullptr) {
        root_ = new AccessRootProvider(bridge_);  // NOLINT(cppcoreguidelines-owning-memory): COM reference, released in detach()
        bridge_->root = root_;
    }
    result = uia().return_provider(bridge_->hwnd, wparam, lparam, root_);
    return true;
}

bool AccessRoot::listening() { return uia().listening != nullptr && uia().listening() != FALSE; }

void AccessRoot::raise(const std::vector<AccessChange>& changes) {
    if (!bridge_ || !bridge_->live() || root_ == nullptr) {
        return;
    }
    const UiaApi& api = uia();
    const AccessTree& tree = bridge_->source->access_tree();
    bridge_->prune(tree);
    for (const AccessChange& change : changes) {
        if (change.id.empty()) {
            if (change.kind == AccessChangeKind::Children && api.raise_structure != nullptr) {
                // The window's own element: its id is the HWND's (the root fragment answers none).
                int ids[2] = {kHwndRuntimeIdBase,
                              static_cast<int>(reinterpret_cast<std::intptr_t>(bridge_->hwnd))};
                api.raise_structure(root_, StructureChangeType_ChildrenInvalidated, ids, 2);
            }
            continue;  // the focus on the window itself is Windows' to say
        }
        const AccessNode* n = tree.node(tree.find(change.id));
        if (n == nullptr) {
            continue;
        }
        auto* element = new AccessElement(bridge_, change.id);  // NOLINT(cppcoreguidelines-owning-memory): COM reference
        auto* simple = static_cast<IRawElementProviderSimple*>(element);
        VARIANT before{};
        before.vt = VT_EMPTY;
        VARIANT after{};
        after.vt = VT_EMPTY;
        switch (change.kind) {
            case AccessChangeKind::Focus:
                if (api.raise_event != nullptr) {
                    api.raise_event(simple, UIA_AutomationFocusChangedEventId);
                }
                break;
            case AccessChangeKind::Name:
                set_bstr(&after, n->name);
                if (api.raise_property != nullptr) {
                    api.raise_property(simple, UIA_NamePropertyId, before, after);
                }
                break;
            case AccessChangeKind::Toggle:
                set_int(&after, n->toggled.value_or(false) ? ToggleState_On : ToggleState_Off);
                if (api.raise_property != nullptr) {
                    api.raise_property(simple, UIA_ToggleToggleStatePropertyId, before, after);
                }
                break;
            case AccessChangeKind::Selected:
                set_bool(&after, n->selected.value_or(false));
                if (api.raise_property != nullptr) {
                    api.raise_property(simple, UIA_SelectionItemIsSelectedPropertyId, before, after);
                }
                if (n->selected.value_or(false) && api.raise_event != nullptr) {
                    api.raise_event(simple, UIA_SelectionItem_ElementSelectedEventId);
                }
                break;
            case AccessChangeKind::Value:
                set_bstr(&after, n->value);
                if (api.raise_property != nullptr) {
                    api.raise_property(simple, UIA_ValueValuePropertyId, before, after);
                }
                break;
            case AccessChangeKind::Enabled:
                set_bool(&after, n->enabled);
                if (api.raise_property != nullptr) {
                    api.raise_property(simple, UIA_IsEnabledPropertyId, before, after);
                }
                break;
            case AccessChangeKind::Children:
                if (api.raise_structure != nullptr) {
                    int ids[2] = {kAppendRuntimeId, bridge_->runtime_id(change.id)};
                    api.raise_structure(simple, StructureChangeType_ChildrenInvalidated, ids, 2);
                }
                break;
        }
        VariantClear(&after);
        element->Release();
    }
}

void AccessRoot::notify(std::string_view text, bool important) {
    if (!bridge_ || !bridge_->live() || root_ == nullptr || uia().raise_notification == nullptr || text.empty() ||
        !listening()) {
        return;
    }
    BSTR said = bstr(access_text(text));
    BSTR activity = SysAllocString(L"FloServer.Message");
    uia().raise_notification(root_, important ? kNotificationOther : kNotificationActionCompleted,
                             important ? kProcessingImportantMostRecent : kProcessingMostRecent, said, activity);
    SysFreeString(said);
    SysFreeString(activity);
}

// --- a real control's name ----------------------------------------------------------

namespace {

// oleacc's Dynamic Annotation (CLSID_AccPropServices, IAccPropServices,
// PROPID_ACC_NAME), spelled out: MinGW's libraries don't all define them.
constexpr GUID kClsidAccPropServices = {0xb5f8350b, 0x0548, 0x48b1, {0xa6, 0xee, 0x88, 0xbd, 0x00, 0xb4, 0xa5, 0xe7}};
constexpr GUID kIidAccPropServices = {0x6e26e776, 0x04f0, 0x495d, {0x80, 0xe4, 0x33, 0x30, 0x35, 0x2e, 0x31, 0x69}};
constexpr GUID kPropIdAccName = {0x608d3df8, 0x8128, 0x4aa7, {0xa4, 0x28, 0xf5, 0x5e, 0x49, 0x26, 0x72, 0x91}};
constexpr DWORD kObjIdClient = 0xFFFFFFFCUL;  // OBJID_CLIENT
constexpr DWORD kChildIdSelf = 0;

}  // namespace

void set_accessible_name(HWND control, std::string_view name) {
    if (control == nullptr) {
        return;
    }
    IAccPropServices* services = nullptr;
    if (FAILED(CoCreateInstance(kClsidAccPropServices, nullptr, CLSCTX_INPROC_SERVER, kIidAccPropServices,
                                reinterpret_cast<void**>(&services))) ||
        services == nullptr) {
        return;
    }
    if (name.empty()) {
        MSAAPROPID props[] = {kPropIdAccName};
        services->ClearHwndProps(control, kObjIdClient, kChildIdSelf, props, 1);
    } else {
        services->SetHwndPropStr(control, kObjIdClient, kChildIdSelf, kPropIdAccName,
                                 win::widen(access_text(name)).c_str());
    }
    services->Release();
}

}  // namespace floserver::ui
