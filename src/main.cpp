#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#include <cstdio>
#include <cwchar>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

#include "Scintilla.h"

static HINSTANCE g_hInst = nullptr;
static HWND g_hwndMain = nullptr;
static HWND g_hwndEdit = nullptr;
static HWND g_hwndUndoButton = nullptr;
static HWND g_hwndRedoButton = nullptr;
static HWND g_hwndSaveButton = nullptr;
static WNDPROC g_originalEditWndProc = nullptr;
static std::wstring g_appDir;
static std::wstring g_currentPath;
static std::wstring g_startPath;
static bool g_loading = false;
static DWORD g_lastBackupTick = 0;
static HWND g_hwndFindDialog = nullptr;
static bool g_findDialogReplace = false;
static UINT g_findReplaceMsg = 0;
static wchar_t g_findText[512] = {};
static wchar_t g_replaceText[512] = {};
static FINDREPLACEW g_findReplace = {};

enum {
    IDM_NEW = 1001,
    IDM_OPEN,
    IDM_SAVE,
    IDM_SAVEAS,
    IDM_EXIT,
    IDM_FIND,
    IDM_REPLACE,
    IDM_UNDO,
    IDM_REDO,
    IDM_HL_YELLOW,
    IDM_HL_GREEN,
    IDM_HL_BLUE,
    IDM_HL_RED,
    IDM_HL_CYAN,
    IDM_HL_INDIGO,
    IDM_HL_PURPLE,
    IDM_BOLD,
    IDM_HL_CLEAR_SELECTION,
    IDM_HL_CLEAR_ALL,
    IDM_OPEN_BACKUPS,
    IDM_CTX_CUT,
    IDM_CTX_COPY,
    IDM_CTX_PASTE,
    IDM_CTX_DELETE,
    IDM_CTX_SELECT_ALL
};

static const UINT_PTR TIMER_AUTOSAVE = 1;
static const UINT AUTOSAVE_MS = 2000; // recovery snapshot interval, never saves the actual document
static const DWORD BACKUP_MS = 5 * 60 * 1000;

static const int INDIC_HL_YELLOW = 10;
static const int INDIC_HL_GREEN = 11;
static const int INDIC_HL_BLUE = 12;
static const int INDIC_HL_RED = 13;
static const int INDIC_HL_CYAN = 14;
static const int INDIC_HL_INDIGO = 15;
static const int INDIC_HL_PURPLE = 16;
static const int INDIC_BOLD = 17;
static const int INDICATORS[] = {INDIC_HL_YELLOW, INDIC_HL_GREEN, INDIC_HL_BLUE, INDIC_HL_RED, INDIC_HL_CYAN, INDIC_HL_INDIGO, INDIC_HL_PURPLE, INDIC_BOLD};
static constexpr int COLOR_COUNT = 7;

static LRESULT Sci(UINT msg, WPARAM wParam = 0, LPARAM lParam = 0) {
    return SendMessageW(g_hwndEdit, msg, wParam, lParam);
}

static std::string WideToUtf8(const wchar_t* text) {
    if (!text || !*text) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    out.resize(static_cast<size_t>(size - 1));
    return out;
}

static std::wstring JoinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

static void EnsureDir(const std::wstring& path) {
    SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
}

