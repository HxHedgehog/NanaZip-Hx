/*
 * PROJECT:    NanaZip Platform User Library (K7User)
 * FILE:       K7UserDarkMode.cpp
 * PURPOSE:    Implementation for NanaZip Platform User Dark Mode Support
 *
 * LICENSE:    The MIT License
 *
 * MAINTAINER: MouriNaruto (Kenji.Mouri@outlook.com)
 */

#include "K7UserPrivate.h"

#include <Mile.Helpers.h>
#include <Mile.Helpers.CppBase.h>

#include <K7Base.h>

#include <Uxtheme.h>
#pragma comment(lib, "Uxtheme.lib")

EXTERN_C HTHEME WINAPI OpenNcThemeData(
    _In_opt_ HWND hwnd,
    _In_ LPCWSTR pszClassList);

EXTERN_C HRESULT WINAPI GetThemeClass(
    _In_ HTHEME hTheme,
    _Out_ LPWSTR pszClassName,
    _In_ int cchClassName);

#include <vssym32.h>
#include <Richedit.h>

#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#include <ShellScalingApi.h>

#include <CommCtrl.h>
#pragma comment(lib,"comctl32.lib")

#include <stdarg.h> // *** TEMPORARY DEBUG INSTRUMENTATION ***

// TODO: Move some workaround for NanaZip.UI.* to this.

namespace
{
    const COLORREF g_LightModeBackgroundColor = RGB(255, 255, 255);
    const COLORREF g_LightModeForegroundColor = RGB(0, 0, 0);

    const COLORREF g_DarkModeBackgroundColor = RGB(0, 0, 0);
    const COLORREF g_DarkModeForegroundColor = RGB(255, 255, 255);
    const COLORREF g_DarkModeBorderColor = RGB(127, 127, 127);
    const COLORREF g_DarkModeMenuSelectedBackgroundColor = RGB(65, 65, 65);

    // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
    static void K7ThemeDebugTrace(
        _In_z_ PCWSTR Format,
        ...)
    {
        SYSTEMTIME Time = {};
        ::GetLocalTime(&Time);

        WCHAR Buffer[1024] = {};
        int Length = wsprintfW(
            Buffer,
            L"[%02u:%02u:%02u.%03u][%u:%u] ",
            Time.wHour,
            Time.wMinute,
            Time.wSecond,
            Time.wMilliseconds,
            ::GetCurrentProcessId(),
            ::GetCurrentThreadId());

        va_list Arguments;
        va_start(Arguments, Format);
        Length += wvsprintfW(Buffer + Length, Format, Arguments);
        va_end(Arguments);

        Buffer[Length++] = L'\r';
        Buffer[Length++] = L'\n';
        Buffer[Length] = L'\0';

        OutputDebugStringW(Buffer);

        WCHAR Path[MAX_PATH + 32] = {};
        if (0 != ::GetTempPathW(MAX_PATH, Path))
        {
            lstrcatW(Path, L"NanaZipThemeDebug.log");
            HANDLE FileHandle = ::CreateFileW(
                Path,
                FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE,
                nullptr,
                OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL,
                nullptr);
            if (INVALID_HANDLE_VALUE != FileHandle)
            {
                DWORD Written = 0;
                ::WriteFile(
                    FileHandle,
                    Buffer,
                    Length * sizeof(WCHAR),
                    &Written,
                    nullptr);
                ::CloseHandle(FileHandle);
            }
        }
    }
    // *** END TEMPORARY DEBUG INSTRUMENTATION ***

    static bool K7UserReadThemeInvert()
    {
        // The "Invert Theme" option is exposed by the File Manager settings and
        // is stored as a REG_DWORD under HKCU\Software\NanaZip\FM\InvertTheme.
        DWORD Value = 0;
        DWORD ValueSize = sizeof(Value);

        HKEY KeyHandle = nullptr;
        if (ERROR_SUCCESS == ::RegOpenKeyExW(
            HKEY_CURRENT_USER,
            L"Software\\NanaZip\\FM",
            0,
            KEY_READ,
            &KeyHandle))
        {
            if (ERROR_SUCCESS != ::RegQueryValueExW(
                KeyHandle,
                L"InvertTheme",
                nullptr,
                nullptr,
                reinterpret_cast<LPBYTE>(&Value),
                &ValueSize))
            {
                Value = 0;
            }
            ::RegCloseKey(KeyHandle);
        }

        return (Value != 0);
    }

    static bool ComputeShouldAppsUseDarkMode()
    {
        const bool BaseShouldUseDarkMode =
            ::MileShouldAppsUseDarkMode() &&
            !::MileShouldAppsUseHighContrastMode();
        const bool InvertTheme = ::K7UserReadThemeInvert();
        const bool Result = InvertTheme ? !BaseShouldUseDarkMode
                                        : BaseShouldUseDarkMode;
        // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
        K7ThemeDebugTrace(
            L"ComputeShouldAppsUseDarkMode: systemDark=%d invert=%d => dark=%d",
            (int)BaseShouldUseDarkMode,
            (int)InvertTheme,
            (int)Result);
        // *** END TEMPORARY DEBUG INSTRUMENTATION ***
        return Result;
    }

    // uxtheme ordinal 135 (SetPreferredAppMode). The Mile headers available
    // in some build environments don't expose MILE_PREFERRED_APP_MODE_FORCE_DARK
    // (enum values can't be guarded with #ifndef), so the undocumented export
    // is called directly. This is the same export MileSetPreferredAppMode
    // wraps; value 2 (ForceDark) is guaranteed by the uxtheme contract.
    enum class K7PreferredAppMode : int
    {
        Default = 0,
        AllowDark = 1,
        ForceDark = 2,
        ForceLight = 3,
        Max = 4,
    };

    using K7SetPreferredAppModeType =
        K7PreferredAppMode(WINAPI*)(K7PreferredAppMode);

    static void K7SetPreferredAppMode(_In_ K7PreferredAppMode Mode)
    {
        static K7SetPreferredAppModeType Cached =
            []() -> K7SetPreferredAppModeType
        {
            HMODULE Uxtheme = ::GetModuleHandleW(L"uxtheme.dll");
            if (!Uxtheme)
            {
                return nullptr;
            }
            return reinterpret_cast<K7SetPreferredAppModeType>(
                ::GetProcAddress(Uxtheme, MAKEINTRESOURCEA(135)));
        }();
        if (Cached)
        {
            Cached(Mode);
        }
    }

    static void ApplyProcessThemePolicy(
        _In_ bool ShouldUseDarkMode)
    {
        // The process-wide uxtheme color mode must follow the possibly
        // inverted theme state instead of always following the system
        // setting. With the inverted theme enabled, AUTO would keep
        // rendering theme-drawn controls (list view items, headers, buttons)
        // with the system light mode styles while the non-theme rendering
        // paths (DWM attributes, background erasing, control color messages)
        // already use the inverted colors, resulting in unreadable
        // light-on-light or dark-on-dark content. For the non-inverted cases
        // DARK/DEFAULT produce the same results as AUTO because
        // ShouldUseDarkMode is derived from the system setting there.
        // System components which read the uxtheme ShouldAppsUseDarkMode
        // export directly (e.g. the common file dialogs) only honor the
        // forced modes, so FORCE_DARK is required on light-theme systems
        // with the inverted theme enabled.
        ::K7SetPreferredAppMode(
            ShouldUseDarkMode
                ? K7PreferredAppMode::ForceDark
                : K7PreferredAppMode::Default);
        ::MileRefreshImmersiveColorPolicyState();
        // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
        K7ThemeDebugTrace(
            L"ApplyProcessThemePolicy: dark=%d (SetPreferredAppMode + RefreshImmersiveColorPolicyState)",
            (int)ShouldUseDarkMode);
        // *** END TEMPORARY DEBUG INSTRUMENTATION ***
    }

    static HBRUSH GetDarkModeBackgroundBrush()
    {
        static HBRUSH CachedResult =
            ::CreateSolidBrush(g_DarkModeBackgroundColor);
        return CachedResult;
    }

    static HBRUSH GetDarkModeForegroundBrush()
    {
        static HBRUSH CachedResult =
            ::CreateSolidBrush(g_DarkModeForegroundColor);
        return CachedResult;
    }

    static HBRUSH GetDarkModeBorderBrush()
    {
        static HBRUSH CachedResult =
            ::CreateSolidBrush(g_DarkModeBorderColor);
        return CachedResult;
    }

    static HBRUSH GetDarkModeMenuSelectedBackgroundBrush()
    {
        static HBRUSH CachedResult =
            ::CreateSolidBrush(g_DarkModeMenuSelectedBackgroundColor);
        return CachedResult;
    }

    static bool IsStandardDynamicRangeMode()
    {
        static bool CachedResult = ([]() -> bool
        {
            bool Result = true;

            UINT32 NumPathArrayElements = 0;
            UINT32 NumModeInfoArrayElements = 0;
            if (ERROR_SUCCESS == ::GetDisplayConfigBufferSizes(
                QDC_ONLY_ACTIVE_PATHS,
                &NumPathArrayElements,
                &NumModeInfoArrayElements))
            {
                std::vector<DISPLAYCONFIG_PATH_INFO> PathArray(
                    NumPathArrayElements);
                std::vector<DISPLAYCONFIG_MODE_INFO> ModeInfoArray(
                    NumModeInfoArrayElements);
                if (ERROR_SUCCESS == ::QueryDisplayConfig(
                    QDC_ONLY_ACTIVE_PATHS,
                    &NumPathArrayElements,
                    &PathArray[0],
                    &NumModeInfoArrayElements,
                    &ModeInfoArray[0],
                    nullptr))
                {
                    for (DISPLAYCONFIG_PATH_INFO const& Path : PathArray)
                    {
                        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO AdvancedColorInfo;
                        std::memset(
                            &AdvancedColorInfo,
                            0,
                            sizeof(DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO));
                        AdvancedColorInfo.header.type =
                            DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
                        AdvancedColorInfo.header.size =
                            sizeof(DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO);
                        AdvancedColorInfo.header.adapterId =
                            Path.targetInfo.adapterId;
                        AdvancedColorInfo.header.id =
                            Path.targetInfo.id;
                        if (ERROR_SUCCESS == ::DisplayConfigGetDeviceInfo(
                            &AdvancedColorInfo.header))
                        {
                            if (AdvancedColorInfo.advancedColorEnabled)
                            {
                                Result = false;
                                break;
                            }
                        }
                    }
                }
            }

            return Result;
        }());

        return CachedResult;
    }

