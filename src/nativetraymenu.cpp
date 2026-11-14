#include "nativetraymenu.h"
#ifdef Q_OS_WIN
#include <QAction>
#include <QMenu>
#include <QOperatingSystemVersion>
#include <QTimer>
namespace Screen {
namespace {
void followSystemMenuTheme(HWND owner) {
    // These optional UxTheme exports are also used by native Windows apps.
    // Ordinal 135 had a different signature before Windows 10 1903.
    // Older systems retain their standard native menu rendering.
    if (QOperatingSystemVersion::current() < QOperatingSystemVersion(QOperatingSystemVersion::Windows, 10, 0, 18362))
        return;
    static const HMODULE theme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!theme) return;
    enum class AppMode { Default, AllowDark, ForceDark, ForceLight };
    using SetMode = AppMode (WINAPI *)(AppMode);
    using AllowWindow = bool (WINAPI *)(HWND, bool);
    using Refresh = void (WINAPI *)();
    const auto setMode = reinterpret_cast<SetMode>(GetProcAddress(theme, MAKEINTRESOURCEA(135)));
    const auto allowWindow = reinterpret_cast<AllowWindow>(GetProcAddress(theme, MAKEINTRESOURCEA(133)));
    const auto refresh = reinterpret_cast<Refresh>(GetProcAddress(theme, MAKEINTRESOURCEA(104)));
    const auto flush = reinterpret_cast<Refresh>(GetProcAddress(theme, MAKEINTRESOURCEA(136)));
    if (!setMode || !allowWindow || !refresh || !flush) return;
    HIGHCONTRASTW contrast{};
    contrast.cbSize = sizeof(contrast);
    const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON);
    refresh();
    setMode(highContrast ? AppMode::Default : AppMode::AllowDark);
    allowWindow(owner, !highContrast);
    flush();
}
}
NativeTrayMenu::NativeTrayMenu(QMenu *source) { handle = build(source); }
NativeTrayMenu::~NativeTrayMenu() {
    if (handle) DestroyMenu(handle); // Also releases the submenus.
}
HMENU NativeTrayMenu::build(QMenu *source) {
    const auto result = CreatePopupMenu();
    if (!result) return nullptr;
    for (auto action : source->actions()) {
        if (!action->isVisible()) continue;
        MENUITEMINFOW item{};
        item.cbSize = sizeof(item);
        item.fMask = MIIM_FTYPE | MIIM_STATE;
        item.fType = action->isSeparator() ? MFT_SEPARATOR : MFT_STRING;
        item.fState = action->isEnabled() ? MFS_ENABLED : MFS_DISABLED;
        if (action->isCheckable() && action->isChecked()) item.fState |= MFS_CHECKED;
        auto label = action->text().toStdWString();
        if (!action->isSeparator()) {
            item.fMask |= MIIM_STRING;
            item.dwTypeData = label.data();
            if (auto submenu = action->menu()) {
                item.fMask |= MIIM_SUBMENU;
                item.hSubMenu = build(submenu);
                if (!item.hSubMenu) { DestroyMenu(result); return nullptr; }
            } else {
                item.fMask |= MIIM_ID;
                item.wID = UINT(commands.size()) + 1;
                commands.insert(item.wID, action);
            }
        }
        if (!InsertMenuItemW(result, UINT(GetMenuItemCount(result)), TRUE, &item)) {
            if (item.hSubMenu) DestroyMenu(item.hSubMenu);
            DestroyMenu(result);
            return nullptr;
        }
    }
    return result;
}
bool NativeTrayMenu::popup() {
    if (!handle) return false;
    // A separate hidden owner also works when the editor has been destroyed
    // in lightweight mode. It must be a top-level window for outside-click dismissal.
    const auto owner = CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"Turing Screen tray menu",
                                      WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) return false;
    followSystemMenuTheme(owner);
    POINT position{};
    GetCursorPos(&position); // Physical coordinates, including mixed-DPI desktops.
    SetForegroundWindow(owner);
    const UINT selected = UINT(TrackPopupMenuEx(handle, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                               position.x, position.y, owner, nullptr));
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyWindow(owner);
    if (auto action = commands.value(selected)) {
        // Run only after the native menu loop has unwound. Actions may rebuild
        // the editor/menu or quit the application.
        QTimer::singleShot(0, action, [action] { if (action && action->isEnabled()) action->trigger(); });
    }
    return true;
}
} // namespace Screen
#endif