static std::wstring GetExeDir() {
    wchar_t path[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *slash = L'\0';
    return path;
}

static const wchar_t* FileNamePtr(const std::wstring& path) {
    const wchar_t* p = path.c_str();
    const wchar_t* slash = wcsrchr(p, L'\\');
    return slash ? slash + 1 : p;
}

static uint64_t HashPath(const std::wstring& path) {
    uint64_t h = 1469598103934665603ULL;
    for (wchar_t ch : path) {
        wchar_t c = static_cast<wchar_t>(towlower(ch));
        h ^= static_cast<uint16_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

static std::wstring MetaPath() {
    std::wstring dir = JoinPath(g_appDir, L"meta");
    EnsureDir(dir);
    wchar_t name[64] = {};
    swprintf(name, std::size(name), L"%016llx.hl", static_cast<unsigned long long>(HashPath(g_currentPath)));
    return JoinPath(dir, name);
}

static std::wstring BackupDir() {
    std::wstring root = JoinPath(g_appDir, L"backups");
    EnsureDir(root);
    wchar_t hash[32] = {};
    swprintf(hash, std::size(hash), L"%016llx", static_cast<unsigned long long>(HashPath(g_currentPath)));
    std::wstring dir = JoinPath(root, hash);
    EnsureDir(dir);
    return dir;
}

static bool ReadAllBytes(const std::wstring& path, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(h, &size) || size.QuadPart < 0 || size.QuadPart > 128LL * 1024 * 1024) {
        CloseHandle(h);
        return false;
    }

    out.resize(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    bool ok = out.empty() || ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    out.resize(read);
    return true;
}

static bool WriteAllBytesAtomic(const std::wstring& path, const std::string& data) {
    std::wstring temp = path + L".lightnote.tmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    bool ok = data.empty() || WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    if (ok) ok = FlushFileBuffers(h) != FALSE;
    CloseHandle(h);

    if (!ok || written != data.size()) {
        DeleteFileW(temp.c_str());
        return false;
    }

    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temp.c_str());
        return false;
    }
    return true;
}

static std::string GetDocumentText() {
    LRESULT len = Sci(SCI_GETLENGTH);
    std::string text(static_cast<size_t>(len) + 1, '\0');
    Sci(SCI_GETTEXT, static_cast<WPARAM>(text.size()), reinterpret_cast<LPARAM>(text.data()));
    text.resize(static_cast<size_t>(len));
    return text;
}

static void UpdateTitle(bool dirty = false) {
    std::wstring title = L"LightNote V0.7.2";
    if (!g_currentPath.empty()) {
        title += L" - ";
        title += FileNamePtr(g_currentPath);
    }
    if (dirty) title += L" *";
    SetWindowTextW(g_hwndMain, title.c_str());
}

static void ConfigureIndicators() {
    auto setup = [](int id, COLORREF color) {
        Sci(SCI_INDICSETSTYLE, id, INDIC_TEXTFORE);
        Sci(SCI_INDICSETFORE, id, color);
        Sci(SCI_INDICSETUNDER, id, FALSE);
    };

    setup(INDIC_HL_YELLOW, RGB(210, 150, 0));
    setup(INDIC_HL_GREEN, RGB(0, 145, 70));
    setup(INDIC_HL_BLUE, RGB(0, 95, 210));
    setup(INDIC_HL_RED, RGB(210, 35, 45));
    setup(INDIC_HL_CYAN, RGB(0, 155, 175));
    setup(INDIC_HL_INDIGO, RGB(50, 70, 180));
    setup(INDIC_HL_PURPLE, RGB(140, 50, 180));
    Sci(SCI_INDICSETSTYLE, INDIC_BOLD, INDIC_HIDDEN);
}

static void ClearIndicator(int id, LRESULT start, LRESULT length) {
    Sci(SCI_SETINDICATORCURRENT, id);
    Sci(SCI_INDICATORCLEARRANGE, static_cast<WPARAM>(start), static_cast<LPARAM>(length));
}

static void FillIndicator(int id, LRESULT start, LRESULT length) {
    if (length <= 0) return;
    Sci(SCI_SETINDICATORCURRENT, id);
    Sci(SCI_SETINDICATORVALUE, 1);
    Sci(SCI_INDICATORFILLRANGE, static_cast<WPARAM>(start), static_cast<LPARAM>(length));
}

static void SaveHighlights() {
    if (g_currentPath.empty()) return;
    FILE* f = _wfopen(MetaPath().c_str(), L"wb");
    if (!f) return;

    LRESULT len = Sci(SCI_GETLENGTH);
    for (int color = 0; color <= COLOR_COUNT; ++color) {
        int id = INDICATORS[color];
        LRESULT pos = 0;
        while (pos < len) {
            LRESULT value = Sci(SCI_INDICATORVALUEAT, id, pos);
            if (!value) {
                ++pos;
                continue;
            }
            LRESULT start = pos;
            while (pos < len && Sci(SCI_INDICATORVALUEAT, id, pos)) ++pos;
            fprintf(f, "%d %lld %lld\n", color,
                    static_cast<long long>(start),
                    static_cast<long long>(pos - start));
        }
    }
    fclose(f);
}

static void LoadHighlights() {
    LRESULT len = Sci(SCI_GETLENGTH);
    for (int id : INDICATORS) ClearIndicator(id, 0, len);

    FILE* f = _wfopen(MetaPath().c_str(), L"rb");
    if (!f) return;

    int color = 0;
    long long start = 0;
    long long length = 0;
    while (fscanf(f, "%d %lld %lld", &color, &start, &length) == 3) {
        if (color >= 0 && color <= COLOR_COUNT && start >= 0 && length > 0 &&
            start < len && start + length <= len) {
            FillIndicator(INDICATORS[color], static_cast<LRESULT>(start), static_cast<LRESULT>(length));
            if (color == COLOR_COUNT) {
                Sci(SCI_STARTSTYLING, static_cast<WPARAM>(start));
                Sci(SCI_SETSTYLING, static_cast<WPARAM>(length), 1);
            }
        }
    }
    fclose(f);
}

static void BackupSnapshot(bool force) {
    if (g_currentPath.empty() || GetFileAttributesW(g_currentPath.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    DWORD now = GetTickCount();
    if (!force && g_lastBackupTick != 0 && (now - g_lastBackupTick) < BACKUP_MS) return;

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t name[MAX_PATH] = {};
    swprintf(name, std::size(name),
             L"%04u%02u%02u-%02u%02u%02u-%ls",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
             FileNamePtr(g_currentPath));

    std::wstring dst = JoinPath(BackupDir(), name);
    if (CopyFileW(g_currentPath.c_str(), dst.c_str(), FALSE)) {
        g_lastBackupTick = now;
    }
}

static std::wstring RecoveryPath() {
    std::wstring dir=JoinPath(g_appDir,L"recovery");
    EnsureDir(dir);
    wchar_t key[40] = {};
    swprintf(key, 40, L"%016llx.recovery", static_cast<unsigned long long>(HashPath(g_currentPath)));
    return JoinPath(dir,key);
}

static void RemoveRecovery() {
    if(!g_currentPath.empty()) DeleteFileW(RecoveryPath().c_str());
}

static void SnapshotRecovery() {
    if(g_loading || g_currentPath.empty()) return;
    if(!Sci(SCI_GETMODIFY)) { RemoveRecovery(); return; }
    WriteAllBytesAtomic(RecoveryPath(),GetDocumentText());
}

static bool SaveDocument(bool forceBackup) {
    if (g_currentPath.empty()) return false;

    std::string text = GetDocumentText();
    if (!WriteAllBytesAtomic(g_currentPath, text)) {
        MessageBoxW(g_hwndMain, L"保存失败。文件可能被其他程序占用。", L"LightNote", MB_OK | MB_ICONERROR);
        return false;
    }

    Sci(SCI_SETSAVEPOINT);
    RemoveRecovery();
    SaveHighlights();
    BackupSnapshot(forceBackup);
    UpdateTitle(false);
    return true;
}

static bool ConfirmPendingChanges() {
    KillTimer(g_hwndMain, TIMER_AUTOSAVE);
    if (!Sci(SCI_GETMODIFY)) return true;
    std::wstring question = L"将修改保存到“";
    question += FileNamePtr(g_currentPath);
    question += L"”？";
    int result = MessageBoxW(g_hwndMain, question.c_str(), L"LightNote", MB_YESNOCANCEL | MB_ICONWARNING);
    if (result == IDCANCEL) return false;
    if (result == IDYES) return SaveDocument(false);
    RemoveRecovery();
    return true;
}

static void ResetAutoSaveTimer() {
    if (g_loading) return;
    KillTimer(g_hwndMain, TIMER_AUTOSAVE);
    SetTimer(g_hwndMain, TIMER_AUTOSAVE, AUTOSAVE_MS, nullptr);
    UpdateTitle(Sci(SCI_GETMODIFY)!=0);
}

static std::wstring NewNotePath() {
    std::wstring dir = JoinPath(g_appDir, L"data");
    EnsureDir(dir);

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t name[96] = {};
    swprintf(name, std::size(name), L"Note-%04u%02u%02u-%02u%02u%02u.txt",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return JoinPath(dir, name);
}

static void LoadDocument(const std::wstring& path) {
    g_loading = true;
    KillTimer(g_hwndMain, TIMER_AUTOSAVE);

    g_currentPath = path;
    std::string bytes;
    if (ReadAllBytes(path, bytes)) {
        Sci(SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(bytes.c_str()));
    } else {
        Sci(SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(""));
    }

    Sci(SCI_EMPTYUNDOBUFFER);
    Sci(SCI_SETSAVEPOINT);
    LoadHighlights();
    Sci(SCI_GOTOPOS, 0);
    g_lastBackupTick = 0;
    UpdateTitle(false);
    g_loading = false;
    std::string recovered;
    if (ReadAllBytes(RecoveryPath(),recovered)) {
        int choice=MessageBoxW(g_hwndMain,L"发现上次异常退出前未保存的文本。是否恢复到编辑器？\\n选择“是”只恢复编辑内容，不会覆盖原文件。",L"LightNote 恢复",MB_YESNO|MB_ICONQUESTION);
        if(choice==IDYES) {
            Sci(SCI_SETTEXT,0,reinterpret_cast<LPARAM>(recovered.c_str()));
            UpdateTitle(true);
            ResetAutoSaveTimer();
        } else RemoveRecovery();
    }
}

static void NewNote() {
    if (!ConfirmPendingChanges()) return;
    std::wstring path = NewNotePath();
    g_currentPath = path;
    g_loading = true;
    Sci(SCI_SETTEXT, 0, reinterpret_cast<LPARAM>(""));
    Sci(SCI_EMPTYUNDOBUFFER);
    Sci(SCI_SETSAVEPOINT);
    g_loading = false;
    UpdateTitle(false);
    SetFocus(g_hwndEdit);
}

static void OpenDocumentDialog() {

    wchar_t path[MAX_PATH * 4] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwndMain;
    ofn.lpstrFilter = L"文本文件\0*.txt;*.md;*.log;*.json;*.csv;*.ini;*.cfg;*.xml;*.yaml;*.yml\0所有文件\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = static_cast<DWORD>(std::size(path));
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    if (GetOpenFileNameW(&ofn) && ConfirmPendingChanges()) LoadDocument(path);
}

static void SaveAsDialog() {
    wchar_t path[MAX_PATH * 4] = {};
    if (!g_currentPath.empty()) wcsncpy(path, g_currentPath.c_str(), std::size(path) - 1);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwndMain;
    ofn.lpstrFilter = L"文本文件\0*.txt\0Markdown\0*.md\0所有文件\0*.*\0";
    ofn.lpstrFile = path;
    ofn.nMaxFile = static_cast<DWORD>(std::size(path));
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_EXPLORER;
    ofn.lpstrDefExt = L"txt";
    if (GetSaveFileNameW(&ofn)) {
        g_currentPath = path;
        SaveDocument(true);
    }
}

static void ApplyManualHighlight(int id) {
    LRESULT start = Sci(SCI_GETSELECTIONSTART);
    LRESULT end = Sci(SCI_GETSELECTIONEND);
    if (end < start) std::swap(start, end);
    if (end > start) {
        for (int colorId : INDICATORS) {
            if (colorId != INDIC_BOLD) ClearIndicator(colorId, start, end - start);
        }
        FillIndicator(id, start, end - start);
        SaveHighlights();
    }
}

static void ToggleSelectionBold() {
    LRESULT start = Sci(SCI_GETSELECTIONSTART);
    LRESULT end = Sci(SCI_GETSELECTIONEND);
    if (end <= start) return;
    bool allBold = true;
    for (LRESULT pos = start; pos < end; ++pos) {
        if (!Sci(SCI_INDICATORVALUEAT, INDIC_BOLD, pos)) { allBold = false; break; }
    }
    ClearIndicator(INDIC_BOLD, start, end-start);
    Sci(SCI_STARTSTYLING, static_cast<WPARAM>(start));
    Sci(SCI_SETSTYLING, static_cast<WPARAM>(end-start), allBold ? 0 : 1);
    if (!allBold) FillIndicator(INDIC_BOLD, start, end-start);
    SaveHighlights();
}

static void ClearSelectionHighlight() {
    LRESULT start = Sci(SCI_GETSELECTIONSTART);
    LRESULT end = Sci(SCI_GETSELECTIONEND);
    if (end < start) std::swap(start, end);
    if (end <= start) return;

    for (int id : INDICATORS) {
        if (id != INDIC_BOLD) ClearIndicator(id, start, end - start);
    }
    SaveHighlights();
}

static void ClearAllHighlights() {
    LRESULT len = Sci(SCI_GETLENGTH);
    for (int id : INDICATORS) {
        if (id != INDIC_BOLD) ClearIndicator(id, 0, len);
    }
    SaveHighlights();
}

static int SearchFlagsFromDialog(DWORD flags) {
    int searchFlags = 0;
    if (flags & FR_MATCHCASE) searchFlags |= SCFIND_MATCHCASE;
    if (flags & FR_WHOLEWORD) searchFlags |= SCFIND_WHOLEWORD;
    return searchFlags;
}

static bool SearchRange(const std::string& needle, LRESULT start, LRESULT end,
                        int searchFlags, bool selectResult) {
    if (needle.empty()) return false;

    Sci(SCI_SETSEARCHFLAGS, searchFlags);
    Sci(SCI_SETTARGETSTART, static_cast<WPARAM>(start));
    Sci(SCI_SETTARGETEND, static_cast<WPARAM>(end));
    LRESULT found = Sci(SCI_SEARCHINTARGET,
                        static_cast<WPARAM>(needle.size()),
                        reinterpret_cast<LPARAM>(needle.c_str()));
    if (found < 0) return false;

    if (selectResult) {
        LRESULT matchStart = Sci(SCI_GETTARGETSTART);
        LRESULT matchEnd = Sci(SCI_GETTARGETEND);
        Sci(SCI_SETSEL, static_cast<WPARAM>(matchStart), static_cast<LPARAM>(matchEnd));
        Sci(SCI_SCROLLCARET);
    }
    return true;
}

static bool FindNextFromDialog(DWORD flags) {
    std::string needle = WideToUtf8(g_findText);
    if (needle.empty()) return false;

    const int searchFlags = SearchFlagsFromDialog(flags);
    const LRESULT length = Sci(SCI_GETLENGTH);
    const LRESULT selStart = Sci(SCI_GETSELECTIONSTART);
    const LRESULT selEnd = Sci(SCI_GETSELECTIONEND);
    const bool forward = (flags & FR_DOWN) != 0;

    bool found = false;
    if (forward) {
        found = SearchRange(needle, selEnd, length, searchFlags, true);
        if (!found && selStart > 0) {
            found = SearchRange(needle, 0, selStart, searchFlags, true);
        }
    } else {
        found = SearchRange(needle, selStart, 0, searchFlags, true);
        if (!found && selEnd < length) {
            found = SearchRange(needle, length, selEnd, searchFlags, true);
        }
    }

    if (!found) MessageBeep(MB_ICONASTERISK);
    return found;
}

static bool SelectionMatchesFind(DWORD flags) {
    const LRESULT selStart = Sci(SCI_GETSELECTIONSTART);
    const LRESULT selEnd = Sci(SCI_GETSELECTIONEND);
    if (selEnd <= selStart) return false;

    std::string needle = WideToUtf8(g_findText);
    if (needle.empty()) return false;

    Sci(SCI_SETSEARCHFLAGS, SearchFlagsFromDialog(flags));
    Sci(SCI_SETTARGETSTART, static_cast<WPARAM>(selStart));
    Sci(SCI_SETTARGETEND, static_cast<WPARAM>(selEnd));
    LRESULT found = Sci(SCI_SEARCHINTARGET,
                        static_cast<WPARAM>(needle.size()),
                        reinterpret_cast<LPARAM>(needle.c_str()));
    return found == selStart && Sci(SCI_GETTARGETEND) == selEnd;
}

static void ReplaceOneFromDialog(DWORD flags) {
    if (SelectionMatchesFind(flags)) {
        std::string replacement = WideToUtf8(g_replaceText);
        Sci(SCI_REPLACESEL, 0, reinterpret_cast<LPARAM>(replacement.c_str()));
    }
    FindNextFromDialog(flags | FR_DOWN);
}

static int ReplaceAllFromDialog(DWORD flags) {
    std::string needle = WideToUtf8(g_findText);
    if (needle.empty()) return 0;
    std::string replacement = WideToUtf8(g_replaceText);

    const int searchFlags = SearchFlagsFromDialog(flags);
    LRESULT searchStart = 0;
    LRESULT length = Sci(SCI_GETLENGTH);
    int count = 0;

    Sci(SCI_BEGINUNDOACTION);
    while (searchStart <= length) {
        Sci(SCI_SETSEARCHFLAGS, searchFlags);
        Sci(SCI_SETTARGETSTART, static_cast<WPARAM>(searchStart));
        Sci(SCI_SETTARGETEND, static_cast<WPARAM>(length));
        LRESULT found = Sci(SCI_SEARCHINTARGET,
                            static_cast<WPARAM>(needle.size()),
                            reinterpret_cast<LPARAM>(needle.c_str()));
        if (found < 0) break;

        LRESULT matchStart = Sci(SCI_GETTARGETSTART);
        Sci(SCI_REPLACETARGET,
            static_cast<WPARAM>(replacement.size()),
            reinterpret_cast<LPARAM>(replacement.c_str()));
        searchStart = matchStart + static_cast<LRESULT>(replacement.size());
        length = Sci(SCI_GETLENGTH);
        ++count;
    }
    Sci(SCI_ENDUNDOACTION);

    if (count == 0) MessageBeep(MB_ICONASTERISK);
    return count;
}

static void ShowFindReplaceDialog(bool replace) {
    if (g_hwndFindDialog) {
        if (g_findDialogReplace == replace) {
            ShowWindow(g_hwndFindDialog, SW_SHOWNORMAL);
            SetForegroundWindow(g_hwndFindDialog);
            return;
        }
        SendMessageW(g_hwndFindDialog, WM_CLOSE, 0, 0);
        g_hwndFindDialog = nullptr;
    }

    ZeroMemory(&g_findReplace, sizeof(g_findReplace));
    g_findReplace.lStructSize = sizeof(g_findReplace);
    g_findReplace.hwndOwner = g_hwndMain;
    g_findReplace.lpstrFindWhat = g_findText;
    g_findReplace.wFindWhatLen = static_cast<WORD>(std::size(g_findText));
    g_findReplace.lpstrReplaceWith = g_replaceText;
    g_findReplace.wReplaceWithLen = static_cast<WORD>(std::size(g_replaceText));
    g_findReplace.Flags = FR_DOWN;

    g_findDialogReplace = replace;
    g_hwndFindDialog = replace ? ReplaceTextW(&g_findReplace) : FindTextW(&g_findReplace);
    if (!g_hwndFindDialog) {
        MessageBoxW(g_hwndMain, L"无法打开查找窗口。", L"LightNote", MB_OK | MB_ICONERROR);
    }
}

static void UpdateLineNumberMargin() {
    LRESULT lines = std::max<LRESULT>(1, Sci(SCI_GETLINECOUNT));
    int digits = 1;
    for (LRESULT n = lines; n >= 10; n /= 10) ++digits;

    std::string sample(static_cast<size_t>(digits), '9');
    int width = static_cast<int>(Sci(
        SCI_TEXTWIDTH,
        STYLE_LINENUMBER,
        reinterpret_cast<LPARAM>(sample.c_str()))) + 10;
    Sci(SCI_SETMARGINWIDTHN, 0, width);
}

static bool HasEditorSelection() {
    return Sci(SCI_GETSELECTIONSTART) != Sci(SCI_GETSELECTIONEND);
}

static void ExecuteEditorContextCommand(UINT command) {
    switch (command) {
    case IDM_CTX_CUT:
        Sci(SCI_CUT);
        break;
    case IDM_CTX_COPY:
        Sci(SCI_COPY);
        break;
    case IDM_CTX_PASTE:
        Sci(SCI_PASTE);
        break;
    case IDM_CTX_DELETE:
        Sci(SCI_CLEAR);
        break;
    case IDM_CTX_SELECT_ALL:
        Sci(SCI_SELECTALL);
        break;
    case IDM_HL_YELLOW:
        ApplyManualHighlight(INDIC_HL_YELLOW);
        break;
    case IDM_HL_GREEN:
        ApplyManualHighlight(INDIC_HL_GREEN);
        break;
    case IDM_HL_BLUE: ApplyManualHighlight(INDIC_HL_BLUE); break;
    case IDM_HL_RED: ApplyManualHighlight(INDIC_HL_RED); break;
    case IDM_HL_CYAN: ApplyManualHighlight(INDIC_HL_CYAN); break;
    case IDM_HL_INDIGO: ApplyManualHighlight(INDIC_HL_INDIGO); break;
    case IDM_HL_PURPLE: ApplyManualHighlight(INDIC_HL_PURPLE); break;
    case IDM_BOLD: ToggleSelectionBold(); break;
    case IDM_HL_CLEAR_SELECTION:
        ClearSelectionHighlight();
        break;
    }
}

static void ShowEditorContextMenu(HWND hwnd, LPARAM lParam) {
    const bool hasSelection = HasEditorSelection();
    const bool canPaste = Sci(SCI_CANPASTE) != 0;
    const bool hasText = Sci(SCI_GETLENGTH) > 0;

    HMENU menu = CreatePopupMenu();
    HMENU highlight = CreatePopupMenu();
    if (!menu || !highlight) {
        if (highlight) DestroyMenu(highlight);
        if (menu) DestroyMenu(menu);
        return;
    }

    const UINT selectionFlags = MF_STRING | (hasSelection ? MF_ENABLED : MF_GRAYED);
    AppendMenuW(menu, selectionFlags, IDM_CTX_CUT, L"剪切\tCtrl+X");
    AppendMenuW(menu, selectionFlags, IDM_CTX_COPY, L"复制\tCtrl+C");
    AppendMenuW(menu, MF_STRING | (canPaste ? MF_ENABLED : MF_GRAYED),
                IDM_CTX_PASTE, L"粘贴\tCtrl+V");
    AppendMenuW(menu, selectionFlags, IDM_CTX_DELETE, L"删除");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (hasText ? MF_ENABLED : MF_GRAYED),
                IDM_CTX_SELECT_ALL, L"全选\tCtrl+A");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(menu, selectionFlags, IDM_BOLD, L"加粗");
    AppendMenuW(highlight, selectionFlags, IDM_HL_RED, L"红色");
    AppendMenuW(highlight, selectionFlags, IDM_HL_YELLOW, L"黄色");
    AppendMenuW(highlight, selectionFlags, IDM_HL_GREEN, L"绿色");
    AppendMenuW(highlight, selectionFlags, IDM_HL_BLUE, L"蓝色");
    AppendMenuW(highlight, selectionFlags, IDM_HL_CYAN, L"青色");
    AppendMenuW(highlight, selectionFlags, IDM_HL_INDIGO, L"靛蓝色");
    AppendMenuW(highlight, selectionFlags, IDM_HL_PURPLE, L"紫色");
    AppendMenuW(highlight, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(highlight, selectionFlags, IDM_HL_CLEAR_SELECTION, L"清除选区高亮");
    AppendMenuW(menu, MF_POPUP | (hasSelection ? MF_ENABLED : MF_GRAYED),
                reinterpret_cast<UINT_PTR>(highlight), L"高亮");

    POINT pt = {};
    if (static_cast<int>(lParam) == -1) {
        GetCursorPos(&pt);
    } else {
        pt.x = static_cast<short>(LOWORD(lParam));
        pt.y = static_cast<short>(HIWORD(lParam));
    }

    const UINT command = TrackPopupMenu(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        pt.x,
        pt.y,
        0,
        hwnd,
        nullptr);

    if (command != 0) {
        ExecuteEditorContextCommand(command);
    }

    DestroyMenu(menu);
}

static LRESULT CALLBACK EditWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_CONTEXTMENU) {
        ShowEditorContextMenu(hwnd, lParam);
        return 0;
    }
    return CallWindowProcW(g_originalEditWndProc, hwnd, msg, wParam, lParam);
}

static HMENU CreateMainMenu() {
    HMENU menu = CreateMenu();

    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_NEW, L"新建\tCtrl+N");
    AppendMenuW(file, MF_STRING, IDM_OPEN, L"打开...\tCtrl+O");
    AppendMenuW(file, MF_STRING, IDM_SAVE, L"保存\tCtrl+S");
    AppendMenuW(file, MF_STRING, IDM_SAVEAS, L"另存为...\tCtrl+Shift+S");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"退出");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"文件");

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, IDM_UNDO, L"撤销\tCtrl+Z");
    AppendMenuW(edit, MF_STRING, IDM_REDO, L"重做\tCtrl+Y");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit), L"编辑");

    HMENU search = CreatePopupMenu();
    AppendMenuW(search, MF_STRING, IDM_FIND, L"查找...\tCtrl+F");
    AppendMenuW(search, MF_STRING, IDM_REPLACE, L"替换...\tCtrl+H");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(search), L"查找");

    HMENU hl = CreatePopupMenu();
    AppendMenuW(hl, MF_STRING, IDM_HL_RED, L"红色高亮");
    AppendMenuW(hl, MF_STRING, IDM_HL_YELLOW, L"黄色高亮\tCtrl+Shift+1");
    AppendMenuW(hl, MF_STRING, IDM_HL_GREEN, L"绿色高亮\tCtrl+Shift+2");
    AppendMenuW(hl, MF_STRING, IDM_HL_BLUE, L"蓝色高亮\tCtrl+Shift+3");
    AppendMenuW(hl, MF_STRING, IDM_HL_CYAN, L"青色高亮");
    AppendMenuW(hl, MF_STRING, IDM_HL_INDIGO, L"靛蓝色高亮");
    AppendMenuW(hl, MF_STRING, IDM_HL_PURPLE, L"紫色高亮");
    AppendMenuW(hl, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hl, MF_STRING, IDM_HL_CLEAR_SELECTION, L"清除选区高亮\tCtrl+Shift+0");
    AppendMenuW(hl, MF_STRING, IDM_HL_CLEAR_ALL, L"清除全部手动高亮");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(hl), L"高亮");

    HMENU history = CreatePopupMenu();
    AppendMenuW(history, MF_STRING, IDM_OPEN_BACKUPS, L"打开历史备份目录");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(history), L"历史");
    AppendMenuW(menu, MF_STRING, IDM_UNDO, L"↶ 撤销");
    AppendMenuW(menu, MF_STRING, IDM_REDO, L"↷ 重做");
    AppendMenuW(menu, MF_STRING, IDM_SAVE, L"保存");

    return menu;
}