    static volatile bool g_GlobalInitialized = false;

    // The uxtheme preferred app mode policy applied by ApplyProcessThemePolicy
    // is process-wide, so the dark mode state consulted by the detours and
    // the window subclasses has to be process-wide too. A per-thread mirror
    // goes stale on threads which never process a theme change notification
    // and then renders with the wrong (light) colors even after the theme
    // was switched to dark.
    static volatile LONG g_ShouldAppsUseDarkMode = 0;

    static bool ShouldAppsUseDarkMode()
    {
        return (0 != g_ShouldAppsUseDarkMode);
    }

    static void SetShouldAppsUseDarkMode(_In_ bool Value)
    {
        ::InterlockedExchange(
            &g_ShouldAppsUseDarkMode,
            Value ? 1 : 0);
    }

    static LRESULT CALLBACK CallWndProcCallback(
        _In_ int nCode,
        _In_ WPARAM wParam,
        _In_ LPARAM lParam);

    struct ThreadContext
    {
    public:

        // Fields for all scenarios.
        // Should always be available if ShouldAppsUseDarkMode is true.

        HHOOK volatile WindowsHookHandle = nullptr;

        // Fields for specific scenarios.
        // May not be available, which need to be checked before use.

        bool volatile MicaBackdropAvailable = false;

    public:

        ThreadContext()
        {
            this->WindowsHookHandle = ::SetWindowsHookExW(
                WH_CALLWNDPROC,
                ::CallWndProcCallback,
                nullptr,
                ::GetCurrentThreadId());
        }

        ~ThreadContext()
        {
            if (this->WindowsHookHandle)
            {
                ::UnhookWindowsHookEx(this->WindowsHookHandle);
                this->WindowsHookHandle = nullptr;
            }
        }
    };
    thread_local ThreadContext g_ThreadContext;

    static void RefreshWindowTheme(
        _In_ HWND WindowHandle)
    {
        wchar_t ClassName[256] = {};
        if (0 != ::GetClassNameW(
            WindowHandle,
            ClassName,
            MO_ARRAY_SIZE(ClassName)))
        {
            // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
            K7ThemeDebugTrace(
                L"RefreshWindowTheme: hwnd=%08X class=%ws",
                (DWORD)(DWORD_PTR)WindowHandle,
                ClassName);
            // *** END TEMPORARY DEBUG INSTRUMENTATION ***
            // Every themed control needs WM_THEMECHANGED to reopen its theme
            // handle after the immersive color policy has been refreshed by
            // ApplyProcessThemePolicy. Controls which do not receive it keep
            // rendering with the stale theme state and can stop drawing
            // anything at all once repainted (e.g. list views and status
            // bars ending up blank after the inverted theme is applied).
            // This used to be sent only for unhandled window classes, which
            // is why only tree views refreshed correctly.
            ::SendMessageW(WindowHandle, WM_THEMECHANGED, 0, 0);

            if (0 == std::wcscmp(ClassName, WC_BUTTONW))
            {
                ::SetWindowTheme(WindowHandle, L"Explorer", nullptr);
            }
            else if (
                (0 == std::wcscmp(ClassName, WC_COMBOBOXW)) ||
                (0 == std::wcscmp(ClassName, WC_EDITW)))
            {
                // "CFD" alone is not a real theme class, so the previous
                // unconditional SetWindowTheme(L"CFD") silently failed and
                // combo boxes kept rendering with the light ComboBox class.
                // The dark variants follow the same naming convention as
                // DarkMode_Explorer. They must only be applied while the
                // inverted theme is active, otherwise freshly created
                // controls would stay dark after switching back to light.
                if (ShouldAppsUseDarkMode())
                {
                    ::SetWindowTheme(
                        WindowHandle,
                        (0 == std::wcscmp(ClassName, WC_COMBOBOXW))
                            ? L"DarkMode_CFD"
                            : L"DarkMode_Explorer",
                        nullptr);
                }
                else
                {
                    ::SetWindowTheme(WindowHandle, nullptr, nullptr);
                }
                ::MileAllowDarkModeForWindow(WindowHandle, TRUE);
            }
            else if (0 == std::wcscmp(ClassName, WC_HEADERW))
            {
                ::SetWindowTheme(WindowHandle, L"ItemsView", nullptr);
            }
            else if (0 == std::wcscmp(ClassName, WC_TREEVIEWW))
            {
                // The namespace tree inside the common file dialogs (and any
                // other tree view) keeps rendering with the light Explorer
                // theme unless the dark variant is requested explicitly.
                if (ShouldAppsUseDarkMode())
                {
                    ::SetWindowTheme(WindowHandle, L"DarkMode_Explorer", nullptr);
                    TreeView_SetBkColor(WindowHandle, g_DarkModeBackgroundColor);
                    TreeView_SetTextColor(WindowHandle, g_DarkModeForegroundColor);
                }
                else
                {
                    ::SetWindowTheme(WindowHandle, nullptr, nullptr);
                    TreeView_SetBkColor(WindowHandle, CLR_DEFAULT);
                    TreeView_SetTextColor(WindowHandle, CLR_DEFAULT);
                }
            }
            else if (0 == std::wcscmp(ClassName, WC_LISTVIEWW))
            {
                ::SetWindowTheme(WindowHandle, L"ItemsView", nullptr);

                if (ShouldAppsUseDarkMode())
                {
                    ListView_SetTextBkColor(
                        WindowHandle,
                        g_DarkModeBackgroundColor);
                    ListView_SetBkColor(
                        WindowHandle,
                        g_DarkModeBackgroundColor);
                    ListView_SetTextColor(
                        WindowHandle,
                        g_DarkModeForegroundColor);
                }
                else
                {
                    ListView_SetTextBkColor(
                        WindowHandle,
                        g_LightModeBackgroundColor);
                    ListView_SetBkColor(
                        WindowHandle,
                        g_LightModeBackgroundColor);
                    ListView_SetTextColor(
                        WindowHandle,
                        g_LightModeForegroundColor);
                }
            }
            else if (0 == std::wcscmp(ClassName, STATUSCLASSNAMEW))
            {
                ::SetWindowLongW(
                    WindowHandle,
                    GWL_EXSTYLE,
                    ::GetWindowLongW(
                        WindowHandle,
                        GWL_EXSTYLE) | WS_EX_COMPOSITED);
            }
            else if (0 == std::wcscmp(ClassName, WC_TABCONTROLW))
            {
                ::SetWindowLongW(
                    WindowHandle,
                    GWL_EXSTYLE,
                    ::GetWindowLongW(
                        WindowHandle,
                        GWL_EXSTYLE) | WS_EX_COMPOSITED);
            }
            else
            {
                if (0 == std::wcscmp(ClassName, TOOLBARCLASSNAMEW))
                {
                    // make it double bufferred
                    ::SetWindowLongW(
                        WindowHandle,
                        GWL_EXSTYLE,
                        ::GetWindowLongW(
                            WindowHandle,
                            GWL_EXSTYLE) | WS_EX_COMPOSITED);

                    COLORSCHEME ColorScheme;
                    ColorScheme.dwSize = sizeof(COLORSCHEME);
                    ColorScheme.clrBtnHighlight = CLR_DEFAULT;
                    ColorScheme.clrBtnShadow = CLR_DEFAULT;
                    if (ShouldAppsUseDarkMode())
                    {
                        ColorScheme.clrBtnHighlight = g_DarkModeBackgroundColor;
                        ColorScheme.clrBtnShadow = g_DarkModeBackgroundColor;
                    }
                    ::SendMessageW(
                        WindowHandle,
                        TB_SETCOLORSCHEME,
                        0,
                        reinterpret_cast<LPARAM>(&ColorScheme));
                }
            }
        }
    }

    static void CALLBACK K7UserWinEventProc(
        _In_opt_ HWINEVENTHOOK WinEventHook,
        _In_ DWORD WinEvent,
        _In_opt_ HWND WindowHandle,
        _In_ LONG ObjectId,
        _In_ LONG ChildId,
        _In_ DWORD EventThreadId,
        _In_ DWORD EventTime)
    {
        UNREFERENCED_PARAMETER(WinEventHook);
        UNREFERENCED_PARAMETER(EventThreadId);
        UNREFERENCED_PARAMETER(EventTime);

        // Apply the class-specific theme settings (Explorer buttons, CFD
        // combo boxes and edit controls, ItemsView headers and list views,
        // composited status bars) to every window as soon as it is created.
        // The in-session theme switch covers existing windows through
        // K7UserRefreshTheme, but freshly created controls (e.g. after a
        // restart with the inverted theme already enabled, or dialogs opened
        // later on) otherwise keep using their default theme classes and
        // render with light theme data.
        if (EVENT_OBJECT_CREATE != WinEvent ||
            OBJID_WINDOW != ObjectId ||
            0 != ChildId ||
            !WindowHandle ||
            !g_GlobalInitialized)
        {
            return;
        }

        wchar_t ClassName[256] = {};
        if (0 == ::GetClassNameW(
            WindowHandle,
            ClassName,
            MO_ARRAY_SIZE(ClassName)))
        {
            return;
        }

        // Only the classes which RefreshWindowTheme has special handling
        // for need the call; skip everything else to keep the hook cheap.
        if (0 == std::wcscmp(ClassName, WC_BUTTONW) ||
            0 == std::wcscmp(ClassName, WC_COMBOBOXW) ||
            0 == std::wcscmp(ClassName, WC_EDITW) ||
            0 == std::wcscmp(ClassName, WC_HEADERW) ||
            0 == std::wcscmp(ClassName, WC_LISTVIEWW) ||
            0 == std::wcscmp(ClassName, STATUSCLASSNAMEW) ||
            0 == std::wcscmp(ClassName, WC_TABCONTROLW) ||
            0 == std::wcscmp(ClassName, TOOLBARCLASSNAMEW))
        {
            ::RefreshWindowTheme(WindowHandle);
        }
    }

    static bool IsFileManagerWindowClassName(
        _In_ LPCWSTR ClassName)
    {
        return (0 == std::wcscmp(ClassName, L"NanaZip.Modern.FileManager"));
    }

