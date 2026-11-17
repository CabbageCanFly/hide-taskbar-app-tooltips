// ==WindhawkMod==
// @id              hide-taskbar-app-tooltips-only
// @name            Hide Taskbar Application Tooltips Only
// @description     Hides only positively identified taskbar application tooltips; preserves other tooltips and previews.
// @version         1.4.0-apps-only.1
// @author          CabbageCanFly (modifications), gilnett (original)
// @github          https://github.com/CabbageCanFly
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lruntimeobject
// @donateUrl       https://buymeacoffee.com/gilnett
// @license         GPL-3.0
// ==/WindhawkMod==

// Source code is published under The GNU General Public License v3.0.

// Modified by CabbageCanFly: application-only classification and suppression.
// Publication metadata updated 2026-11-17. Original author: gilnett.
// Derivative of gilnett's hide-taskbar-tooltips v1.4.0:
// https://github.com/ramensoftware/windhawk-mods/blob/main/mods/hide-taskbar-tooltips.wh.cpp
// ==WindhawkModReadme==
/*
# Hide Taskbar Application Tooltips Only

Windows 11 x64. Hides only positively identified application button tooltips.
Start, tray, clock/status, tray overflow and unknown shell tooltips stay native.
Thumbnail previews and original hover handlers are preserved.

Disable the original Hide Taskbar Tooltips mod BEFORE compiling/loading this
local mod. Never enable both versions together. See the accompanying README
for installation and known limitations. No settings are needed.

Unrecognized tooltips are allowed. A classification diagnostic is logged once
per Explorer process. Windows updates can require symbol updates.
*/
// ==/WindhawkModReadme==

#include <windhawk_api.h>
#include <windhawk_utils.h>

#include <inspectable.h>
#include <roapi.h>
#include <windows.h>
#include <winstring.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <weakreference.h>

using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HString;
using Microsoft::WRL::Wrappers::HStringReference;

struct WinrtSize {
    float Width;
    float Height;
};

struct WinrtPoint {
    float X;
    float Y;
};

struct WinrtRect {
    float X;
    float Y;
    float Width;
    float Height;
};

// {5C526665-F60E-4912-AF59-5FE0680F089D}
static constexpr GUID kIID_IDependencyObject = {
    0x5C526665,
    0xF60E,
    0x4912,
    {0xAF, 0x59, 0x5F, 0xE0, 0x68, 0x0F, 0x08, 0x9D}};

// {7DE5D75F-4415-429F-B523-699BDB8C30DB}
static constexpr GUID kIID_IToolTip = {
    0x7DE5D75F,
    0x4415,
    0x429F,
    {0xB5, 0x23, 0x69, 0x9B, 0xDB, 0x8C, 0x30, 0xDB}};

// {89839403-B538-49E3-A430-3AC037DC6FE0}
static constexpr GUID kIID_IToolTipFactory = {
    0x89839403,
    0xB538,
    0x49E3,
    {0xA4, 0x30, 0x3A, 0xC0, 0x37, 0xDC, 0x6F, 0xE0}};

// {E75758C4-D25D-4B1D-971F-596F17F12BAA}
static constexpr GUID kIID_IVisualTreeHelperStatics = {
    0xE75758C4,
    0xD25D,
    0x4B1D,
    {0x97, 0x1F, 0x59, 0x6F, 0x17, 0xF1, 0x2B, 0xAA}};