static void ConfigureEditor() {
    Sci(SCI_SETCODEPAGE, SC_CP_UTF8);
    Sci(SCI_SETWRAPMODE, SC_WRAP_WORD);
    Sci(SCI_SETWRAPVISUALFLAGS, SC_WRAPVISUALFLAG_END);
    Sci(SCI_STYLESETFONT, STYLE_DEFAULT, reinterpret_cast<LPARAM>("Microsoft YaHei UI"));
    Sci(SCI_STYLESETSIZE, STYLE_DEFAULT, 11);
    Sci(SCI_STYLESETFORE, STYLE_DEFAULT, RGB(35, 35, 35));
    Sci(SCI_STYLESETBACK, STYLE_DEFAULT, RGB(255, 255, 255));
    Sci(SCI_STYLECLEARALL);
    Sci(SCI_STYLESETBOLD, 1, TRUE);
    Sci(SCI_STYLESETFONT, 1, reinterpret_cast<LPARAM>("Microsoft YaHei UI"));
    Sci(SCI_STYLESETSIZE, 1, 11);
    Sci(SCI_STYLESETFONT, STYLE_LINENUMBER, reinterpret_cast<LPARAM>("Microsoft YaHei UI"));
    Sci(SCI_STYLESETSIZE, STYLE_LINENUMBER, 10);
    Sci(SCI_STYLESETFORE, STYLE_LINENUMBER, RGB(115, 115, 115));
    Sci(SCI_STYLESETBACK, STYLE_LINENUMBER, RGB(242, 242, 242));
    Sci(SCI_SETMARGINTYPEN, 0, SC_MARGIN_NUMBER);
    Sci(SCI_SETMARGINBACKN, 0, RGB(242, 242, 242));
    Sci(SCI_SETMARGINWIDTHN, 1, 0);
    UpdateLineNumberMargin();
    Sci(SCI_SETCARETLINEVISIBLE, FALSE);
    Sci(SCI_SETSELFORE, TRUE, RGB(255, 255, 255));
    Sci(SCI_SETSELBACK, TRUE, RGB(40, 110, 210));
    Sci(SCI_SETUSETABS, FALSE);
    Sci(SCI_SETTABWIDTH, 4);
    Sci(SCI_SETSCROLLWIDTHTRACKING, TRUE);
    Sci(SCI_USEPOPUP, SC_POPUP_NEVER);
    Sci(SCI_SETIMEINTERACTION, SC_IME_INLINE);
    ConfigureIndicators();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (g_findReplaceMsg != 0 && msg == g_findReplaceMsg) {
        FINDREPLACEW* fr = reinterpret_cast<FINDREPLACEW*>(lParam);
        if (!fr) return 0;
        if (fr->Flags & FR_DIALOGTERM) {
            g_hwndFindDialog = nullptr;
            return 0;
        }
        if (fr->Flags & FR_FINDNEXT) {
            FindNextFromDialog(fr->Flags);
            return 0;
        }
        if (fr->Flags & FR_REPLACEALL) {
            ReplaceAllFromDialog(fr->Flags);
            return 0;
        }
        if (fr->Flags & FR_REPLACE) {
            ReplaceOneFromDialog(fr->Flags);
            return 0;
        }
    }

    switch (msg) {
    case WM_CREATE: {
        g_hwndEdit = CreateWindowExW(
            0, L"Scintilla", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPCHILDREN | WS_VSCROLL | WS_HSCROLL,
            0, 0, 0, 0, hwnd, nullptr, g_hInst, nullptr);
        if (!g_hwndEdit) return -1;

        g_originalEditWndProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(g_hwndEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditWndProc)));
        if (!g_originalEditWndProc) return -1;

        ConfigureEditor();

        g_hwndUndoButton = CreateWindowW(L"BUTTON", L"撤销", WS_CHILD | BS_OWNERDRAW,
            6, 4, 82, 27, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDM_UNDO)), g_hInst, nullptr);
        g_hwndRedoButton = CreateWindowW(L"BUTTON", L"重做", WS_CHILD | BS_OWNERDRAW,
            94, 4, 82, 27, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDM_REDO)), g_hInst, nullptr);
        g_hwndSaveButton = CreateWindowW(L"BUTTON", L"保存", WS_CHILD | BS_OWNERDRAW,
            182, 4, 72, 27, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDM_SAVE)), g_hInst, nullptr);

        if (!g_startPath.empty()) {
            LoadDocument(g_startPath);
        } else {
            std::wstring dataDir = JoinPath(g_appDir, L"data");
            EnsureDir(dataDir);
            std::wstring quick = JoinPath(dataDir, L"QuickNote.txt");
            if (GetFileAttributesW(quick.c_str()) == INVALID_FILE_ATTRIBUTES) {
                WriteAllBytesAtomic(quick, std::string());
            }
            LoadDocument(quick);
        }
        return 0;
    }

    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* d = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!d || (d->CtlID != IDM_UNDO && d->CtlID != IDM_REDO && d->CtlID != IDM_SAVE)) break;
        HDC dc = d->hDC;
        RECT rect = d->rcItem;
        HBRUSH bg = CreateSolidBrush((d->itemState & ODS_SELECTED) ? RGB(225,239,253) : RGB(255,255,255));
        FillRect(dc, &rect, bg);
        DeleteObject(bg);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(35,35,35));
        HFONT font = CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        HGDIOBJ oldFont = SelectObject(dc,font);
        const bool save = d->CtlID == IDM_SAVE;
        const bool redo = d->CtlID == IDM_REDO;
        if (!save) {
            HPEN pen = CreatePen(PS_SOLID,3,RGB(43,119,220));
            HGDIOBJ oldPen = SelectObject(dc,pen);
            POINT pts[5];
            int x = rect.left + 10;
            int y = (rect.top + rect.bottom)/2;
            if (redo) {
                pts[0]={x,y+5};pts[1]={x+2,y-3};pts[2]={x+10,y-7};pts[3]={x+19,y-3};pts[4]={x+24,y+4};
            } else {
                pts[0]={x+24,y+5};pts[1]={x+22,y-3};pts[2]={x+14,y-7};pts[3]={x+5,y-3};pts[4]={x,y+4};
            }
            Polyline(dc,pts,5);
            HBRUSH arrowBrush=CreateSolidBrush(RGB(43,119,220));
            HGDIOBJ oldBrush=SelectObject(dc,arrowBrush);
            POINT arrow[3];
            if(redo){arrow[0]={x+17,y+3};arrow[1]={x+29,y+4};arrow[2]={x+22,y+12};}
            else {arrow[0]={x+7,y+3};arrow[1]={x-5,y+4};arrow[2]={x+2,y+12};}
            Polygon(dc,arrow,3);
            SelectObject(dc,oldBrush);DeleteObject(arrowBrush);
            SelectObject(dc,oldPen);DeleteObject(pen);
        }
        RECT textRect=rect;
        textRect.left += save ? 0 : 31;
        DrawTextW(dc,save ? L"保存" : (redo ? L"重做" : L"撤销"),-1,&textRect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(dc,oldFont);DeleteObject(font);
        return TRUE;
    }

    case WM_SIZE:
        if (g_hwndEdit) {
            MoveWindow(g_hwndEdit, 0, 0, LOWORD(lParam), HIWORD(lParam), TRUE);
        }
        return 0;

    case WM_SETFOCUS:
        if (g_hwndEdit) SetFocus(g_hwndEdit);
        return 0;

    case WM_NOTIFY: {
        SCNotification* scn = reinterpret_cast<SCNotification*>(lParam);
        if (scn && scn->nmhdr.hwndFrom == g_hwndEdit) {
            if (scn->nmhdr.code == SCN_MODIFIED) {
                if (scn->modificationType & (SC_MOD_INSERTTEXT | SC_MOD_DELETETEXT)) {
                    UpdateLineNumberMargin();
                    ResetAutoSaveTimer();
                }
            }
        }
        break;
    }

    case WM_TIMER:
        if (wParam == TIMER_AUTOSAVE) {
            KillTimer(hwnd, TIMER_AUTOSAVE);
            SnapshotRecovery();
            return 0;
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_UNDO: Sci(SCI_UNDO); return 0;
        case IDM_REDO: Sci(SCI_REDO); return 0;
        case IDM_NEW: NewNote(); return 0;
        case IDM_OPEN: OpenDocumentDialog(); return 0;
        case IDM_SAVE: SaveDocument(true); return 0;
        case IDM_SAVEAS: SaveAsDialog(); return 0;
        case IDM_FIND: ShowFindReplaceDialog(false); return 0;
        case IDM_REPLACE: ShowFindReplaceDialog(true); return 0;
        case IDM_EXIT: SendMessageW(hwnd, WM_CLOSE, 0, 0); return 0;
        case IDM_HL_YELLOW: ApplyManualHighlight(INDIC_HL_YELLOW); return 0;
        case IDM_HL_GREEN: ApplyManualHighlight(INDIC_HL_GREEN); return 0;
        case IDM_HL_BLUE: ApplyManualHighlight(INDIC_HL_BLUE); return 0;
        case IDM_HL_RED: ApplyManualHighlight(INDIC_HL_RED); return 0;
        case IDM_HL_CYAN: ApplyManualHighlight(INDIC_HL_CYAN); return 0;
        case IDM_HL_INDIGO: ApplyManualHighlight(INDIC_HL_INDIGO); return 0;
        case IDM_HL_PURPLE: ApplyManualHighlight(INDIC_HL_PURPLE); return 0;
        case IDM_BOLD: ToggleSelectionBold(); return 0;
        case IDM_HL_CLEAR_SELECTION: ClearSelectionHighlight(); return 0;
        case IDM_HL_CLEAR_ALL: ClearAllHighlights(); return 0;
        case IDM_OPEN_BACKUPS: {
            std::wstring dir = BackupDir();
            ShellExecuteW(hwnd, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return 0;
        }
        }
        break;

    case WM_CLOSE:
        if (!ConfirmPendingChanges()) return 0;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static HACCEL CreateAccelerators() {
    ACCEL accels[] = {
        { FCONTROL | FVIRTKEY, 'Z', IDM_UNDO },
        { FCONTROL | FVIRTKEY, 'Y', IDM_REDO },
        { FCONTROL | FVIRTKEY, 'N', IDM_NEW },
        { FCONTROL | FVIRTKEY, 'O', IDM_OPEN },
        { FCONTROL | FVIRTKEY, 'S', IDM_SAVE },
        { FCONTROL | FSHIFT | FVIRTKEY, 'S', IDM_SAVEAS },
        { FCONTROL | FVIRTKEY, 'F', IDM_FIND },
        { FCONTROL | FVIRTKEY, 'H', IDM_REPLACE },
        { FCONTROL | FSHIFT | FVIRTKEY, '1', IDM_HL_YELLOW },
        { FCONTROL | FSHIFT | FVIRTKEY, '2', IDM_HL_GREEN },
        { FCONTROL | FSHIFT | FVIRTKEY, '3', IDM_HL_BLUE },
        { FCONTROL | FSHIFT | FVIRTKEY, '0', IDM_HL_CLEAR_SELECTION }
    };
    return CreateAcceleratorTableW(accels, static_cast<int>(std::size(accels)));
}

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInst = hInstance;
    g_findReplaceMsg = RegisterWindowMessageW(L"commdlg_FindReplace");
    g_appDir = GetExeDir();
    EnsureDir(JoinPath(g_appDir, L"data"));
    EnsureDir(JoinPath(g_appDir, L"backups"));
    EnsureDir(JoinPath(g_appDir, L"meta"));

    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) {
        wchar_t full[MAX_PATH * 4] = {};
        if (GetFullPathNameW(argv[1], static_cast<DWORD>(std::size(full)), full, nullptr)) {
            g_startPath = full;
        }
    }
    if (argv) LocalFree(argv);

    if (!Scintilla_RegisterClasses(hInstance)) {
        MessageBoxW(nullptr, L"Scintilla 初始化失败。", L"LightNote", MB_OK | MB_ICONERROR);
        return 1;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = hInstance;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"LightNoteMainWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&wc)) return 2;

    g_hwndMain = CreateWindowExW(
        0, wc.lpszClassName, L"LightNote",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 700,
        nullptr, CreateMainMenu(), hInstance, nullptr);

    if (!g_hwndMain) return 3;

    UpdateTitle(Sci(SCI_GETMODIFY) != 0);
    ShowWindow(g_hwndMain, nCmdShow);
    UpdateWindow(g_hwndMain);
    SetFocus(g_hwndEdit);

    HACCEL accel = CreateAccelerators();
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!TranslateAcceleratorW(g_hwndMain, accel, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (accel) DestroyAcceleratorTable(accel);

    return static_cast<int>(msg.wParam);
}