    static bool IsFileManagerPanelWindowClassName(
        _In_ LPCWSTR ClassName)
    {
        return (0 == std::wcscmp(ClassName, L"NanaZip::Panel"));
    }

    static bool IsFileManagerWindow(
        _In_ HWND WindowHandle)
    {
        wchar_t ClassName[256] = {};
        if (0 != ::GetClassNameW(
            WindowHandle,
            ClassName,
            MO_ARRAY_SIZE(ClassName)))
        {
            return ::IsFileManagerWindowClassName(ClassName);
        }

        return false;
    }

    LRESULT CALLBACK WindowSubclassCallback(
        _In_ HWND hWnd,
        _In_ UINT uMsg,
        _In_ WPARAM wParam,
        _In_ LPARAM lParam,
        _In_ UINT_PTR uIdSubclass,
        _In_ DWORD_PTR dwRefData)
    {
        UNREFERENCED_PARAMETER(uIdSubclass);
        UNREFERENCED_PARAMETER(dwRefData);

        switch (uMsg)
        {
        // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
        case WM_ACTIVATE:
        {
            K7ThemeDebugTrace(
                L"WM_ACTIVATE: hwnd=%08X wp=%08X",
                (DWORD)(DWORD_PTR)hWnd,
                (DWORD)(DWORD_PTR)wParam);
            break;
        }
        case WM_THEMECHANGED:
        {
            K7ThemeDebugTrace(
                L"WM_THEMECHANGED(recv): hwnd=%08X",
                (DWORD)(DWORD_PTR)hWnd);
            break;
        }
        case WM_DESTROY:
        {
            K7ThemeDebugTrace(
                L"WM_DESTROY: hwnd=%08X",
                (DWORD)(DWORD_PTR)hWnd);
            break;
        }
        // *** END TEMPORARY DEBUG INSTRUMENTATION ***
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
        case WM_CTLCOLORDLG:
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
        {
            // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
            {
                wchar_t ParentClassName[256] = {};
                ::GetClassNameW(hWnd, ParentClassName, 256);
                K7ThemeDebugTrace(
                    L"CTLCOLOR: msg=%08X parent=%08X (%s) child=%08X dark=%d",
                    (unsigned)uMsg,
                    (DWORD)(DWORD_PTR)hWnd,
                    ParentClassName,
                    (DWORD)(DWORD_PTR)lParam,
                    (int)ShouldAppsUseDarkMode());
            }
            // *** END TEMPORARY DEBUG INSTRUMENTATION ***
            HDC DeviceContextHandle = reinterpret_cast<HDC>(wParam);
            if (DeviceContextHandle)
            {
                ::SetTextColor(
                    DeviceContextHandle,
                    ShouldAppsUseDarkMode() ?
                        g_DarkModeForegroundColor :
                        g_LightModeForegroundColor);
                ::SetBkColor(
                    DeviceContextHandle,
                    ShouldAppsUseDarkMode() ?
                        g_DarkModeBackgroundColor :
                        g_LightModeBackgroundColor);
            }

            return reinterpret_cast<INT_PTR>(
                ShouldAppsUseDarkMode() ?
                    ::GetDarkModeBackgroundBrush() :
                    ::GetStockObject(WHITE_BRUSH));
        }
        default:
            break;
        }

        LRESULT Result = ::DefSubclassProc(
            hWnd,
            uMsg,
            wParam,
            lParam);

        switch (uMsg)
        {
        case WM_SETTINGCHANGE:
        {
            LPCTSTR Section = reinterpret_cast<LPCTSTR>(lParam);

            // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
            K7ThemeDebugTrace(
                L"WM_SETTINGCHANGE(recv): hwnd=%08X section=%ws",
                (DWORD)(DWORD_PTR)hWnd,
                Section ? Section : L"(null)");
            // *** END TEMPORARY DEBUG INSTRUMENTATION ***

            if (Section && 0 == std::wcscmp(Section, L"ImmersiveColorSet"))
            {
                ::MileRefreshImmersiveColorPolicyState();

                bool ShouldUseDarkMode = ::ComputeShouldAppsUseDarkMode();
                SetShouldAppsUseDarkMode(ShouldUseDarkMode);

                ::ApplyProcessThemePolicy(ShouldUseDarkMode);

                ::MileEnableImmersiveDarkModeForWindow(
                    hWnd,
                    ShouldUseDarkMode);

                bool ShouldExtendFrame = (
                    ShouldUseDarkMode &&
                    ::IsStandardDynamicRangeMode() &&
                    g_ThreadContext.MicaBackdropAvailable);
                MARGINS Margins = {};
                if (ShouldExtendFrame)
                {
                    Margins = { -1 };
                }
                else if (::IsFileManagerWindow(hWnd))
                {
                    UINT DpiValue = ::GetDpiForWindow(hWnd);
                    Margins.cyTopHeight =
                        ::MulDiv(84, DpiValue, USER_DEFAULT_SCREEN_DPI);
                    Margins.cyBottomHeight =
                        ::MulDiv(32, DpiValue, USER_DEFAULT_SCREEN_DPI);
                }
                ::DwmExtendFrameIntoClientArea(hWnd, &Margins);

                ::EnumChildWindows(
                    hWnd,
                    [](
                        _In_ HWND hWnd,
                        _In_ LPARAM lParam) -> BOOL
                {
                    UNREFERENCED_PARAMETER(lParam);
                    ::RefreshWindowTheme(hWnd);
                    return TRUE;
                },
                    0);

                ::InvalidateRect(hWnd, nullptr, TRUE);
            }

            break;
        }
        case WM_INITDIALOG:
        case WM_CREATE:
        {
            // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
            K7ThemeDebugTrace(
                L"WM_INITDIALOG/WM_CREATE: hwnd=%08X msg=%u",
                (DWORD)(DWORD_PTR)hWnd,
                uMsg);
            // *** END TEMPORARY DEBUG INSTRUMENTATION ***
            ::MileAllowDarkModeForWindow(
                hWnd,
                TRUE);

            ::MileSetWindowSystemBackdropTypeAttribute(
                hWnd,
                MILE_WINDOW_SYSTEM_BACKDROP_TYPE_MICA);

            g_ThreadContext.MicaBackdropAvailable =
                (S_OK == ::MileEnableImmersiveDarkModeForWindow(
                    hWnd,
                    ShouldAppsUseDarkMode()));

            bool ShouldExtendFrame = (
                ShouldAppsUseDarkMode() &&
                ::IsStandardDynamicRangeMode() &&
                g_ThreadContext.MicaBackdropAvailable);
            if (ShouldExtendFrame)
            {
                MARGINS Margins = { -1 };
                ::DwmExtendFrameIntoClientArea(hWnd, &Margins);
            }
            else if (::IsFileManagerWindow(hWnd))
            {
                UINT DpiValue = ::GetDpiForWindow(hWnd);

                MARGINS Margins = {};
                Margins.cyTopHeight =
                    ::MulDiv(84, DpiValue, USER_DEFAULT_SCREEN_DPI);
                Margins.cyBottomHeight =
                    ::MulDiv(32, DpiValue, USER_DEFAULT_SCREEN_DPI);
                ::DwmExtendFrameIntoClientArea(hWnd, &Margins);
            }

            ::RefreshWindowTheme(hWnd);

            wchar_t ClassName[256] = {};
            if (0 != ::GetClassNameW(
                hWnd,
                ClassName,
                MO_ARRAY_SIZE(ClassName)))
            {
                if (0 == std::wcscmp(ClassName, WC_TABCONTROLW))
                {
                    ::SetWindowLongPtrW(
                        hWnd,
                        GWL_STYLE,
                        (::GetWindowLongPtrW(hWnd, GWL_STYLE) & ~TCS_BUTTONS)
                        | TCS_TABS);
                    ::SetWindowTheme(hWnd, nullptr, nullptr);
                }
            }

            break;
        }
        case WM_ERASEBKGND:
        {
            wchar_t ClassName[256] = {};
            if (0 != ::GetClassNameW(
                hWnd,
                ClassName,
                MO_ARRAY_SIZE(ClassName)))
            {
                if (ShouldAppsUseDarkMode() &&
                    0 == std::wcscmp(ClassName, STATUSCLASSNAMEW))
                {
                    RECT ClientArea = {};
                    if (::GetClientRect(hWnd, &ClientArea))
                    {
                        ::FillRect(
                            reinterpret_cast<HDC>(wParam),
                            &ClientArea,
                            reinterpret_cast<HBRUSH>(
                                ::GetStockObject(BLACK_BRUSH)));
                        return TRUE;
                    }
                }

                if (::IsFileManagerWindowClassName(ClassName) ||
                    ::IsFileManagerPanelWindowClassName(ClassName))
                {
                    RECT ClientArea = {};
                    if (::GetClientRect(hWnd, &ClientArea))
                    {
                        ::FillRect(
                            reinterpret_cast<HDC>(wParam),
                            &ClientArea,
                            reinterpret_cast<HBRUSH>(
                                ::GetStockObject(
                                    ShouldAppsUseDarkMode()
                                    ? BLACK_BRUSH
                                    : WHITE_BRUSH)));
                        return TRUE;
                    }
                }
            }

            break;
        }
        case WM_DPICHANGED:
        {
            bool ShouldExtendFrame = (
                ShouldAppsUseDarkMode() &&
                ::IsStandardDynamicRangeMode() &&
                g_ThreadContext.MicaBackdropAvailable);
            if (!ShouldExtendFrame && ::IsFileManagerWindow(hWnd))
            {
                UINT DpiValue = ::GetDpiForWindow(hWnd);

                MARGINS Margins = {};
                Margins.cyTopHeight =
                    ::MulDiv(84, DpiValue, USER_DEFAULT_SCREEN_DPI);
                Margins.cyBottomHeight =
                    ::MulDiv(32, DpiValue, USER_DEFAULT_SCREEN_DPI);
                ::DwmExtendFrameIntoClientArea(hWnd, &Margins);
            }

            break;
        }
        default:
            break;
        }

        if (ShouldAppsUseDarkMode() && ::GetMenu(hWnd))
        {
            if (WM_UAHDRAWMENU == uMsg)
            {
                PUAHMENU UahMenu = reinterpret_cast<PUAHMENU>(lParam);
                if (UahMenu)
                {
                    MENUBARINFO MenuBarInfo;
                    MenuBarInfo.cbSize = sizeof(MENUBARINFO);
                    if (::GetMenuBarInfo(hWnd, OBJID_MENU, 0, &MenuBarInfo))
                    {
                        RECT WindowRect = {};
                        ::GetWindowRect(hWnd, &WindowRect);

                        RECT MenuRect = MenuBarInfo.rcBar;
                        ::OffsetRect(
                            &MenuRect,
                            -WindowRect.left,
                            -WindowRect.top);

                        ::FillRect(
                            UahMenu->hdc,
                            &MenuRect,
                            ::GetDarkModeBackgroundBrush());
                    }
                }

                return TRUE;
            }
            else if (WM_UAHDRAWMENUITEM == uMsg)
            {
                PUAHDRAWMENUITEM UahDrawMenuItem =
                    reinterpret_cast<PUAHDRAWMENUITEM>(lParam);
                if (UahDrawMenuItem)
                {
                    PDRAWITEMSTRUCT DrawItemStruct = &UahDrawMenuItem->dis;
                    if (ODT_MENU == DrawItemStruct->CtlType)
                    {
                        wchar_t Buffer[256] = {};
                        MENUITEMINFOW MenuItemInfo;
                        MenuItemInfo.cbSize = sizeof(MENUITEMINFOW);
                        MenuItemInfo.fMask = MIIM_STRING;
                        MenuItemInfo.dwTypeData = Buffer;
                        MenuItemInfo.cch = MO_ARRAY_SIZE(Buffer) - 1;
                        if (::GetMenuItemInfoW(
                            UahDrawMenuItem->um.hmenu,
                            UahDrawMenuItem->umi.iPosition,
                            TRUE,
                            &MenuItemInfo))
                        {
                            int StateId = 0;
                            COLORREF TextColor = g_DarkModeForegroundColor;
                            HBRUSH BackgroundBrush =
                                ::GetDarkModeBackgroundBrush();
                            if (DrawItemStruct->itemState & ODS_INACTIVE)
                            {
                                StateId = MBI_DISABLED;
                                TextColor = RGB(109, 109, 109);
                            }
                            else if ((DrawItemStruct->itemState & ODS_GRAYED) &&
                                (DrawItemStruct->itemState & ODS_HOTLIGHT))
                            {
                                StateId = MBI_DISABLEDHOT;
                            }
                            else if (DrawItemStruct->itemState & ODS_GRAYED)
                            {
                                StateId = MBI_DISABLED;
                                TextColor = RGB(109, 109, 109);
                            }
                            else if (DrawItemStruct->itemState
                                & (ODS_HOTLIGHT | ODS_SELECTED))
                            {
                                StateId = MBI_HOT;
                                BackgroundBrush =
                                    ::GetDarkModeMenuSelectedBackgroundBrush();
                            }
                            else
                            {
                                StateId = MBI_NORMAL;
                            }

                            ::FillRect(
                                DrawItemStruct->hDC,
                                &DrawItemStruct->rcItem,
                                BackgroundBrush);

                            // We have to specify the text colour explicitly as
                            // by default black would be used, making the menu
                            // label unreadable on the (almost) black
                            // background.
                            DTTOPTS TextOptions = {};
                            TextOptions.dwSize = sizeof(DTTOPTS);
                            TextOptions.dwFlags = DTT_TEXTCOLOR;
                            TextOptions.crText = TextColor;

                            DWORD TextFlags =
                                DT_CENTER | DT_VCENTER | DT_SINGLELINE;
                            if (DrawItemStruct->itemState & ODS_NOACCEL)
                            {
                                TextFlags |= DT_HIDEPREFIX;
                            }

                            HTHEME ThemeHandle = ::OpenThemeData(hWnd, L"Menu");
                            if (ThemeHandle)
                            {
                                ::DrawThemeTextEx(
                                    ThemeHandle,
                                    UahDrawMenuItem->um.hdc,
                                    MENU_BARITEM,
                                    StateId,
                                    Buffer,
                                    static_cast<int>(std::wcslen(Buffer)),
                                    TextFlags,
                                    &DrawItemStruct->rcItem,
                                    &TextOptions);

                                ::CloseThemeData(ThemeHandle);
                            }
                        }
                    }
                }

                return TRUE;
            }
            else if (WM_NCPAINT == uMsg || WM_NCACTIVATE == uMsg)
            {
                MENUBARINFO MenuBarInfo;
                MenuBarInfo.cbSize = sizeof(MENUBARINFO);
                if (::GetMenuBarInfo(hWnd, OBJID_MENU, 0, &MenuBarInfo))
                {
                    RECT ClientRect = {};
                    ::GetClientRect(hWnd, &ClientRect);

                    ::MapWindowPoints(
                        hWnd,
                        nullptr,
                        reinterpret_cast<PPOINT>(&ClientRect),
                        2);

                    RECT WindowRect = {};
                    ::GetWindowRect(hWnd, &WindowRect);

                    ::OffsetRect(
                        &ClientRect,
                        -WindowRect.left,
                        -WindowRect.top);

                    RECT AnnoyingLineRect = ClientRect;
                    AnnoyingLineRect.bottom = AnnoyingLineRect.top;
                    --AnnoyingLineRect.top;

                    HDC DeviceContextHandle = ::GetWindowDC(hWnd);
                    if (DeviceContextHandle)
                    {
                        ::FillRect(
                            DeviceContextHandle,
                            &AnnoyingLineRect,
                            ::GetDarkModeBackgroundBrush());

                        ::ReleaseDC(hWnd, DeviceContextHandle);
                    }
                }
            }
        }

        return Result;
    }