struct IToolTipAbi : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_HorizontalOffset(double* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_HorizontalOffset(double value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_IsOpen(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsOpen(boolean value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Placement(int32_t* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_Placement(int32_t value) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    get_PlacementTarget(IInspectable** value) = 0;
};

struct IToolTipFactoryAbi : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE
    CreateInstance(IInspectable* baseInterface,
                   IInspectable** innerInterface,
                   IToolTipAbi** value) = 0;
};

struct IVisualTreeHelperStaticsAbi : public IInspectable {
    virtual HRESULT STDMETHODCALLTYPE
    FindElementsInHostCoordinatesPoint(WinrtPoint intersectingPoint,
                                       IInspectable* subtree,
                                       IInspectable** elements) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    FindElementsInHostCoordinatesRect(WinrtRect intersectingRect,
                                      IInspectable* subtree,
                                      IInspectable** elements) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    FindAllElementsInHostCoordinatesPoint(WinrtPoint intersectingPoint,
                                          IInspectable* subtree,
                                          boolean includeAllElements,
                                          IInspectable** elements) = 0;
    virtual HRESULT STDMETHODCALLTYPE
    FindAllElementsInHostCoordinatesRect(WinrtRect intersectingRect,
                                         IInspectable* subtree,
                                         boolean includeAllElements,
                                         IInspectable** elements) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChild(IInspectable* reference,
                                               int32_t childIndex,
                                               IInspectable** child) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChildrenCount(IInspectable* reference,
                                                       int32_t* count) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParent(IInspectable* reference,
                                                IInspectable** parent) = 0;
};

static HString GetRuntimeClassName(IUnknown* pUnk) {
    HString str;
    if (!pUnk) {
        return str;
    }
    ComPtr<IInspectable> spInsp;
    if (SUCCEEDED(pUnk->QueryInterface(__uuidof(IInspectable), &spInsp)) &&
        spInsp) {
        spInsp->GetRuntimeClassName(str.GetAddressOf());
    }
    return str;
}

static std::wstring_view GetStringView(const HString& str) {
    unsigned int len = 0;
    const wchar_t* raw = str.GetRawBuffer(&len);
    return raw ? std::wstring_view(raw, len) : std::wstring_view{};
}

static ComPtr<IVisualTreeHelperStaticsAbi> GetVisualTreeHelperStatics() {
    ComPtr<IVisualTreeHelperStaticsAbi> spStatics;
    RoGetActivationFactory(
        HStringReference(L"Windows.UI.Xaml.Media.VisualTreeHelper").Get(),
        kIID_IVisualTreeHelperStatics, &spStatics);
    return spStatics;
}

static ComPtr<IInspectable> GetVisualTreeParent(
    IVisualTreeHelperStaticsAbi* pHelper, IUnknown* pElement) {
    ComPtr<IInspectable> spParent;
    if (!pHelper || !pElement) {
        return spParent;
    }
    ComPtr<IInspectable> spDepObj;
    if (FAILED(pElement->QueryInterface(kIID_IDependencyObject, &spDepObj)) ||
        !spDepObj) {
        return spParent;
    }
    pHelper->GetParent(spDepObj.Get(), &spParent);
    return spParent;
}

static bool IsWindows11OrGreater() {
    HMODULE hNtdll = GetModuleHandleW(L"ntdll.dll");
    if (!hNtdll) {
        return false;
    }

    using fnRtlGetNtVersionNumbers =
        void(WINAPI*)(LPDWORD major, LPDWORD minor, LPDWORD build);
    auto RtlGetNtVersionNumbers =
        reinterpret_cast<fnRtlGetNtVersionNumbers>(
            GetProcAddress(hNtdll, "RtlGetNtVersionNumbers"));
    if (!RtlGetNtVersionNumbers) {
        return false;
    }

    DWORD major = 0, minor = 0, build = 0;
    RtlGetNtVersionNumbers(&major, &minor, &build);
    build &= ~0xF0000000;
    return (major > 10) || (major == 10 && build >= 22000);
}

static std::atomic<bool> g_uninitializing{false};
static thread_local int t_taskbarAppHoverDepth = 0;

struct TaskbarAppHoverScope {
    TaskbarAppHoverScope() { t_taskbarAppHoverDepth++; }
    ~TaskbarAppHoverScope() { t_taskbarAppHoverDepth--; }
};

static void LogUnknownOnce(const wchar_t* reason) {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true, std::memory_order_relaxed)) {
        Wh_Log(L"[CLASSIFY] %s; leaving tooltip visible. Further classification "
               L"failures are silent for this Explorer process.", reason);
    }
}

enum class ToolTipArea {
    Unknown,
    TaskbarApp,
    StartButton,
    SystemTray,
    TrayOverflow,
};

// Weak references prevent recycled COM addresses inheriting an app classification.
static std::unordered_map<void*, ComPtr<IWeakReference>> s_taskbarToolTips;
static std::mutex s_toolTipsMutex;

static constexpr bool IsAreaSuppressed(ToolTipArea area) {
    return area == ToolTipArea::TaskbarApp;
}
static_assert(IsAreaSuppressed(ToolTipArea::TaskbarApp));
static_assert(!IsAreaSuppressed(ToolTipArea::Unknown));
static_assert(!IsAreaSuppressed(ToolTipArea::StartButton));
static_assert(!IsAreaSuppressed(ToolTipArea::SystemTray));
static_assert(!IsAreaSuppressed(ToolTipArea::TrayOverflow));

static void RegisterAppToolTip(void* pToolTip) {
    if (!pToolTip || t_taskbarAppHoverDepth == 0) {
        return;
    }
    ComPtr<IToolTipAbi> tooltip;
    ComPtr<IWeakReferenceSource> source;
    ComPtr<IWeakReference> weak;
    auto* unk = static_cast<IUnknown*>(pToolTip);
    if (FAILED(unk->QueryInterface(kIID_IToolTip, &tooltip)) || !tooltip ||
        FAILED(tooltip->QueryInterface(IID_PPV_ARGS(&source))) || !source ||
        FAILED(source->GetWeakReference(&weak)) || !weak) {
        LogUnknownOnce(L"Application tooltip does not support a usable weak reference");
        return;
    }
    std::lock_guard<std::mutex> lock(s_toolTipsMutex);
    // shortcut: bounded registration cache, increase only if >500 live app tooltips are needed.
    if (s_taskbarToolTips.size() >= 500) {
        s_taskbarToolTips.clear();
    }
    s_taskbarToolTips[tooltip.Get()] = weak;
}

static ToolTipArea LookupRegisteredToolTip(void* pToolTip) {
    ComPtr<IWeakReference> weak;
    {
        std::lock_guard<std::mutex> lock(s_toolTipsMutex);
        auto it = s_taskbarToolTips.find(pToolTip);
        if (it == s_taskbarToolTips.end()) {
            return ToolTipArea::Unknown;
        }
        weak = it->second;
    }
    ComPtr<IToolTipAbi> live;
    if (SUCCEEDED(weak->Resolve(kIID_IToolTip,
                               reinterpret_cast<IInspectable**>(live.GetAddressOf()))) &&
        live && live.Get() == pToolTip) {
        return ToolTipArea::TaskbarApp;
    }
    return ToolTipArea::Unknown;
}

static bool IsInNotificationAreaOverflow(IVisualTreeHelperStaticsAbi* pHelper,
                                         IUnknown* pElement) {
    if (!pHelper || !pElement) {
        return false;
    }
    ComPtr<IInspectable> current;
    if (FAILED(pElement->QueryInterface(__uuidof(IInspectable), &current))) {
        return false;
    }
    for (int depth = 0; current && depth < 24;
         depth++, current = GetVisualTreeParent(pHelper, current.Get())) {
        if (GetStringView(GetRuntimeClassName(current.Get())) ==
            L"SystemTray.NotificationAreaOverflow") {
            return true;
        }
    }
    return false;
}

static ToolTipArea InspectPlacementTargetArea(void* pIToolTip, bool* hasTarget) {
    *hasTarget = true;
    if (!pIToolTip) {
        return ToolTipArea::Unknown;
    }
    try {
        auto* pUnk = static_cast<IUnknown*>(pIToolTip);
        ComPtr<IToolTipAbi> spToolTip;
        if (FAILED(pUnk->QueryInterface(kIID_IToolTip, &spToolTip)) ||
            !spToolTip) {
            return ToolTipArea::Unknown;
        }

        ComPtr<IInspectable> target;
        if (FAILED(spToolTip->get_PlacementTarget(&target))) {
            return ToolTipArea::Unknown;
        }
        *hasTarget = target != nullptr;
        if (target) {
            ComPtr<IVisualTreeHelperStaticsAbi> spVisualHelper =
                GetVisualTreeHelperStatics();
            if (IsInNotificationAreaOverflow(spVisualHelper.Get(),
                                             target.Get())) {
                return ToolTipArea::TrayOverflow;
            }

            ComPtr<IInspectable> current = target;
            for (int depth = 0; current && depth < 16; depth++) {
                HString name = GetRuntimeClassName(current.Get());
                std::wstring_view sv = GetStringView(name);
                if (!sv.empty()) {
                    if (sv == L"Taskbar.TaskListButton") {
                        return ToolTipArea::TaskbarApp;
                    }
                    if (sv == L"Taskbar.ExperienceToggleButton") {
                        return ToolTipArea::StartButton;
                    }
                    if (sv.find(L"SystemTray") != std::wstring_view::npos ||
                        sv.find(L"Tray") != std::wstring_view::npos ||
                        sv.find(L"Clock") != std::wstring_view::npos ||
                        sv.find(L"StatusArea") != std::wstring_view::npos) {
                        return ToolTipArea::SystemTray;
                    }
                }
                current =
                    GetVisualTreeParent(spVisualHelper.Get(), current.Get());
            }
        }

    } catch (...) {
        LogUnknownOnce(L"Exception inspecting tooltip placement target");
    }
    return ToolTipArea::Unknown;
}

static ToolTipArea IdentifyToolTipArea(void* pThis) {
    if (!pThis) {
        return ToolTipArea::Unknown;
    }
    bool hasTarget;
    ToolTipArea targetArea = InspectPlacementTargetArea(pThis, &hasTarget);
    if (targetArea != ToolTipArea::Unknown) {
        return targetArea;
    }
    // Timer-driven opens can have no PlacementTarget: require prior app-handler registration.
    ToolTipArea cached = hasTarget ? ToolTipArea::Unknown
                                  : LookupRegisteredToolTip(pThis);
    if (cached == ToolTipArea::Unknown) {
        LogUnknownOnce(hasTarget ? L"Unrecognized placement target or target query failed"
                                 : L"No placement target and no live application registration");
    }
    return cached;
}

static BOOL CALLBACK EnumWindowsTaskbarProc(HWND hWnd, LPARAM lParam) {
    DWORD dwProcessId = 0;
    WCHAR className[32] = {0};
    if (GetWindowThreadProcessId(hWnd, &dwProcessId) &&
        dwProcessId == GetCurrentProcessId() &&
        GetClassNameW(hWnd, className, ARRAYSIZE(className)) &&
        _wcsicmp(className, L"Shell_TrayWnd") == 0) {
        *reinterpret_cast<HWND*>(lParam) = hWnd;
        return FALSE;
    }
    return TRUE;
}

static HWND FindCurrentProcessTaskbarWnd() {
    HWND hTaskbarWnd = nullptr;
    EnumWindows(EnumWindowsTaskbarProc, reinterpret_cast<LPARAM>(&hTaskbarWnd));
    return hTaskbarWnd;
}

// WinRT IToolTip ABI: put_IsOpen is at vtable index 9
using ToolTip_put_IsOpen_t = HRESULT(__stdcall*)(void* pThis, boolean value);
static ToolTip_put_IsOpen_t g_toolTipPutIsOpenOriginal = nullptr;
static std::atomic<bool> g_putIsOpenHooked{false};

static HRESULT __stdcall ToolTip_put_IsOpen_Hook(void* pThis, boolean value) {
    if (g_uninitializing.load(std::memory_order_relaxed)) {
        if (g_toolTipPutIsOpenOriginal) {
            return g_toolTipPutIsOpenOriginal(pThis, value);
        }
        return S_OK;
    }

    if (!value) {
        if (g_toolTipPutIsOpenOriginal) {
            return g_toolTipPutIsOpenOriginal(pThis, value);
        }
        return S_OK;
    }

    ToolTipArea area = IdentifyToolTipArea(pThis);
    if (IsAreaSuppressed(area)) {
        // Hidden area: always suppress permanently
        return S_OK;
    }

    if (g_toolTipPutIsOpenOriginal) {
        return g_toolTipPutIsOpenOriginal(pThis, value);
    }
    return S_OK;
}

using RunFromWindowThreadProc_t = void(WINAPI*)(void* parameter);

static UINT GetRunFromWindowThreadMessage() {
    static const UINT message =
        RegisterWindowMessageW(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);
    return message;
}

struct RunFromWindowThreadParam {
    RunFromWindowThreadProc_t proc;
    void* procParam;
};

static LRESULT CALLBACK CallWndProcHook(int nCode,
                                        WPARAM wParam,
                                        LPARAM lParam) {
    if (nCode == HC_ACTION) {
        const auto* cwp = reinterpret_cast<const CWPSTRUCT*>(lParam);
        if (cwp->message == GetRunFromWindowThreadMessage()) {
            auto* param =
                reinterpret_cast<RunFromWindowThreadParam*>(cwp->lParam);
            if (param && param->proc) {
                param->proc(param->procParam);
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

static bool RunFromWindowThread(HWND hWnd,
                                RunFromWindowThreadProc_t proc,
                                void* procParam) {
    DWORD dwThreadId = GetWindowThreadProcessId(hWnd, nullptr);
    if (dwThreadId == 0) {
        return false;
    }

    if (dwThreadId == GetCurrentThreadId()) {
        proc(procParam);
        return true;
    }

    HHOOK hook = SetWindowsHookExW(
        WH_CALLWNDPROC,
        CallWndProcHook,
        nullptr, dwThreadId);
    if (!hook) {
        return false;
    }

    RunFromWindowThreadParam param{proc, procParam};
    SendMessageW(hWnd, GetRunFromWindowThreadMessage(), 0,
                 reinterpret_cast<LPARAM>(&param));

    UnhookWindowsHookEx(hook);
    return true;
}

static void WINAPI EagerHookToolTipOnTaskbarThread(void* /*procParam*/) {
    try {
        ComPtr<IToolTipFactoryAbi> spFactory;
        if (FAILED(RoGetActivationFactory(
                HStringReference(L"Windows.UI.Xaml.Controls.ToolTip").Get(),
                kIID_IToolTipFactory, &spFactory)) ||
            !spFactory) {
            return;
        }

        ComPtr<IInspectable> spInner;
        ComPtr<IToolTipAbi> spToolTip;
        if (SUCCEEDED(
                spFactory->CreateInstance(nullptr, &spInner, &spToolTip)) &&
            spToolTip) {
            void** vtable = *reinterpret_cast<void***>(spToolTip.Get());
            if (vtable && vtable[9]) {
                auto put_IsOpen =
                    reinterpret_cast<ToolTip_put_IsOpen_t>(vtable[9]);
                if (!g_putIsOpenHooked.exchange(true)) {
                    if (!WindhawkUtils::SetFunctionHook(
                        reinterpret_cast<void*>(put_IsOpen),
                        reinterpret_cast<void*>(ToolTip_put_IsOpen_Hook),
                        reinterpret_cast<void**>(&g_toolTipPutIsOpenOriginal))) {
                        g_putIsOpenHooked.store(false, std::memory_order_relaxed);
                        static std::atomic<bool> logged{false};
                        if (!logged.exchange(true, std::memory_order_relaxed)) {
                            Wh_Log(L"[HOOK] Could not hook ToolTip::put_IsOpen; tooltips remain native");
                        }
                        return;
                    }
                    Wh_ApplyHookOperations();
                    Wh_Log(
                        L"[HOOK] Hooked ToolTip::put_IsOpen eagerly on "
                        L"Taskbar UI thread");
                }
            }
        }
    } catch (...) {
        Wh_Log(L"[HOOK] Eager ToolTip hook on UI thread caught exception");
    }
}

static void EnsurePutIsOpenHooked() {
    if (!g_putIsOpenHooked.load(std::memory_order_relaxed)) {
        EagerHookToolTipOnTaskbarThread(nullptr);
    }
}

static void InitEagerToolTipHook() {
    HWND hTaskbarWnd = FindCurrentProcessTaskbarWnd();
    if (hTaskbarWnd) {
        DWORD threadId = GetWindowThreadProcessId(hTaskbarWnd, nullptr);
        Wh_Log(
            L"[HOOK] Dispatching eager ToolTip hook to Taskbar window %p "
            L"(thread %u)",
            hTaskbarWnd, threadId);
        RunFromWindowThread(hTaskbarWnd, EagerHookToolTipOnTaskbarThread,
                            nullptr);
    } else {
        Wh_Log(L"[HOOK] Taskbar window not found yet for eager ToolTip hook");
    }
}

// Taskbar.View.dll: TaskbarLocationHelpers::ApplyTaskbarTooltipPlacement (4 params)
using ApplyTaskbarTooltipPlacement_t = void(__cdecl*)(void* pToolTip,
                                                      int location,
                                                      WinrtSize size1,
                                                      WinrtSize size2);

static ApplyTaskbarTooltipPlacement_t
    ApplyTaskbarTooltipPlacement_View_Original = nullptr;

static void __cdecl ApplyTaskbarTooltipPlacement_View_Hook(void* pToolTip,
                                                           int location,
                                                           WinrtSize size1,
                                                           WinrtSize size2) {
    EnsurePutIsOpenHooked();
    void* pIToolTip =
        pToolTip ? *(reinterpret_cast<void**>(pToolTip)) : nullptr;
    RegisterAppToolTip(pIToolTip);
    if (ApplyTaskbarTooltipPlacement_View_Original) {
        ApplyTaskbarTooltipPlacement_View_Original(pToolTip, location, size1, size2);
    }
}

// Taskbar.View.dll: TaskbarLocationHelpers::ApplyTaskbarTooltipPlacement (6 params)
using ApplyTaskbarTooltipPlacement6_t = void(__cdecl*)(void* pToolTip,
                                                       int location,
                                                       WinrtSize size1,
                                                       WinrtSize size2,
                                                       bool b1,
                                                       bool b2);
static ApplyTaskbarTooltipPlacement6_t ApplyTaskbarTooltipPlacement6_Original =
    nullptr;

static void __cdecl ApplyTaskbarTooltipPlacement6_Hook(void* pToolTip,
                                                       int location,
                                                       WinrtSize size1,
                                                       WinrtSize size2,
                                                       bool b1,
                                                       bool b2) {
    EnsurePutIsOpenHooked();
    void* pIToolTip =
        pToolTip ? *(reinterpret_cast<void**>(pToolTip)) : nullptr;
    RegisterAppToolTip(pIToolTip);
    if (ApplyTaskbarTooltipPlacement6_Original) {
        ApplyTaskbarTooltipPlacement6_Original(pToolTip, location, size1, size2, b1, b2);
    }
}

// Taskbar.View.dll: TaskbarLocationHelpers::ApplyTaskbarTooltipOrientation
using ApplyTaskbarTooltipOrientation2_t = void(__cdecl*)(void* pToolTip,
                                                         int location);
static ApplyTaskbarTooltipOrientation2_t
    ApplyTaskbarTooltipOrientation2_Original = nullptr;

static void __cdecl ApplyTaskbarTooltipOrientation2_Hook(void* pToolTip,
                                                         int location) {
    EnsurePutIsOpenHooked();
    void* pIToolTip =
        pToolTip ? *(reinterpret_cast<void**>(pToolTip)) : nullptr;
    RegisterAppToolTip(pIToolTip);
    if (ApplyTaskbarTooltipOrientation2_Original) {
        ApplyTaskbarTooltipOrientation2_Original(pToolTip, location);
    }
}

using ApplyTaskbarTooltipOrientation6_t = void(__cdecl*)(void* pToolTip,
                                                         int location,
                                                         bool b1,
                                                         bool b2,
                                                         float f1,
                                                         double d1);
static ApplyTaskbarTooltipOrientation6_t
    ApplyTaskbarTooltipOrientation6_Original = nullptr;

static void __cdecl ApplyTaskbarTooltipOrientation6_Hook(void* pToolTip,
                                                         int location,
                                                         bool b1,
                                                         bool b2,
                                                         float f1,
                                                         double d1) {
    EnsurePutIsOpenHooked();
    void* pIToolTip =
        pToolTip ? *(reinterpret_cast<void**>(pToolTip)) : nullptr;
    RegisterAppToolTip(pIToolTip);
    if (ApplyTaskbarTooltipOrientation6_Original) {
        ApplyTaskbarTooltipOrientation6_Original(pToolTip, location, b1, b2, f1, d1);
    }
}

// Preserve the entire application hover handler, including preview interactions.
using TaskListButton_UpdateHover_t = void(__cdecl*)(void* pThis);
static TaskListButton_UpdateHover_t TaskListButton_UpdateHover_Original =
    nullptr;

static void __cdecl TaskListButton_UpdateHover_Hook(void* pThis) {
    EnsurePutIsOpenHooked();
    TaskbarAppHoverScope scope;
    if (TaskListButton_UpdateHover_Original) {
        TaskListButton_UpdateHover_Original(pThis);
    }
}

static std::atomic<bool> g_taskbarViewHooked{false};

static bool HookTaskbarViewSymbols(HMODULE module) {
    Wh_Log(L"[HOOK] Hooking Taskbar.View symbols (module=%p)", module);
    // Taskbar.View.dll
    WindhawkUtils::SYMBOL_HOOK taskbarViewHooks[] = {
        {
            {
                LR"(public: void __cdecl winrt::Taskbar::implementation::TaskListButton::UpdateToolTipPlacementForHoverState(void))",
            },
            &TaskListButton_UpdateHover_Original,
            TaskListButton_UpdateHover_Hook,
            true,
        },
        {
            {
                LR"(void __cdecl TaskbarLocationHelpers::ApplyTaskbarTooltipPlacement(struct winrt::Windows::UI::Xaml::Controls::ToolTip const &,enum winrt::WindowsUdk::UI::Shell::TaskbarLocation,struct winrt::Windows::Foundation::Size,struct winrt::Windows::Foundation::Size,bool,bool))",
                LR"(void __cdecl TaskbarLocationHelpers::ApplyTaskbarTooltipPlacement(struct winrt::Windows::UI::Xaml::Controls::ToolTip const &,enum winrt::WindowsUdk::Shell::TaskbarLocation,struct winrt::Windows::Foundation::Size,struct winrt::Windows::Foundation::Size,bool,bool))",
            },
            &ApplyTaskbarTooltipPlacement6_Original,
            ApplyTaskbarTooltipPlacement6_Hook,
            true,
        },
        {
            {
                LR"(void __cdecl TaskbarLocationHelpers::ApplyTaskbarTooltipPlacement(struct winrt::Windows::UI::Xaml::Controls::ToolTip const &,enum winrt::WindowsUdk::UI::Shell::TaskbarLocation,struct winrt::Windows::Foundation::Size,struct winrt::Windows::Foundation::Size))",
                LR"(void __cdecl TaskbarLocationHelpers::ApplyTaskbarTooltipPlacement(struct winrt::Windows::UI::Xaml::Controls::ToolTip const &,enum winrt::WindowsUdk::Shell::TaskbarLocation,struct winrt::Windows::Foundation::Size,struct winrt::Windows::Foundation::Size))",
            },
            &ApplyTaskbarTooltipPlacement_View_Original,
            ApplyTaskbarTooltipPlacement_View_Hook,
            true,
        },
        {
            {
                LR"(void __cdecl TaskbarLocationHelpers::ApplyTaskbarTooltipOrientation(struct winrt::Windows::UI::Xaml::Controls::ToolTip const &,enum winrt::WindowsUdk::UI::Shell::TaskbarLocation))",
                LR"(void __cdecl TaskbarLocationHelpers::ApplyTaskbarTooltipOrientation(struct winrt::Windows::UI::Xaml::Controls::ToolTip const &,enum winrt::WindowsUdk::Shell::TaskbarLocation))",
            },
            &ApplyTaskbarTooltipOrientation2_Original,
            ApplyTaskbarTooltipOrientation2_Hook,
            true,
        },
        {
            {
                LR"(void __cdecl TaskbarLocationHelpers::ApplyTaskbarTooltipOrientation(struct winrt::Windows::UI::Xaml::Controls::ToolTip const &,enum winrt::WindowsUdk::UI::Shell::TaskbarLocation,bool,bool,float,double))",
                LR"(void __cdecl TaskbarLocationHelpers::ApplyTaskbarTooltipOrientation(struct winrt::Windows::UI::Xaml::Controls::ToolTip const &,enum winrt::WindowsUdk::Shell::TaskbarLocation,bool,bool,float,double))",
            },
            &ApplyTaskbarTooltipOrientation6_Original,
            ApplyTaskbarTooltipOrientation6_Hook,
            true,
        },
    };

    bool res = WindhawkUtils::HookSymbols(module, taskbarViewHooks,
                                          ARRAYSIZE(taskbarViewHooks));
    Wh_Log(L"[HOOK] HookTaskbarViewSymbols completed (result=%d)", res ? 1 : 0);
    if (!TaskListButton_UpdateHover_Original ||
        (!ApplyTaskbarTooltipPlacement_View_Original &&
         !ApplyTaskbarTooltipPlacement6_Original &&
         !ApplyTaskbarTooltipOrientation2_Original &&
         !ApplyTaskbarTooltipOrientation6_Original)) {
        Wh_Log(L"[HOOK] App registration symbols unavailable; timer-driven app "
               L"tooltips without placement targets will remain visible");
    }
    return res;
}

static void HandleLoadedModule(HMODULE module) {
    if (!module) {
        return;
    }

    if (!g_taskbarViewHooked &&
               GetModuleHandleW(L"Taskbar.View.dll") == module) {
        if (!g_taskbarViewHooked.exchange(true)) {
            Wh_Log(L"[INIT] Loaded Taskbar.View.dll dynamically");
            HookTaskbarViewSymbols(module);
            Wh_ApplyHookOperations();
        }
    }
}

using LoadLibraryExW_t = decltype(&LoadLibraryExW);
static LoadLibraryExW_t LoadLibraryExW_Original = nullptr;

static HMODULE WINAPI LoadLibraryExW_Hook(LPCWSTR lpLibFileName,
                                          HANDLE hFile,
                                          DWORD dwFlags) {
    HMODULE module = LoadLibraryExW_Original(lpLibFileName, hFile, dwFlags);
    if (module) {
        HandleLoadedModule(module);
    }
    return module;
}

BOOL Wh_ModInit() {
    Wh_Log(L"[INIT] Initializing Hide Taskbar Tooltips mod v" WH_MOD_VERSION);

    if (!IsWindows11OrGreater()) {
        Wh_Log(L"[INIT] Only Windows 11 is supported");
        return FALSE;
    }

    g_uninitializing.store(false, std::memory_order_relaxed);

    bool hookedAny = false;

    if (HMODULE taskbarViewModule = GetModuleHandleW(L"Taskbar.View.dll")) {
        g_taskbarViewHooked = true;
        if (HookTaskbarViewSymbols(taskbarViewModule)) {
            hookedAny = true;
        }
    }

    bool waitingForModules = false;
    if (!g_taskbarViewHooked) {
        HMODULE kernelBaseModule = GetModuleHandleW(L"kernelbase.dll");
        if (kernelBaseModule) {
            auto pKernelBaseLoadLibraryExW =
                reinterpret_cast<decltype(&LoadLibraryExW)>(
                    GetProcAddress(kernelBaseModule, "LoadLibraryExW"));
            if (pKernelBaseLoadLibraryExW) {
                WindhawkUtils::SetFunctionHook(pKernelBaseLoadLibraryExW,
                                               LoadLibraryExW_Hook,
                                               &LoadLibraryExW_Original);
                waitingForModules = true;
            }
        }
    }

    return hookedAny || waitingForModules;
}

void Wh_ModAfterInit() {
    InitEagerToolTipHook();
    Wh_Log(
        L"[INIT] Application tooltips only active (taskbarView=%d)",
        static_cast<int>(g_taskbarViewHooked.load()));
}

void Wh_ModBeforeUninit() {
    g_uninitializing.store(true, std::memory_order_relaxed);
}

void Wh_ModUninit() {
    Wh_Log(L"[UNINIT] Uninitializing Hide Taskbar Tooltips mod");
    std::lock_guard<std::mutex> lock(s_toolTipsMutex);
    s_taskbarToolTips.clear();
}
