#include "person_pack.h"
#include "person_pack_metadata.h"
#include <winhttp.h>
#include <shellapi.h>
#include <process.h>
#include <objbase.h>
#include <filesystem>
#include <array>
#include <atomic>
#include <cwchar>
#include <cstring>
#include <algorithm>

namespace lapse {
namespace {
enum class DownloadPhase { Downloading, Cancelled, Committing, Finished };
struct Download {
    std::atomic<DownloadPhase> phase{DownloadPhase::Downloading};
    std::atomic<bool> cancelled{false}, done{false};
    std::atomic<uint64_t> received{0};
    bool success = false;
    std::wstring path, detail;
    void cancel() noexcept {
        auto expected = DownloadPhase::Downloading;
        if (phase.compare_exchange_strong(expected, DownloadPhase::Cancelled)) cancelled.store(true);
    }
};
std::atomic<bool> downloadBusy{false};
struct Internet {
    HINTERNET value = nullptr;
    ~Internet() { if (value) WinHttpCloseHandle(value); }
};
struct Temporary {
    HANDLE value = INVALID_HANDLE_VALUE;
    std::wstring cleanupPath;
    ~Temporary() {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        if (!cleanupPath.empty()) DeleteFileW(cleanupPath.c_str());
    }
};
bool receivePack(Download& task) {
    if (!PersonPackExpectedBytes) { task.detail = L"This development build has no approved detector download."; return false; }
    const auto directory = std::filesystem::path(task.path).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    if (ec || task.cancelled.load()) { task.detail = L"The local model folder could not be prepared."; return false; }
    GUID guid{}; wchar_t token[40]{};
    if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, token, static_cast<int>(std::size(token)))) return false;
    const auto temporary = directory / (std::wstring(L".download-") + token + L".tmp");
    Temporary file;
    // Explicit delete disposition is reversible. FILE_FLAG_DELETE_ON_CLOSE is
    // not: clearing disposition still leaves that creation flag effective and
    // would delete a successfully renamed installation when this handle closes.
    auto cleanupPath = temporary.wstring();
    file.value = CreateFileW(temporary.c_str(), GENERIC_READ | GENERIC_WRITE | DELETE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) { task.detail = L"The optional download file could not be created."; return false; }
    file.cleanupPath.swap(cleanupPath);
    FILE_DISPOSITION_INFO initialDisposition{}; initialDisposition.DeleteFile = TRUE;
    if (!SetFileInformationByHandle(file.value, FileDispositionInfo, &initialDisposition, sizeof(initialDisposition))) {
        task.detail = L"The optional download file could not be prepared safely."; return false;
    }
    Internet session, connection, request;
    session.value = WinHttpOpen(L"Timelapse optional person detector", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.value || !WinHttpSetTimeouts(session.value, 5000, 5000, 5000, 5000)) return false;
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    if (!WinHttpSetOption(session.value, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols))) return false;
    std::array<wchar_t, 256> host{}; std::array<wchar_t, 2048> resource{};
    URL_COMPONENTS url{}; url.dwStructSize = sizeof(url);
    url.lpszHostName = host.data(); url.dwHostNameLength = static_cast<DWORD>(host.size());
    url.lpszUrlPath = resource.data(); url.dwUrlPathLength = static_cast<DWORD>(resource.size());
    if (!WinHttpCrackUrl(PersonPackDownloadUrl, 0, 0, &url) || url.nScheme != INTERNET_SCHEME_HTTPS) return false;
    connection.value = WinHttpConnect(session.value, host.data(), url.nPort, 0);
    if (!connection.value) return false;
    request.value = WinHttpOpenRequest(connection.value, L"GET", resource.data(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request.value) return false;
    DWORD redirects = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    DWORD disabled = WINHTTP_DISABLE_AUTHENTICATION | WINHTTP_DISABLE_COOKIES;
    if (!WinHttpSetOption(request.value, WINHTTP_OPTION_REDIRECT_POLICY, &redirects, sizeof(redirects)) ||
        !WinHttpSetOption(request.value, WINHTTP_OPTION_DISABLE_FEATURE, &disabled, sizeof(disabled))) return false;
    if (task.cancelled.load() || !WinHttpSendRequest(request.value, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr)) return false;
    DWORD status = 0, statusBytes = sizeof(status);
    if (!WinHttpQueryHeaders(request.value, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes, WINHTTP_NO_HEADER_INDEX) || status != 200) {
        task.detail = L"The detector download is unavailable. Try again later."; return false;
    }
    std::array<uint8_t, 65536> buffer{};
    uint64_t total = 0;
    const uint64_t began = GetTickCount64();
    for (;;) {
        if (task.cancelled.load() || GetTickCount64() - began > 120000) return false;
        DWORD count = 0;
        if (!WinHttpReadData(request.value, buffer.data(), static_cast<DWORD>(buffer.size()), &count)) return false;
        if (!count) break;
        if (count > PersonPackExpectedBytes - total) { task.detail = L"The downloaded detector has an unexpected size."; return false; }
        DWORD written = 0;
        if (!WriteFile(file.value, buffer.data(), count, &written, nullptr) || written != count) {
            task.detail = L"The detector download could not be saved. Check free disk space."; return false;
        }
        total += count; task.received.store(total);
    }
    if (total != PersonPackExpectedBytes || !FlushFileBuffers(file.value) || !verifyPersonPackFile(file.value, &task.cancelled)) {
        task.detail = L"The download did not pass verification. No detector was installed."; return false;
    }
    const size_t nameBytes = task.path.size() * sizeof(wchar_t);
    std::vector<uint8_t> storage(sizeof(FILE_RENAME_INFO) + nameBytes);
    auto& rename = *reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename.ReplaceIfExists = TRUE; rename.RootDirectory = nullptr; rename.FileNameLength = static_cast<DWORD>(nameBytes);
    std::memcpy(rename.FileName, task.path.data(), nameBytes);
    std::wstring successDetail = L"Ready. The detector stays local and runs only while a person mode is recording.";
    auto expected = DownloadPhase::Downloading;
    if (!task.phase.compare_exchange_strong(expected, DownloadPhase::Committing)) return false;
    // Disable temporary deletion BEFORE replacing the prior installation. After
    // a successful rename there are no remaining operations that can fail it.
    FILE_DISPOSITION_INFO disposition{}; disposition.DeleteFile = FALSE;
    if (!SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition))) {
        task.detail = L"The verified detector could not be retained. Try installing again."; return false;
    }
    if (!SetFileInformationByHandle(file.value, FileRenameInfo, &rename, static_cast<DWORD>(storage.size()))) {
        task.detail = L"The verified detector could not replace the installed copy. Close any recording and try again."; return false;
    }
    file.cleanupPath.clear();
    task.detail.swap(successDetail);
    return true;
}
unsigned __stdcall download(void* argument) noexcept {
    std::unique_ptr<std::shared_ptr<Download>> owned(static_cast<std::shared_ptr<Download>*>(argument));
    const auto task = *owned;
    try { task->success = receivePack(*task); }
    catch (...) { try { task->detail = L"The detector download could not allocate a resource."; } catch (...) {} }
    if (!task->success) {
        try {
            if (task->cancelled.load()) task->detail = L"Download cancelled. The previous installation was preserved.";
            else if (task->detail.empty()) task->detail = L"The detector could not be downloaded. Check the connection and try again.";
        } catch (...) {}
    }
    task->phase.store(DownloadPhase::Finished);
    // Completion must publish the released global slot too: the UI consumes a
    // completed task once, so seeing done before !busy could strand Download.
    downloadBusy.store(false); task->done.store(true, std::memory_order_release); return 0;
}
enum { Info = 2400, Location, Status, Get, Remove, Licenses, LocationLabel };
struct Dialog {
    HWND owner = nullptr;
    PersonPackInfo installed;
    std::shared_ptr<Download> task;
    HFONT font = nullptr;
    bool closing = false;
    int dpi = 96, scrollX = 0, scrollY = 0;
    bool layingOut = false;
    int scale(int value) const noexcept { return MulDiv(value, dpi, 96); }
};
RECT fitManager(RECT rectangle) {
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&rectangle, MONITOR_DEFAULTTONEAREST), &monitor)) return rectangle;
    const LONG width = std::min(rectangle.right - rectangle.left, monitor.rcWork.right - monitor.rcWork.left);
    const LONG height = std::min(rectangle.bottom - rectangle.top, monitor.rcWork.bottom - monitor.rcWork.top);
    rectangle.left = std::clamp(rectangle.left, monitor.rcWork.left, monitor.rcWork.right - width);
    rectangle.top = std::clamp(rectangle.top, monitor.rcWork.top, monitor.rcWork.bottom - height);
    rectangle.right = rectangle.left + width; rectangle.bottom = rectangle.top + height; return rectangle;
}
void layoutManager(HWND window, Dialog& dialog) {
    if (dialog.layingOut || !GetDlgItem(window, IDCANCEL)) return;
    dialog.layingOut = true;
    RECT client{}; GetClientRect(window, &client);
    const auto style = GetWindowLongPtrW(window, GWL_STYLE);
    const int barW = GetSystemMetricsForDpi(SM_CXVSCROLL, dialog.dpi), barH = GetSystemMetricsForDpi(SM_CYHSCROLL, dialog.dpi);
    const int availableW = client.right + ((style & WS_VSCROLL) ? barW : 0), availableH = client.bottom + ((style & WS_HSCROLL) ? barH : 0);
    const int width = dialog.scale(566), height = dialog.scale(360);
    bool horizontal = false, vertical = false;
    for (int i = 0; i < 3; ++i) { horizontal = availableW - (vertical ? barW : 0) < width; vertical = availableH - (horizontal ? barH : 0) < height; }
    ShowScrollBar(window, SB_HORZ, horizontal); ShowScrollBar(window, SB_VERT, vertical); GetClientRect(window, &client);
    dialog.scrollX = std::clamp(dialog.scrollX, 0, std::max(0, width - int(client.right)));
    dialog.scrollY = std::clamp(dialog.scrollY, 0, std::max(0, height - int(client.bottom)));
    SCROLLINFO info{sizeof(info), SIF_RANGE | SIF_PAGE | SIF_POS}; info.nMax = width - 1; info.nPage = std::max(1L, client.right); info.nPos = dialog.scrollX; SetScrollInfo(window, SB_HORZ, &info, TRUE);
    info.nMax = height - 1; info.nPage = std::max(1L, client.bottom); info.nPos = dialog.scrollY; SetScrollInfo(window, SB_VERT, &info, TRUE);
    const auto move = [&](int id, int x, int y, int w, int h) { MoveWindow(GetDlgItem(window, id), dialog.scale(x) - dialog.scrollX, dialog.scale(y) - dialog.scrollY, dialog.scale(w), dialog.scale(h), TRUE); };
    move(Info, 20, 18, 526, 110); move(LocationLabel, 20, 139, 526, 22); move(Location, 20, 164, 526, 25);
    move(Status, 20, 205, 526, 66); move(Get, 20, 309, 158, 30); move(Remove, 188, 309, 85, 30); move(Licenses, 283, 309, 85, 30); move(IDCANCEL, 390, 309, 156, 30);
    dialog.layingOut = false;
}
void revealManager(HWND window, Dialog& dialog, HWND child) {
    if (!child || !IsChild(window, child)) return;
    RECT rectangle{}, client{}; GetWindowRect(child, &rectangle); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rectangle), 2); GetClientRect(window, &client);
    if (rectangle.left < 0) dialog.scrollX += rectangle.left; else if (rectangle.right > client.right) dialog.scrollX += rectangle.right - client.right;
    if (rectangle.top < 0) dialog.scrollY += rectangle.top; else if (rectangle.bottom > client.bottom) dialog.scrollY += rectangle.bottom - client.bottom;
    layoutManager(window, dialog);
}
void fontManager(HWND window, Dialog& dialog) {
    HFONT replacement = CreateFontW(-dialog.scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    if (!replacement) throw std::bad_alloc();
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(replacement), TRUE);
    if (dialog.font) DeleteObject(dialog.font); dialog.font = replacement;
}
void enableActions(HWND window, Dialog& dialog) {
    const bool busy = dialog.task && !dialog.task->done.load(std::memory_order_acquire);
    EnableWindow(GetDlgItem(window, Get), !busy && !downloadBusy.load());
    EnableWindow(GetDlgItem(window, Remove), !busy && dialog.installed.state != PersonPackState::Missing);
    SetDlgItemTextW(window, IDCANCEL, busy ? L"Cancel download" : L"Close");
}
INT_PTR CALLBACK manageProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    auto* dialog = reinterpret_cast<Dialog*>(GetWindowLongPtrW(window, DWLP_USER));
    try {
        if (msg == WM_INITDIALOG) {
            dialog = reinterpret_cast<Dialog*>(lp); SetWindowLongPtrW(window, DWLP_USER, lp);
            const int dpi = static_cast<int>(GetDpiForWindow(window)); dialog->dpi = dpi;
            auto scale = [&](int n) { return MulDiv(n, dpi, 96); };
            RECT client{0, 0, scale(566), scale(360)};
            AdjustWindowRectExForDpi(&client, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | DS_MODALFRAME, FALSE, 0, dpi);
            RECT owner{}; GetWindowRect(dialog->owner, &owner);
            const LONG width = client.right - client.left, height = client.bottom - client.top;
            RECT position{(owner.left + owner.right - width) / 2, (owner.top + owner.bottom - height) / 2, 0, 0};
            position.right = position.left + width; position.bottom = position.top + height; position = fitManager(position);
            SetWindowPos(window, nullptr, position.left, position.top, position.right - position.left, position.bottom - position.top, SWP_NOZORDER);
            SetWindowTextW(window, L"Optional person detector");
            fontManager(window, *dialog);
            auto control = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
                HWND child = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, scale(x), scale(y), scale(w), scale(h),
                    window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
                if (!child) throw std::bad_alloc();
                SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(dialog->font), TRUE);
            };
            const std::wstring description = L"NanoDet-m person detector, revision 1\r\n\r\nOptional download: " +
                std::to_wstring(PersonPackExpectedBytes) + L" bytes (model and CPU runtime).\r\n"
                L"Images stay on this computer. Checks can miss people.\r\n"
                L"Installing does not turn on time compression.";
            control(L"STATIC", description.c_str(), SS_NOPREFIX, Info, 20, 18, 526, 110);
            control(L"STATIC", L"Installed location", SS_NOPREFIX, LocationLabel, 20, 139, 526, 22);
            control(L"EDIT", dialog->installed.path.c_str(), ES_READONLY | ES_AUTOHSCROLL | WS_BORDER | WS_TABSTOP, Location, 20, 164, 526, 25);
            control(L"STATIC", dialog->installed.detail.c_str(), SS_NOPREFIX, Status, 20, 205, 526, 66);
            control(L"BUTTON", L"&Download / reinstall", WS_TABSTOP | BS_PUSHBUTTON | BS_NOTIFY, Get, 20, 309, 158, 30);
            control(L"BUTTON", L"&Remove", WS_TABSTOP | BS_PUSHBUTTON | BS_NOTIFY, Remove, 188, 309, 85, 30);
            control(L"BUTTON", L"&Licenses", WS_TABSTOP | BS_PUSHBUTTON | BS_NOTIFY, Licenses, 283, 309, 85, 30);
            control(L"BUTTON", L"Close", WS_TABSTOP | BS_DEFPUSHBUTTON | BS_NOTIFY, IDCANCEL, 390, 309, 156, 30);
            layoutManager(window, *dialog);
            if (!SetTimer(window, 1, 150, nullptr)) { EndDialog(window, IDCANCEL); return TRUE; }
            enableActions(window, *dialog); return TRUE;
        }
        if (!dialog) return FALSE;
        if (msg == WM_SIZE) { layoutManager(window, *dialog); return TRUE; }
        if (msg == WM_DPICHANGED) {
            const int priorDpi = dialog->dpi; dialog->dpi = HIWORD(wp);
            dialog->scrollX = MulDiv(dialog->scrollX, dialog->dpi, priorDpi); dialog->scrollY = MulDiv(dialog->scrollY, dialog->dpi, priorDpi);
            fontManager(window, *dialog); const auto position = fitManager(*reinterpret_cast<RECT*>(lp));
            SetWindowPos(window, nullptr, position.left, position.top, position.right - position.left, position.bottom - position.top, SWP_NOZORDER | SWP_NOACTIVATE);
            layoutManager(window, *dialog); revealManager(window, *dialog, GetFocus()); return TRUE;
        }
        if (msg == WM_HSCROLL || msg == WM_VSCROLL || msg == WM_MOUSEWHEEL) {
            const bool horizontal = msg == WM_HSCROLL; int& offset = horizontal ? dialog->scrollX : dialog->scrollY;
            RECT client{}; GetClientRect(window, &client); const int page = horizontal ? client.right : client.bottom;
            if (msg == WM_MOUSEWHEEL) offset -= GET_WHEEL_DELTA_WPARAM(wp) * dialog->scale(48) / WHEEL_DELTA;
            else switch (LOWORD(wp)) {
                case SB_LINEUP: offset -= dialog->scale(24); break; case SB_LINEDOWN: offset += dialog->scale(24); break;
                case SB_PAGEUP: offset -= page; break; case SB_PAGEDOWN: offset += page; break;
                case SB_TOP: offset = 0; break; case SB_BOTTOM: offset = dialog->scale(horizontal ? 566 : 360); break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: { SCROLLINFO info{sizeof(info), SIF_TRACKPOS}; GetScrollInfo(window, horizontal ? SB_HORZ : SB_VERT, &info); offset = info.nTrackPos; break; }
            }
            layoutManager(window, *dialog); return TRUE;
        }
        if (msg == WM_COMMAND && (HIWORD(wp) == BN_SETFOCUS || HIWORD(wp) == EN_SETFOCUS)) { revealManager(window, *dialog, reinterpret_cast<HWND>(lp)); return TRUE; }
        if (msg == WM_TIMER) {
            if (!IsWindow(dialog->owner)) { if (dialog->task) dialog->task->cancel(); EndDialog(window, IDCANCEL); return TRUE; }
            if (dialog->task) {
                if (dialog->task->done.load(std::memory_order_acquire)) {
                    SetDlgItemTextW(window, Status, dialog->task->detail.c_str());
                    dialog->installed = inspectPersonPack(); dialog->task.reset(); enableActions(window, *dialog);
                    if (dialog->closing) EndDialog(window, IDCANCEL);
                } else {
                    const std::wstring status = dialog->task->cancelled.load() ? L"Cancelling download..." :
                        dialog->task->phase.load() == DownloadPhase::Committing ? L"Finishing verified installation..." :
                        L"Downloading: " + std::to_wstring(dialog->task->received.load()) + L" / " + std::to_wstring(PersonPackExpectedBytes) + L" bytes";
                    SetDlgItemTextW(window, Status, status.c_str());
                }
            }
            return TRUE;
        }
        if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL && HIWORD(wp) == BN_CLICKED)) {
            if (dialog->task && !dialog->task->done.load(std::memory_order_acquire)) {
                dialog->task->cancel(); dialog->closing = true; EnableWindow(GetDlgItem(window, IDCANCEL), FALSE);
            } else EndDialog(window, IDCANCEL);
            return TRUE;
        }
        if (msg == WM_COMMAND && LOWORD(wp) == Get && HIWORD(wp) == BN_CLICKED) {
            bool expected = false;
            if (dialog->task || !downloadBusy.compare_exchange_strong(expected, true)) return TRUE;
            try {
                dialog->task = std::make_shared<Download>(); dialog->task->path = personPackPath();
                auto argument = std::make_unique<std::shared_ptr<Download>>(dialog->task);
                const uintptr_t thread = _beginthreadex(nullptr, 0, download, argument.get(), 0, nullptr);
                if (!thread) throw std::bad_alloc();
                argument.release(); CloseHandle(reinterpret_cast<HANDLE>(thread)); enableActions(window, *dialog);
            } catch (...) {
                dialog->task.reset(); downloadBusy.store(false); SetDlgItemTextW(window, Status, L"Windows could not start the optional download.");
            }
            return TRUE;
        }
        if (msg == WM_COMMAND && LOWORD(wp) == Remove && HIWORD(wp) == BN_CLICKED) {
            if (dialog->task || downloadBusy.load()) return TRUE;
            const std::wstring path = personPackPath();
            const bool removed = !path.empty() && DeleteFileW(path.c_str());
            dialog->installed = inspectPersonPack();
            SetDlgItemTextW(window, Status, removed ? L"Optional detector removed. Recording preferences are unchanged." : L"The detector could not be removed. Finish recording and try again.");
            enableActions(window, *dialog); return TRUE;
        }
        if (msg == WM_COMMAND && LOWORD(wp) == Licenses && HIWORD(wp) == BN_CLICKED) {
            ShellExecuteW(window, L"open", PersonPackLicenseUrl, nullptr, nullptr, SW_SHOWNORMAL); return TRUE;
        }
        if (msg == WM_DESTROY) {
            KillTimer(window, 1); if (dialog->task) dialog->task->cancel();
            if (dialog->font) { DeleteObject(dialog->font); dialog->font = nullptr; }
            return TRUE;
        }
    } catch (...) {
        if (dialog && dialog->task) dialog->task->cancel();
        EndDialog(window, IDCANCEL); return TRUE;
    }
    return FALSE;
}
}
void showPersonPackDialog(HWND owner) {
    Dialog dialog; dialog.owner = owner; dialog.installed = inspectPersonPack();
    struct Template { DLGTEMPLATE dialog; WORD menu, type, title; } resource{};
    resource.dialog.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | DS_MODALFRAME;
    resource.dialog.cx = 280; resource.dialog.cy = 180;
    DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &resource.dialog, owner, manageProc, reinterpret_cast<LPARAM>(&dialog));
}
}