    static std::wstring GetAssociatedModuleNameFromWindowHandle(
        _In_ HWND WindowHandle)
    {
        // 32767 is the maximum path length without the terminating null
        // character.
        std::wstring Path(32767, L'\0');
        Path.resize(::GetWindowModuleFileNameW(
            WindowHandle, &Path[0], static_cast<UINT>(Path.size())));
        wchar_t* LastBackslash = std::wcsrchr(Path.data(), L'\\');
        return LastBackslash ? std::wstring(LastBackslash + 1) : Path;
    }

    static bool IsModernizedWindow(
        _In_ HWND WindowHandle)
    {
        std::wstring ModuleName =
            ::GetAssociatedModuleNameFromWindowHandle(WindowHandle);
        if (!::_wcsicmp(ModuleName.c_str(), L"combase.dll") ||
            !::_wcsicmp(ModuleName.c_str(), L"CoreMessaging.dll") ||
            !::_wcsicmp(ModuleName.c_str(), L"InputHost.dll") ||
            !::_wcsicmp(ModuleName.c_str(), L"Windows.UI.dll") ||
            !::_wcsicmp(ModuleName.c_str(), L"Windows.UI.Xaml.dll"))
        {
            return true;
        }

        wchar_t ClassName[256] = {};
        if (0 != ::GetClassNameW(
            WindowHandle,
            ClassName,
            MO_ARRAY_SIZE(ClassName)))
        {
            if (std::wcsstr(ClassName, L"Windows.UI.") ||
                std::wcsstr(ClassName, L"Mile.Xaml.") ||
                std::wcsstr(ClassName, L"Xaml_WindowedPopupClass"))
            {
                return true;
            }
        }

        return false;
    }

    static LRESULT CALLBACK CallWndProcCallback(
        _In_ int nCode,
        _In_ WPARAM wParam,
        _In_ LPARAM lParam)
    {
        if (g_GlobalInitialized && nCode == HC_ACTION)
        {
            PCWPSTRUCT WndProcStruct =
                reinterpret_cast<PCWPSTRUCT>(lParam);

            switch (WndProcStruct->message)
            {
            case WM_CREATE:
            case WM_INITDIALOG:
            {
                if (!::IsModernizedWindow(WndProcStruct->hwnd))
                {
                    ::SetWindowSubclass(
                        WndProcStruct->hwnd,
                        ::WindowSubclassCallback,
                        0,
                        0);
                }
                break;
            }
            default:
                break;
            }
        }

        return ::CallNextHookEx(
            nullptr,
            nCode,
            wParam,
            lParam);
    }

    namespace FunctionTypes
    {
        enum
        {
            GetSysColor,
            GetSysColorBrush,
            GetThemeColor,
            DrawThemeText,
            DrawThemeTextEx,
            DrawThemeBackground,
            DrawThemeBackgroundEx,
            OpenNcThemeData,
            GetThemeClass,
            GetThemeSysColor,

            MaximumFunction
            };
    }

    struct FunctionItem
    {
        PVOID Original;
        PVOID Detoured;
    };

    FunctionItem g_FunctionTable[FunctionTypes::MaximumFunction];

    static DWORD WINAPI OriginalGetSysColor(
        _In_ int nIndex)
    {
        using FunctionType = decltype(::GetSysColor)*;
        FunctionType FunctionAddress = reinterpret_cast<FunctionType>(
            g_FunctionTable[FunctionTypes::GetSysColor].Original);
        if (!FunctionAddress)
        {
            return 0;
        }
        return FunctionAddress(nIndex);
    }

    static HBRUSH WINAPI OriginalGetSysColorBrush(
        _In_ int nIndex)
    {
        using FunctionType = decltype(::GetSysColorBrush)*;
        FunctionType FunctionAddress = reinterpret_cast<FunctionType>(
            g_FunctionTable[FunctionTypes::GetSysColorBrush].Original);
        if (!FunctionAddress)
        {
            return nullptr;
        }
        return FunctionAddress(nIndex);
    }

    static HRESULT WINAPI OriginalGetThemeColor(
        _In_ HTHEME hTheme,
        _In_ int iPartId,
        _In_ int iStateId,
        _In_ int iPropId,
        _Out_ COLORREF* pColor)
    {
        using FunctionType = decltype(::GetThemeColor)*;
        FunctionType FunctionAddress = reinterpret_cast<FunctionType>(
            g_FunctionTable[FunctionTypes::GetThemeColor].Original);
        if (!FunctionAddress)
        {
            return E_NOINTERFACE;
        }
        return FunctionAddress(
            hTheme,
            iPartId,
            iStateId,
            iPropId,
            pColor);
    }

    static HRESULT WINAPI OriginalDrawThemeText(
        _In_ HTHEME hTheme,
        _In_ HDC hdc,
        _In_ int iPartId,
        _In_ int iStateId,
        _In_ LPCWSTR pszText,
        _In_ int cchText,
        _In_ DWORD dwTextFlags,
        _In_ DWORD dwTextFlags2,
        _In_ LPCRECT pRect)
    {
        using FunctionType = decltype(::DrawThemeText)*;
        FunctionType FunctionAddress = reinterpret_cast<FunctionType>(
            g_FunctionTable[FunctionTypes::DrawThemeText].Original);
        if (!FunctionAddress)
        {
            return E_NOINTERFACE;
        }
        return FunctionAddress(
            hTheme,
            hdc,
            iPartId,
            iStateId,
            pszText,
            cchText,
            dwTextFlags,
            dwTextFlags2,
            pRect);
    }

    static HRESULT WINAPI OriginalDrawThemeTextEx(
        _In_ HTHEME hTheme,
        _In_ HDC hdc,
        _In_ int iPartId,
        _In_ int iStateId,
        _In_ LPCWSTR pszText,
        _In_ int cchText,
        _In_ DWORD dwTextFlags,
        _In_ LPRECT lprc,
        _In_opt_ const DTTOPTS* pOptions)
    {
        using FunctionType = decltype(::DrawThemeTextEx)*;
        FunctionType FunctionAddress = reinterpret_cast<FunctionType>(
            g_FunctionTable[FunctionTypes::DrawThemeTextEx].Original);
        if (!FunctionAddress)
        {
            return E_NOINTERFACE;
        }
        return FunctionAddress(
            hTheme,
            hdc,
            iPartId,
            iStateId,
            pszText,
            cchText,
            dwTextFlags,
            lprc,
            pOptions);
    }

    static HRESULT WINAPI OriginalDrawThemeBackground(
        _In_ HTHEME hTheme,
        _In_ HDC hdc,
        _In_ int iPartId,
        _In_ int iStateId,
        _In_ LPCRECT pRect,
        _In_opt_ LPCRECT pClipRect)
    {
        using FunctionType = decltype(::DrawThemeBackground)*;
        FunctionType FunctionAddress = reinterpret_cast<FunctionType>(
            g_FunctionTable[FunctionTypes::DrawThemeBackground].Original);
        if (!FunctionAddress)
        {
            return E_NOINTERFACE;
        }
        return FunctionAddress(
            hTheme,
            hdc,
            iPartId,
            iStateId,
            pRect,
            pClipRect);
    }

    static HRESULT WINAPI OriginalDrawThemeBackgroundEx(
        _In_ HTHEME hTheme,
        _In_ HDC hdc,
        _In_ int iPartId,
        _In_ int iStateId,
        _In_ LPCRECT pRect,
        _In_opt_ const DTBGOPTS* pOptions)
    {
        using FunctionType = decltype(::DrawThemeBackgroundEx)*;
        FunctionType FunctionAddress = reinterpret_cast<FunctionType>(
            g_FunctionTable[FunctionTypes::DrawThemeBackgroundEx].Original);
        if (!FunctionAddress)
        {
            return E_NOINTERFACE;
        }
        return FunctionAddress(
            hTheme,
            hdc,
            iPartId,
            iStateId,
            pRect,
            pOptions);
    }

    static HTHEME WINAPI OriginalOpenNcThemeData(
        _In_opt_ HWND hwnd,
        _In_ LPCWSTR pszClassList)
    {
        using FunctionType = decltype(::OpenNcThemeData)*;
        FunctionType FunctionAddress = reinterpret_cast<FunctionType>(
            g_FunctionTable[FunctionTypes::OpenNcThemeData].Original);
        if (!FunctionAddress)
        {
            return nullptr;
        }
        return FunctionAddress(hwnd, pszClassList);
    }

    static HRESULT WINAPI OriginalGetThemeClass(
        _In_ HTHEME hTheme,
        _Out_ LPWSTR pszClassName,
        _In_ int cchClassName)
    {
        using FunctionType = decltype(::GetThemeClass)*;
        FunctionType FunctionAddress = reinterpret_cast<FunctionType>(
            g_FunctionTable[FunctionTypes::GetThemeClass].Original);
        if (!FunctionAddress)
        {
            return E_NOINTERFACE;
        }
        return FunctionAddress(
            hTheme,
            pszClassName,
            cchClassName);
    }

    // Resolves the text color which has to be used for theme text on a
    // system forced into the inverted (dark) mode. The light theme data
    // handed out by uxtheme would otherwise provide a dark text color which
    // is unreadable on the dark backgrounds we draw ourselves.
    static COLORREF GetDarkModeThemeTextColor(
        _In_ bool HasClassName,
        _In_reads_(256) LPCWSTR ClassName,
        _In_ int iPartId,
        _In_ int iStateId)
    {
        if (HasClassName && 0 == ::_wcsicmp(ClassName, L"Explorer"))
        {
            if ((BP_PUSHBUTTON == iPartId && PBS_DISABLED == iStateId) ||
                (BP_CHECKBOX == iPartId &&
                    (CBS_UNCHECKEDDISABLED == iStateId ||
                        CBS_CHECKEDDISABLED == iStateId)) ||
                (BP_RADIOBUTTON == iPartId &&
                    (RBS_UNCHECKEDDISABLED == iStateId ||
                        RBS_CHECKEDDISABLED == iStateId)))
            {
                return RGB(109, 109, 109);
            }
        }

        return g_DarkModeForegroundColor;
    }

    static DWORD WINAPI DetouredGetSysColor(
        _In_ int nIndex)
    {
        if (!g_GlobalInitialized || !ShouldAppsUseDarkMode())
        {
            return ::OriginalGetSysColor(nIndex);
        }

        switch (nIndex)
        {
        case COLOR_WINDOW:
        case COLOR_BTNFACE:
            // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
            K7ThemeDebugTrace(
                L"GetSysColor(%d) -> dark",
                nIndex);
            // *** END TEMPORARY DEBUG INSTRUMENTATION ***
            return g_DarkModeBackgroundColor;
        case COLOR_WINDOWTEXT:
        case COLOR_BTNTEXT:
            return g_DarkModeForegroundColor;
        default:
            return ::OriginalGetSysColor(nIndex);
        }
    }

    static HBRUSH WINAPI DetouredGetSysColorBrush(
        _In_ int nIndex)
    {
        if (!g_GlobalInitialized || !ShouldAppsUseDarkMode())
        {
            return ::OriginalGetSysColorBrush(nIndex);
        }

        switch (nIndex)
        {
        case COLOR_BTNFACE:
            return ::GetDarkModeBackgroundBrush();
        case COLOR_BTNTEXT:
            return ::GetDarkModeForegroundBrush();
        default:
            return ::OriginalGetSysColorBrush(nIndex);
        }
    }

    static COLORREF WINAPI OriginalGetThemeSysColor(
        _In_ HTHEME hTheme,
        _In_ int iColorId)
    {
        using FunctionType = decltype(::GetThemeSysColor)*;
        auto Original = reinterpret_cast<FunctionType>(
            g_FunctionTable[FunctionTypes::GetThemeSysColor].Original);
        return Original(hTheme, iColorId);
    }

    // The DirectUI content inside common file dialogs draws its list
    // background with GetThemeSysColor instead of GetSysColor, which left
    // the file list white in dark mode. Route the same system colors to the
    // dark palette here.
    static COLORREF WINAPI DetouredGetThemeSysColor(
        _In_ HTHEME hTheme,
        _In_ int iColorId)
    {
        if (!g_GlobalInitialized || !ShouldAppsUseDarkMode())
        {
            return ::OriginalGetThemeSysColor(hTheme, iColorId);
        }

        switch (iColorId)
        {
        case COLOR_WINDOW:
        case COLOR_BTNFACE:
            // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
            K7ThemeDebugTrace(
                L"GetThemeSysColor(%d) -> dark",
                iColorId);
            // *** END TEMPORARY DEBUG INSTRUMENTATION ***
            return g_DarkModeBackgroundColor;
        case COLOR_WINDOWTEXT:
        case COLOR_BTNTEXT:
            return g_DarkModeForegroundColor;
        default:
            return ::OriginalGetThemeSysColor(hTheme, iColorId);
        }
    }

    // Forward declaration: DetouredGetThemeColor resolves theme class names
    // through this helper, whose definition appears further down next to
    // the other Original* wrappers.
    static bool IsThemeClass(
        _In_ HTHEME hTheme,
        _In_z_ LPCWSTR ExpectedClassName);

    static HRESULT WINAPI DetouredGetThemeColor(
        _In_ HTHEME hTheme,
        _In_ int iPartId,
        _In_ int iStateId,
        _In_ int iPropId,
        _Out_ COLORREF* pColor)
    {
        if (!g_GlobalInitialized || !ShouldAppsUseDarkMode())
        {
            return ::OriginalGetThemeColor(
                hTheme,
                iPartId,
                iStateId,
                iPropId,
                pColor);
        }

        HRESULT hr = ::OriginalGetThemeColor(
            hTheme,
            iPartId,
            iStateId,
            iPropId,
            pColor);
        if (S_OK != hr)
        {
            return hr;
        }

        if (TMT_TEXTCOLOR == iPropId)
        {
            wchar_t ClassName[256] = {};
            if (FAILED(::OriginalGetThemeClass(
                hTheme,
                ClassName,
                MO_ARRAY_SIZE(ClassName))))
            {
                *pColor = g_DarkModeForegroundColor;
            }
            else if (0 == std::wcsncmp(ClassName, L"DarkMode_", 9))
            {
                // Native dark theme data opened through the DarkMode_
                // class redirect: keep the native color untouched.
            }
            else if (0 != ::_wcsicmp(ClassName, L"Menu"))
            {
                // On a light system forced into dark mode, uxtheme hands out
                // light theme data, so callers resolving the theme text color
                // directly get a dark color which is unreadable on the dark
                // backgrounds we draw. Mirror the native dark theme behavior
                // by providing white. Menus are excluded because their
                // backgrounds stay light there.
                *pColor = g_DarkModeForegroundColor;
            }
        }
        else if (TMT_FILLCOLOR == iPropId)
        {
            wchar_t FillClassName[256] = {};
            bool FillExempt = false;
            if (SUCCEEDED(::OriginalGetThemeClass(
                hTheme,
                FillClassName,
                MO_ARRAY_SIZE(FillClassName))))
            {
                // Native dark theme data keeps its own (already dark) fill
                // colors, and menus keep the system-provided light
                // background on a light system forced into dark mode.
                FillExempt =
                    (0 == std::wcsncmp(FillClassName, L"DarkMode_", 9)) ||
                    (0 == ::_wcsicmp(FillClassName, L"Menu"));
            }

            // DirectUI surfaces inside the common file dialogs (file list,
            // namespace tree) resolve their background via GetThemeColor
            // TMT_FILLCOLOR with various class names and never call
            // DrawThemeBackground for it (verified with the debug trace).
            // Provide the dark background fill color for all of them.
            // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
            K7ThemeDebugTrace(
                L"GetThemeColor FILLCOLOR class=%ws part=%d state=%d exempt=%d",
                FillClassName,
                iPartId,
                iStateId,
                (int)FillExempt);
            // *** END TEMPORARY DEBUG INSTRUMENTATION ***
            if (!FillExempt)
            {
                *pColor = g_DarkModeBackgroundColor;
            }
        }

        return S_OK;
    }

    static bool IsToolbarThemeText(
        _In_ HTHEME hTheme)
    {
        wchar_t ClassName[256] = {};
        if (FAILED(::OriginalGetThemeClass(
            hTheme,
            ClassName,
            MO_ARRAY_SIZE(ClassName))))
        {
            return false;
        }

        // Native dark theme data opened through the DarkMode_ class
        // redirect keeps its own (already light) text color, so no forcing
        // is needed there either.
        if (0 == std::wcsncmp(ClassName, L"DarkMode_", 9))
        {
            return true;
        }

        // Menus keep the system-provided (light) background on a light
        // system forced into dark mode, so forcing white text on them
        // produced white-on-white menus. The toolbar is no longer exempt:
        // its plates are drawn dark by our DrawThemeBackground handler, so
        // its text has to turn white too.
        return (0 == ::_wcsicmp(ClassName, L"Menu"));
    }

    static HRESULT WINAPI DetouredDrawThemeText(
        _In_ HTHEME hTheme,
        _In_ HDC hdc,
        _In_ int iPartId,
        _In_ int iStateId,
        _In_ LPCWSTR pszText,
        _In_ int cchText,
        _In_ DWORD dwTextFlags,
        _In_ DWORD dwTextFlags2,
        _In_ LPCRECT pRect)
    {
        if (!g_GlobalInitialized || !ShouldAppsUseDarkMode())
        {
            return ::OriginalDrawThemeText(
                hTheme,
                hdc,
                iPartId,
                iStateId,
                pszText,
                cchText,
                dwTextFlags,
                dwTextFlags2,
                pRect);
        }

        if (IsToolbarThemeText(hTheme))
        {
            return ::OriginalDrawThemeText(
                hTheme,
                hdc,
                iPartId,
                iStateId,
                pszText,
                cchText,
                dwTextFlags,
                dwTextFlags2,
                pRect);
        }

        wchar_t ClassName[256] = {};
        bool HasClassName = SUCCEEDED(::OriginalGetThemeClass(
            hTheme,
            ClassName,
            MO_ARRAY_SIZE(ClassName)));

        DTTOPTS TextOptions = {};
        TextOptions.dwSize = sizeof(DTTOPTS);
        TextOptions.dwFlags = DTT_TEXTCOLOR;
        TextOptions.crText = GetDarkModeThemeTextColor(
            HasClassName,
            ClassName,
            iPartId,
            iStateId);

        RECT Rect = *pRect;

        return ::OriginalDrawThemeTextEx(
            hTheme,
            hdc,
            iPartId,
            iStateId,
            pszText,
            cchText,
            dwTextFlags,
            &Rect,
            &TextOptions);
    }

    static HRESULT WINAPI DetouredDrawThemeTextEx(
        _In_ HTHEME hTheme,
        _In_ HDC hdc,
        _In_ int iPartId,
        _In_ int iStateId,
        _In_ LPCWSTR pszText,
        _In_ int cchText,
        _In_ DWORD dwTextFlags,
        _In_ LPRECT lprc,
        _In_opt_ const DTTOPTS* pOptions)
    {
        if (!g_GlobalInitialized || !ShouldAppsUseDarkMode())
        {
            return ::OriginalDrawThemeTextEx(
                hTheme,
                hdc,
                iPartId,
                iStateId,
                pszText,
                cchText,
                dwTextFlags,
                lprc,
                pOptions);
        }

        if (IsToolbarThemeText(hTheme))
        {
            return ::OriginalDrawThemeTextEx(
                hTheme,
                hdc,
                iPartId,
                iStateId,
                pszText,
                cchText,
                dwTextFlags,
                lprc,
                pOptions);
        }

        const DTTOPTS* OptionPointer = pOptions;
        DTTOPTS AdjustedOptions = {};

        // Keep an explicitly specified color (e.g. the menu bar drawing uses
        // one) but force the color for everything else, because the light
        // theme data would otherwise provide an unreadable dark text color.
        if (!pOptions ||
            (0 == (pOptions->dwFlags & DTT_TEXTCOLOR)) ||
            (CLR_INVALID == pOptions->crText))
        {
            if (pOptions)
            {
                AdjustedOptions = *pOptions;
            }
            else
            {
                AdjustedOptions.dwSize = sizeof(DTTOPTS);
            }

            wchar_t ClassName[256] = {};
            bool HasClassName = SUCCEEDED(::OriginalGetThemeClass(
                hTheme,
                ClassName,
                MO_ARRAY_SIZE(ClassName)));

            AdjustedOptions.dwFlags |= DTT_TEXTCOLOR;
            AdjustedOptions.crText = GetDarkModeThemeTextColor(
                HasClassName,
                ClassName,
                iPartId,
                iStateId);
            OptionPointer = &AdjustedOptions;
        }

        return ::OriginalDrawThemeTextEx(
            hTheme,
            hdc,
            iPartId,
            iStateId,
            pszText,
            cchText,
            dwTextFlags,
            lprc,
            OptionPointer);
    }

    static bool IsThemeClass(
        _In_ HTHEME hTheme,
        _In_z_ LPCWSTR ExpectedClassName)
    {
        wchar_t ClassName[256] = {};
        if (FAILED(::OriginalGetThemeClass(
            hTheme,
            ClassName,
            MO_ARRAY_SIZE(ClassName))))
        {
            return false;
        }

        return (0 == ::_wcsicmp(ClassName, ExpectedClassName));
    }

    static HRESULT WINAPI DetouredDrawThemeBackground(
        _In_ HTHEME hTheme,
        _In_ HDC hdc,
        _In_ int iPartId,
        _In_ int iStateId,
        _In_ LPCRECT pRect,
        _In_opt_ LPCRECT pClipRect)
    {
        if (!g_GlobalInitialized || !ShouldAppsUseDarkMode())
        {
            return ::OriginalDrawThemeBackground(
                hTheme,
                hdc,
                iPartId,
                iStateId,
                pRect,
                pClipRect);
        }

        // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
        {
            wchar_t BgClassName[256] = {};
            if (SUCCEEDED(::OriginalGetThemeClass(
                hTheme,
                BgClassName,
                MO_ARRAY_SIZE(BgClassName))))
            {
                K7ThemeDebugTrace(
                    L"DrawThemeBackground class=%ws part=%d state=%d",
                    BgClassName,
                    iPartId,
                    iStateId);
            }
        }
        // *** END TEMPORARY DEBUG INSTRUMENTATION ***

        // The class names are resolved through GetThemeClass instead of
        // comparing cached theme handles, because the handles are per-window
        // and can be reopened (and thus invalidated) at any time, while the
        // class name of an opened theme data is stable.
        if (IsThemeClass(hTheme, L"Tab"))
        {
            const int HoveredCheckStateId[] =
            {
                -1,
                TIS_HOT,
                TILES_HOT,
                TIRES_HOT,
                TIBES_HOT,
                TTIS_HOT,
                TTILES_HOT,
                TTIRES_HOT,
                TTIBES_HOT,
                -1,
                -1,
                -1
            };

            const int SelectedCheckStateId[] =
            {
                -1,
                TIS_SELECTED,
                TILES_SELECTED,
                TIRES_SELECTED,
                TIBES_SELECTED,
                TTIS_SELECTED,
                TTILES_SELECTED,
                TTIRES_SELECTED,
                TTIBES_SELECTED,
                -1,
                -1,
                -1
            };

            switch (iPartId)
            {
            case TABP_TABITEM:
            case TABP_TABITEMLEFTEDGE:
            case TABP_TABITEMRIGHTEDGE:
            case TABP_TABITEMBOTHEDGE:
            case TABP_TOPTABITEM:
            case TABP_TOPTABITEMLEFTEDGE:
            case TABP_TOPTABITEMRIGHTEDGE:
            case TABP_TOPTABITEMBOTHEDGE:
            {
                RECT paddedRect = *pRect;
                RECT insideRect =
                {
                    pRect->left + 1,
                    pRect->top + 1,
                    pRect->right - 1,
                    pRect->bottom - 1
                };

                if (iStateId == SelectedCheckStateId[iPartId])
                {
                    paddedRect.top += 1;
                    paddedRect.bottom -= 2;

                    // Allow the rect to overlap so the bottom border outline is removed
                    insideRect.top += 1;
                    insideRect.bottom += 1;
                }

                ::FrameRect(
                    hdc,
                    &paddedRect,
                    ::GetDarkModeBorderBrush());
                ::FillRect(
                    hdc,
                    &insideRect,
                    iStateId == HoveredCheckStateId[iPartId]
                    ? ::GetDarkModeBorderBrush()
                    : ::GetDarkModeBackgroundBrush());

                return S_OK;
            }
            case TABP_PANE:
                return S_OK;
            default:
                break;
            }
        }
        else if (::IsThemeClass(hTheme, L"StatusBar"))
        {
            switch (iPartId)
            {
            case 0:
            {
                // Outside border (top, right)
                ::FillRect(hdc, pRect, ::GetDarkModeBorderBrush());
                return S_OK;
            }
            case SP_PANE:
            case SP_GRIPPERPANE:
            case SP_GRIPPER:
            {
                // Everything else
                ::FillRect(hdc, pRect, ::GetDarkModeBackgroundBrush());
                return S_OK;
            }
            default:
                break;
            }
        }
        else if (
            IsThemeClass(hTheme, L"ItemsView") ||
            IsThemeClass(hTheme, L"Header"))
        {
            // Header items and list view item backgrounds. Part 1 covers
            // HP_HEADERITEM as well as LVP_LISTITEM; parts 2-4 cover the
            // sorted/detail variations. The Header class also matches,
            // because header controls only get ItemsView applied via
            // SetWindowTheme during an in-session theme switch, while
            // freshly created controls (e.g. after a restart with inverted
            // theme already enabled) still use the default Header class.
            // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
            K7ThemeDebugTrace(
                L"DrawThemeBackground ItemsView/Header part=%d state=%d",
                iPartId,
                iStateId);
            // *** END TEMPORARY DEBUG INSTRUMENTATION ***
            switch (iPartId)
            {
            case 1:
            {
                ::FillRect(
                    hdc,
                    pRect,
                    (2 == iStateId || 3 == iStateId)
                        ? ::GetDarkModeMenuSelectedBackgroundBrush()
                        : ::GetDarkModeBackgroundBrush());
                return S_OK;
            }
            default:
            {
                // Every remaining part (0 = list background, 2-4 = detail
                // variations, 5 = empty text area, ...) is a plain
                // background surface. The DirectUI list inside the common
                // file dialogs uses parts outside 1-4, which previously
                // fell through and kept rendering with light theme colors.
                ::FillRect(hdc, pRect, ::GetDarkModeBackgroundBrush());
                return S_OK;
            }
            }
        }
        else if (
            ::IsThemeClass(hTheme, L"Explorer") ||
            ::IsThemeClass(hTheme, L"Button"))
        {
            // Buttons use the Explorer class via SetWindowTheme. Its part
            // ids collide with the tree view glyph parts, but the File
            // Manager windows don't contain tree views, so drawing the
            // button parts dark is safe here. The Button class also matches
            // for the same reason as Header above: buttons created after
            // startup (e.g. reopening the options dialog in a session that
            // started with inverted theme) still use the default class.
            switch (iPartId)
            {
            case BP_PUSHBUTTON:
            {
                ::FillRect(
                    hdc,
                    pRect,
                    (PBS_HOT == iStateId || PBS_PRESSED == iStateId)
                        ? ::GetDarkModeMenuSelectedBackgroundBrush()
                        : ::GetDarkModeBackgroundBrush());
                if (PBS_DISABLED != iStateId)
                {
                    ::FrameRect(hdc, pRect, ::GetDarkModeBorderBrush());
                }
                return S_OK;
            }
            case BP_CHECKBOX:
            case BP_RADIOBUTTON:
            {
                ::FillRect(hdc, pRect, ::GetDarkModeBackgroundBrush());

                RECT BoxRect = *pRect;
                LONG Side = BoxRect.bottom - BoxRect.top;
                if (Side > BoxRect.right - BoxRect.left)
                {
                    Side = BoxRect.right - BoxRect.left;
                }
                BoxRect.right = BoxRect.left + Side;

                // The checkbox and radio button states share the same
                // layout: 1-4 unchecked (normal/hot/pressed/disabled) and
                // 5-8 checked (normal/hot/pressed/disabled).
                DWORD Flags = (
                    BP_CHECKBOX == iPartId
                        ? DFCS_BUTTONCHECK
                        : DFCS_BUTTONRADIO);
                if (5 <= iStateId && 8 >= iStateId)
                {
                    Flags |= DFCS_CHECKED;
                }
                if (2 == iStateId || 6 == iStateId)
                {
                    Flags |= DFCS_HOT;
                }
                if (3 == iStateId || 7 == iStateId)
                {
                    Flags |= DFCS_PUSHED;
                }
                if (4 == iStateId || 8 == iStateId)
                {
                    Flags |= DFCS_INACTIVE;
                }
                ::DrawFrameControl(hdc, &BoxRect, DFC_BUTTON, Flags);
                return S_OK;
            }
            case BP_GROUPBOX:
            {
                ::FrameRect(hdc, pRect, ::GetDarkModeBorderBrush());
                return S_OK;
            }
            default:
                break;
            }
        }
        else if (::IsThemeClass(hTheme, L"Tooltip"))
        {
            switch (iPartId)
            {
            case TTP_STANDARD:
            case TTP_STANDARDTITLE:
            {
                ::FillRect(hdc, pRect, ::GetDarkModeBackgroundBrush());
                ::FrameRect(hdc, pRect, ::GetDarkModeBorderBrush());
                return S_OK;
            }
            default:
                break;
            }
        }
        else if (::IsThemeClass(hTheme, L"Toolbar"))
        {
            // The command toolbars (the "Organize / New folder" row inside
            // the common file dialogs and the File Manager main toolbar)
            // keep their light plates otherwise, which clashes with the
            // dark surfaces around them. All toolbar parts are plain
            // background surfaces for the dark look.
            ::FillRect(hdc, pRect, ::GetDarkModeBackgroundBrush());
            return S_OK;
        }
        else if (::IsThemeClass(hTheme, L"Edit"))
        {
            // Edit borders (1 = EP_EDITTEXT, 2-5 = the no-scroll / h-scroll
            // / v-scroll / hv-scroll border variants used e.g. by the search
            // box) fall back to light gradient bitmaps otherwise.
            ::FillRect(hdc, pRect, ::GetDarkModeBackgroundBrush());
            ::FrameRect(
                hdc,
                pRect,
                (4 == iStateId) // ETS_DISABLED
                    ? ::GetDarkModeMenuSelectedBackgroundBrush()
                    : ::GetDarkModeBorderBrush());
            return S_OK;
        }
        else if (::IsThemeClass(hTheme, L"SearchBox"))
        {
            ::FillRect(hdc, pRect, ::GetDarkModeBackgroundBrush());
            ::FrameRect(hdc, pRect, ::GetDarkModeBorderBrush());
            return S_OK;
        }

        return ::OriginalDrawThemeBackground(
            hTheme,
            hdc,
            iPartId,
            iStateId,
            pRect,
            pClipRect);
    }

    static HRESULT WINAPI DetouredDrawThemeBackgroundEx(
        _In_ HTHEME hTheme,
        _In_ HDC hdc,
        _In_ int iPartId,
        _In_ int iStateId,
        _In_ LPCRECT pRect,
        _In_opt_ const DTBGOPTS* pOptions)
    {
        if (!g_GlobalInitialized || !ShouldAppsUseDarkMode())
        {
            return ::OriginalDrawThemeBackgroundEx(
                hTheme,
                hdc,
                iPartId,
                iStateId,
                pRect,
                pOptions);
        }

        bool NeedTaskDialogWorkaround = (
            TDLG_PRIMARYPANEL == iPartId ||
            TDLG_SECONDARYPANEL == iPartId ||
            TDLG_EXPANDOBUTTON == iPartId ||
            TDLG_FOOTNOTEPANE == iPartId ||
            TDLG_FOOTNOTESEPARATOR == iPartId);
        if (NeedTaskDialogWorkaround)
        {
            NeedTaskDialogWorkaround = false;
            wchar_t ClassName[256] = {};
            if (S_OK == ::OriginalGetThemeClass(
                hTheme,
                ClassName,
                MO_ARRAY_SIZE(ClassName)))
            {
                NeedTaskDialogWorkaround =
                    (0 == ::_wcsicmp(ClassName, VSCLASS_TASKDIALOG));
            }
        }

        if (NeedTaskDialogWorkaround)
        {
            if (TDLG_PRIMARYPANEL == iPartId)
            {
                ::FillRect(hdc, pRect, ::GetDarkModeBackgroundBrush());
                return S_OK;
            }
            else if (
                TDLG_SECONDARYPANEL == iPartId ||
                TDLG_FOOTNOTEPANE == iPartId ||
                TDLG_FOOTNOTESEPARATOR == iPartId)
            {
                ::FillRect(hdc, pRect, ::GetDarkModeBorderBrush());
                RECT ContentRect = *pRect;
                ContentRect.top += 1;
                ::FillRect(hdc, &ContentRect, ::GetDarkModeBackgroundBrush());
                return S_OK;
            }
            else if (iPartId == TDLG_EXPANDOBUTTON)
            {
                // It seems our current implementation doesn't have the issue
                // that the button becomes invisible on dark mode in Windows 11,
                // so we don't need to do anything here for now.
            }
        }

        // In dark mode route the rest through DetouredDrawThemeBackground so
        // the Button/Header/Explorer/ItemsView fallbacks also cover callers
        // of DrawThemeBackgroundEx (e.g. freshly created controls after a
        // restart with inverted theme already enabled).
        if (pOptions && (pOptions->dwFlags & DTBG_CLIPRECT))
        {
            return ::OriginalDrawThemeBackgroundEx(
                hTheme,
                hdc,
                iPartId,
                iStateId,
                pRect,
                pOptions);
        }

        return ::DetouredDrawThemeBackground(
            hTheme,
            hdc,
            iPartId,
            iStateId,
            pRect,
            nullptr);
    }

    static HTHEME WINAPI DetouredOpenNcThemeData(
        _In_opt_ HWND hwnd,
        _In_ LPCWSTR pszClassList)
    {
        // Workaround for dark mode scrollbar
        if (0 == std::wcscmp(pszClassList, L"ScrollBar"))
        {
            return ::DetouredOpenNcThemeData(
                nullptr,
                L"Explorer::ScrollBar");
        }

        // Redirect every theme class to its built-in DarkMode_ variant.
        // Since Windows 10 1803, aero.msstyles contains a complete set of
        // dark theme classes (DarkMode_Explorer, DarkMode_ItemsView,
        // DarkMode_Toolbar, ...) which the system shell uses for its own
        // dark mode. Opening the dark variant here makes every subsequent
        // DrawThemeBackground/GetThemeColor/DrawThemeText call render with
        // native dark theme data at once - without per-class patching.
        // Classes without a DarkMode_ variant simply fail to open and fall
        // back to the original class, where the hand-drawn overrides in
        // DetouredDrawThemeBackground take over.
        if (g_GlobalInitialized &&
            ShouldAppsUseDarkMode() &&
            pszClassList &&
            L'\0' != pszClassList[0] &&
            0 != std::wcsncmp(pszClassList, L"DarkMode_", 9) &&
            nullptr == std::wcschr(pszClassList, L';'))
        {
            wchar_t DarkClassList[256] = {};
            HRESULT hr = ::StringCchPrintfW(
                DarkClassList,
                MO_ARRAY_SIZE(DarkClassList),
                L"DarkMode_%s",
                pszClassList);
            if (SUCCEEDED(hr))
            {
                HTHEME DarkTheme = ::OriginalOpenNcThemeData(
                    hwnd,
                    DarkClassList);
                // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
                K7ThemeDebugTrace(
                    L"OpenNcThemeData class=%ws -> DarkMode variant %s",
                    pszClassList,
                    (DarkTheme ? L"HIT" : L"MISS"));
                // *** END TEMPORARY DEBUG INSTRUMENTATION ***
                if (DarkTheme)
                {
                    return DarkTheme;
                }
            }
        }

        return ::OriginalOpenNcThemeData(hwnd, pszClassList);
    }

    static bool InitializeFunctionTable()
    {
        g_FunctionTable[FunctionTypes::GetSysColor].Original =
            ::GetSysColor;
        g_FunctionTable[FunctionTypes::GetSysColor].Detoured =
            ::DetouredGetSysColor;

        g_FunctionTable[FunctionTypes::GetSysColorBrush].Original =
            ::GetSysColorBrush;
        g_FunctionTable[FunctionTypes::GetSysColorBrush].Detoured =
            ::DetouredGetSysColorBrush;

        g_FunctionTable[FunctionTypes::GetThemeColor].Original =
            ::GetThemeColor;
        g_FunctionTable[FunctionTypes::GetThemeColor].Detoured =
            ::DetouredGetThemeColor;

        g_FunctionTable[FunctionTypes::DrawThemeText].Original =
            ::DrawThemeText;
        g_FunctionTable[FunctionTypes::DrawThemeText].Detoured =
            ::DetouredDrawThemeText;

        g_FunctionTable[FunctionTypes::DrawThemeTextEx].Original =
            ::DrawThemeTextEx;
        g_FunctionTable[FunctionTypes::DrawThemeTextEx].Detoured =
            ::DetouredDrawThemeTextEx;

        g_FunctionTable[FunctionTypes::DrawThemeBackground].Original =
            ::DrawThemeBackground;
        g_FunctionTable[FunctionTypes::DrawThemeBackground].Detoured =
            ::DetouredDrawThemeBackground;

        g_FunctionTable[FunctionTypes::DrawThemeBackgroundEx].Original =
            ::DrawThemeBackgroundEx;
        g_FunctionTable[FunctionTypes::DrawThemeBackgroundEx].Detoured =
            ::DetouredDrawThemeBackgroundEx;

        {
            HMODULE ModuleHandle = ::GetModuleHandleW(
                L"uxtheme.dll");
            if (ModuleHandle)
            {
                PVOID ProcAddress = ::GetProcAddress(
                    ModuleHandle,
                    MAKEINTRESOURCEA(49));
                if (ProcAddress)
                {
                    g_FunctionTable[FunctionTypes::OpenNcThemeData].Original =
                        ProcAddress;
                    g_FunctionTable[FunctionTypes::OpenNcThemeData].Detoured =
                        ::DetouredOpenNcThemeData;
                }
            }
            if (ModuleHandle)
            {
                PVOID ProcAddress = ::GetProcAddress(
                    ModuleHandle,
                    MAKEINTRESOURCEA(74));
                if (ProcAddress)
                {
                    g_FunctionTable[FunctionTypes::GetThemeClass].Original =
                        ProcAddress;
                    g_FunctionTable[FunctionTypes::GetThemeClass].Detoured =
                        nullptr;
                }
            }
        }

        g_FunctionTable[FunctionTypes::GetThemeSysColor].Original =
            ::GetThemeSysColor;
        g_FunctionTable[FunctionTypes::GetThemeSysColor].Detoured =
            ::DetouredGetThemeSysColor;

        return true;
    }

    static void UninitializeFunctionTable()
    {
        for (size_t i = 0; i < FunctionTypes::MaximumFunction; ++i)
        {
            g_FunctionTable[i].Original = nullptr;
            g_FunctionTable[i].Detoured = nullptr;
        }
    }
}

EXTERN_C MO_RESULT MOAPI K7UserRefreshTheme()
{
    // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
    K7ThemeDebugTrace(L"K7UserRefreshTheme: enter");
    // *** END TEMPORARY DEBUG INSTRUMENTATION ***
    if (!g_GlobalInitialized)
    {
        // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
        K7ThemeDebugTrace(L"K7UserRefreshTheme: not initialized, skip");
        // *** END TEMPORARY DEBUG INSTRUMENTATION ***
        return MO_RESULT_SUCCESS_OK;
    }

    bool ShouldUseDarkMode = ::ComputeShouldAppsUseDarkMode();
    SetShouldAppsUseDarkMode(ShouldUseDarkMode);

    ::ApplyProcessThemePolicy(ShouldUseDarkMode);

    ::EnumThreadWindows(
        ::GetCurrentThreadId(),
        [](
            _In_ HWND hWnd,
            _In_ LPARAM lParam) -> BOOL
    {
        UNREFERENCED_PARAMETER(lParam);

        ::MileEnableImmersiveDarkModeForWindow(
            hWnd,
            ShouldAppsUseDarkMode());

        bool ShouldExtendFrame = (
            ShouldAppsUseDarkMode() &&
            ::IsStandardDynamicRangeMode() &&
            g_ThreadContext.MicaBackdropAvailable);

        MARGINS Margins = {};
        if (ShouldExtendFrame)
        {
            Margins = { -1 };
        }
        else if (::IsFileManagerWindow(hWnd))
        {
            UINT DpiValue = ::GetDpiForWindow(hWnd);
            Margins.cyTopHeight =
                ::MulDiv(84, DpiValue, USER_DEFAULT_SCREEN_DPI);
            Margins.cyBottomHeight =
                ::MulDiv(32, DpiValue, USER_DEFAULT_SCREEN_DPI);
        }
        ::DwmExtendFrameIntoClientArea(hWnd, &Margins);

        // The top-level window itself also needs the class-specific theme
        // refresh, not only its children, otherwise the main window keeps
        // its stale theme state after the theme was switched from the
        // settings page.
        ::RefreshWindowTheme(hWnd);

        ::EnumChildWindows(
            hWnd,
            [](
                _In_ HWND hWnd,
                _In_ LPARAM lParam) -> BOOL
        {
            UNREFERENCED_PARAMETER(lParam);
            ::RefreshWindowTheme(hWnd);
            return TRUE;
        },
            0);

        ::InvalidateRect(hWnd, nullptr, TRUE);
        return TRUE;
    },
        0);

    // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
    K7ThemeDebugTrace(
        L"K7UserRefreshTheme: exit dark=%d",
        (int)ShouldAppsUseDarkMode());
    // *** END TEMPORARY DEBUG INSTRUMENTATION ***
    return MO_RESULT_SUCCESS_OK;
}

EXTERN_C MO_RESULT MOAPI K7UserInitializeDarkModeSupport()
{
    if (g_GlobalInitialized)
    {
        return MO_RESULT_SUCCESS_OK;
    }

    if (!::MileIsWindowsVersionAtLeast(10, 0, 0))
    {
        // Dark mode is only supported on Windows 10 and above, so we can just
        // return success without doing anything on older versions of Windows.
        return MO_RESULT_SUCCESS_OK;
    }

    // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
    K7ThemeDebugTrace(L"K7UserInitializeDarkModeSupport: begin");
    // *** END TEMPORARY DEBUG INSTRUMENTATION ***

    if (!::InitializeFunctionTable())
    {
        return MO_RESULT_ERROR_FAIL;
    }

    bool ShouldUseDarkMode = ::ComputeShouldAppsUseDarkMode();
    SetShouldAppsUseDarkMode(ShouldUseDarkMode);
    ::ApplyProcessThemePolicy(ShouldUseDarkMode);

    ::K7BaseDetourTransactionBegin();
    ::K7BaseDetourUpdateThread(::GetCurrentThread());
    for (size_t i = 0; i < FunctionTypes::MaximumFunction; ++i)
    {
        if (g_FunctionTable[i].Original &&
            g_FunctionTable[i].Detoured)
        {
            if (NO_ERROR != ::K7BaseDetourAttach(
                &g_FunctionTable[i].Original,
                g_FunctionTable[i].Detoured))
            {
                ::K7BaseDetourTransactionAbort();
                ::UninitializeFunctionTable();
                return MO_RESULT_ERROR_FAIL;
            }
        }
    }
    ::K7BaseDetourTransactionCommit();

    g_GlobalInitialized = true;

    // Watch for window creation so every themed control gets its
    // class-specific dark mode settings applied immediately, independent of
    // when it is created (startup, dialogs opened later, restarts with the
    // inverted theme already enabled, ...).
    // *** TEMPORARY DEBUG INSTRUMENTATION - REMOVE BEFORE RELEASE ***
    K7ThemeDebugTrace(L"K7UserInitializeDarkModeSupport: installing WinEvent hook");
    // *** END TEMPORARY DEBUG INSTRUMENTATION ***
    if (!::SetWinEventHook(
        EVENT_OBJECT_CREATE,
        EVENT_OBJECT_CREATE,
        nullptr,
        ::K7UserWinEventProc,
        0,
        0,
        WINEVENT_OUTOFCONTEXT))
    {
        K7ThemeDebugTrace(L"K7UserInitializeDarkModeSupport: SetWinEventHook failed");
    }

    return MO_RESULT_SUCCESS_OK;
}

EXTERN_C MO_RESULT MOAPI K7UserUninitializeDarkModeSupport()
{
    if (!g_GlobalInitialized)
    {
        return MO_RESULT_SUCCESS_OK;
    }
    g_GlobalInitialized = false;

    ::MileAllowDarkModeForApp(FALSE);
    ::MileRefreshImmersiveColorPolicyState();

    ::K7BaseDetourTransactionBegin();
    ::K7BaseDetourUpdateThread(::GetCurrentThread());
    for (size_t i = 0; i < FunctionTypes::MaximumFunction; ++i)
    {
        if (g_FunctionTable[i].Original &&
            g_FunctionTable[i].Detoured)
        {
            if (NO_ERROR != ::K7BaseDetourDetach(
                &g_FunctionTable[i].Original,
                g_FunctionTable[i].Detoured))
            {
                ::K7BaseDetourTransactionAbort();
                return MO_RESULT_ERROR_FAIL;
            }
        }
    }
    ::K7BaseDetourTransactionCommit();

    ::UninitializeFunctionTable();

    return MO_RESULT_SUCCESS_OK;
}
