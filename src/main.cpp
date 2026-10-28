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
#include <commctrl.h>
#include <ole2.h>

#include "Scintilla.h"

static HINSTANCE g_hInst = nullptr;
static HWND g_hwndMain = nullptr;
static HWND g_hwndEdit = nullptr;
static HWND g_hwndFiles = nullptr;
static HWND g_hwndSplitter = nullptr;
static WNDPROC g_originalSplitterWndProc = nullptr;
static bool g_resizingSidebar = false;
static WNDPROC g_originalListWndProc = nullptr;
static int g_dragStart = -1;
static int g_activeDoc = -1;
static HWND g_hwndRenameEdit = nullptr;
static WNDPROC g_originalRenameWndProc = nullptr;
static int g_renameIndex = -1;
struct OpenNote { std::wstring path; HWND editor = nullptr; DWORD lastBackupTick = 0; };
static std::vector<OpenNote> g_notes;
static constexpr int SIDEBAR_DEFAULT = 194;
static constexpr int SIDEBAR_MIN = 112;
static constexpr int SIDEBAR_MAX = 620;
static constexpr int SPLITTER_WIDTH = 7;
static int g_sidebarWidth = SIDEBAR_DEFAULT;
static constexpr int IDM_FILELIST = 1800;
static constexpr int IDM_CLOSE_DOC = 1801;
static constexpr int IDM_RENAME_DOC = 1802;
static constexpr int IDM_RENAME_EDIT = 1803;

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

static void SnapshotRecovery();
static void UpdateLineNumberMargin();
static void ConfigureEditor();
static void LoadDocument(const std::wstring&);
static bool ConfirmPendingChanges();
static std::wstring NewNotePath();
static LRESULT CALLBACK EditWndProc(HWND,UINT,WPARAM,LPARAM);
static void LayoutEditors(HWND);
static bool OpenDocumentTab(const std::wstring&);
static void SelectDocument(int);
static bool CloseDocumentAt(int);
static void MoveDocument(int,int);
static void BeginRenameDocument(int);
static void EndRenameEdit(bool);
static LRESULT CALLBACK RenameEditWndProc(HWND,UINT,WPARAM,LPARAM);
static void HandleDroppedFiles(HDROP);
static void InstallEditorDropTarget(HWND);
static LRESULT CALLBACK SplitterWndProc(HWND,UINT,WPARAM,LPARAM);
static LRESULT CALLBACK ListWndProc(HWND,UINT,WPARAM,LPARAM);
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

static std::wstring HighlightFileName() {
    wchar_t name[64] = {};
    swprintf(name, std::size(name), L"%016llx.hl",
             static_cast<unsigned long long>(HashPath(g_currentPath)));
    return name;
}

static std::wstring LegacyMetaPath() {
    return JoinPath(JoinPath(g_appDir, L"meta"), HighlightFileName());
}

// Use a per-user location independent of where an EXE version was extracted.
// Keep a legacy fallback below for portable installations predating V0.8.3.
static std::wstring MetaPath() {
    wchar_t localData[MAX_PATH * 4] = {};
    const DWORD length = GetEnvironmentVariableW(
        L"LOCALAPPDATA", localData, static_cast<DWORD>(std::size(localData)));
    if (length == 0 || length >= std::size(localData)) {
        const std::wstring oldDir = JoinPath(g_appDir, L"meta");
        EnsureDir(oldDir);
        return LegacyMetaPath();
    }
    const std::wstring dir = JoinPath(JoinPath(localData, L"LightNote"), L"meta");
    EnsureDir(dir);
    return JoinPath(dir, HighlightFileName());
}

static std::wstring HighlightReadPath() {
    const std::wstring shared = MetaPath();
    if (GetFileAttributesW(shared.c_str()) != INVALID_FILE_ATTRIBUTES) return shared;
    const std::wstring legacy = LegacyMetaPath();
    if (_wcsicmp(shared.c_str(), legacy.c_str()) == 0) return shared;
    if (GetFileAttributesW(legacy.c_str()) == INVALID_FILE_ATTRIBUTES) return shared;
    // One-time migration for an existing local highlight file; never overwrite.
    if (CopyFileW(legacy.c_str(), shared.c_str(), TRUE)) return shared;
    return legacy; // Preserve visibility if the migration failed.
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
    std::wstring title = L"LightNote V0.8.3";
    if (!g_currentPath.empty()) {
        title += L" - ";
        title += FileNamePtr(g_currentPath);
    }
    if (dirty) title += L" *";
    if (g_hwndMain) SetWindowTextW(g_hwndMain, title.c_str());
    if (g_hwndFiles && g_activeDoc >= 0 && g_activeDoc < static_cast<int>(g_notes.size())) {
        std::wstring item = FileNamePtr(g_currentPath);
        if (dirty) item += L"  *";
        const LRESULT length = SendMessageW(g_hwndFiles, LB_GETTEXTLEN, g_activeDoc, 0);
        std::wstring previous;
        if (length != LB_ERR && length >= 0) {
            previous.resize(static_cast<size_t>(length) + 1);
            SendMessageW(g_hwndFiles, LB_GETTEXT, g_activeDoc,
                         reinterpret_cast<LPARAM>(&previous[0]));
            previous.resize(static_cast<size_t>(length));
        }
        if (previous != item) {
            SendMessageW(g_hwndFiles, LB_DELETESTRING, g_activeDoc, 0);
            SendMessageW(g_hwndFiles, LB_INSERTSTRING, g_activeDoc,
                         reinterpret_cast<LPARAM>(item.c_str()));
        }
        SendMessageW(g_hwndFiles, LB_SETCURSEL, g_activeDoc, 0);
    }
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

    FILE* f = _wfopen(HighlightReadPath().c_str(), L"rb");
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

// Resolve relative paths and slash variants before comparing open documents.
static std::wstring NormalizeDocumentPath(const std::wstring& raw) {
    if (raw.empty()) return {};
    std::wstring candidate = raw;
    std::replace(candidate.begin(), candidate.end(), L'/', L'\\');
    const DWORD required = GetFullPathNameW(candidate.c_str(), 0, nullptr, nullptr);
    if (required == 0 || required > 32767) return {};
    std::wstring full(static_cast<size_t>(required) + 1, L'\0');
    const DWORD length = GetFullPathNameW(candidate.c_str(),
        static_cast<DWORD>(full.size()), &full[0], nullptr);
    if (length == 0 || length >= full.size()) return {};
    full.resize(length);
    return full;
}

// Two differently spelled paths may refer to one file (8.3 name, junction, hard link).
static bool SameDocumentPath(const std::wstring& first, const std::wstring& second) {
    if (first.empty() || second.empty()) return false;
    if (_wcsicmp(first.c_str(), second.c_str()) == 0) return true;
    constexpr DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    HANDLE a = CreateFileW(first.c_str(), FILE_READ_ATTRIBUTES, share,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE b = CreateFileW(second.c_str(), FILE_READ_ATTRIBUTES, share,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    BY_HANDLE_FILE_INFORMATION ai = {}, bi = {};
    const bool same = a != INVALID_HANDLE_VALUE && b != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(a, &ai) && GetFileInformationByHandle(b, &bi) &&
        ai.dwVolumeSerialNumber == bi.dwVolumeSerialNumber &&
        ai.nFileIndexHigh == bi.nFileIndexHigh && ai.nFileIndexLow == bi.nFileIndexLow;
    if (a != INVALID_HANDLE_VALUE) CloseHandle(a);
    if (b != INVALID_HANDLE_VALUE) CloseHandle(b);
    return same;
}

static int FindOpenDocument(const std::wstring& path) {
    for (int i = 0; i < static_cast<int>(g_notes.size()); ++i)
        if (SameDocumentPath(g_notes[i].path, path)) return i;
    return -1;
}

static std::wstring NewNotePath() {
    std::wstring dir = JoinPath(g_appDir, L"data");
    EnsureDir(dir);

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t name[96] = {};
    swprintf(name, std::size(name), L"Note-%04u%02u%02u-%02u%02u%02u.txt",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::wstring candidate = JoinPath(dir, name);
    int suffix = 2;
    while (FindOpenDocument(candidate) >= 0 ||
           GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring base(name);
        if (base.size() >= 4) base.resize(base.size() - 4);
        candidate = JoinPath(dir, base + L"-" + std::to_wstring(suffix++) + L".txt");
    }
    return candidate;
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

static std::wstring SettingsPath() {
    return JoinPath(g_appDir, L"settings.ini");
}

static void LayoutEditors(HWND owner) {
    RECT rc = {};
    GetClientRect(owner, &rc);
    const int width = static_cast<int>(rc.right - rc.left);
    const int height = static_cast<int>(rc.bottom - rc.top);
    const int sidebar = std::max(0, std::min(g_sidebarWidth, width - 230));
    const int editorX = sidebar + SPLITTER_WIDTH;
    if (g_hwndFiles) MoveWindow(g_hwndFiles, 0, 0, sidebar, height, TRUE);
    if (g_hwndSplitter) MoveWindow(g_hwndSplitter, sidebar, 0, SPLITTER_WIDTH, height, TRUE);
    for (auto& note : g_notes)
        if (note.editor) MoveWindow(note.editor, editorX, 0,
                                    std::max(0, width - editorX), height, TRUE);
}

static LRESULT CALLBACK SplitterWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return TRUE;
    case WM_LBUTTONDOWN:
        g_resizingSidebar = true;
        SetCapture(hwnd);
        SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        return 0;
    case WM_MOUSEMOVE:
        if (g_resizingSidebar && GetCapture() == hwnd) {
            POINT cursor = {};
            GetCursorPos(&cursor);
            ScreenToClient(g_hwndMain, &cursor);
            RECT rc = {};
            GetClientRect(g_hwndMain, &rc);
            const int maxSide = std::max(0, std::min(SIDEBAR_MAX,
                                               static_cast<int>(rc.right) - 230));
            const int minSide = std::min(SIDEBAR_MIN, maxSide);
            const int newWidth = std::max(minSide,
                              std::min(static_cast<int>(cursor.x) - SPLITTER_WIDTH / 2, maxSide));
            if (newWidth != g_sidebarWidth) {
                g_sidebarWidth = newWidth;
                LayoutEditors(g_hwndMain);
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_resizingSidebar) {
            g_resizingSidebar = false;
            ReleaseCapture();
            const std::wstring value = std::to_wstring(g_sidebarWidth);
            WritePrivateProfileStringW(L"Layout", L"SidebarWidth", value.c_str(),
                                       SettingsPath().c_str());
        }
        return 0;
    case WM_CAPTURECHANGED:
        g_resizingSidebar = false;
        return 0;
    case WM_ERASEBKGND:
        return TRUE;
    case WM_PAINT: {
        PAINTSTRUCT ps = {};
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        HBRUSH background = CreateSolidBrush(RGB(227, 231, 237));
        FillRect(dc, &rc, background);
        DeleteObject(background);
        HPEN grip = CreatePen(PS_SOLID, 1, RGB(151, 160, 172));
        HGDIOBJ previousPen = SelectObject(dc, grip);
        const int cx = (rc.left + rc.right) / 2;
        const int cy = (rc.top + rc.bottom) / 2;
        MoveToEx(dc, cx, cy - 17, nullptr);
        LineTo(dc, cx, cy + 18);
        SelectObject(dc, previousPen);
        DeleteObject(grip);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DROPFILES:
        SendMessageW(g_hwndMain, WM_DROPFILES, wParam, lParam);
        return 0;
    }
    return CallWindowProcW(g_originalSplitterWndProc, hwnd, msg, wParam, lParam);
}
static void SelectDocument(int index) {
    if(index<0 || index>=static_cast<int>(g_notes.size()) || index==g_activeDoc) return;
    if(g_hwndEdit && g_activeDoc>=0) {
        SnapshotRecovery();
        g_notes[g_activeDoc].lastBackupTick=g_lastBackupTick;
        ShowWindow(g_hwndEdit,SW_HIDE);
    }
    g_activeDoc=index;
    g_hwndEdit=g_notes[index].editor;
    g_currentPath=g_notes[index].path;
    g_lastBackupTick=g_notes[index].lastBackupTick;
    ShowWindow(g_hwndEdit,SW_SHOW);
    SendMessageW(g_hwndFiles,LB_SETCURSEL,index,0);
    UpdateTitle(Sci(SCI_GETMODIFY)!=0);
    UpdateLineNumberMargin();
    SetFocus(g_hwndEdit);
}
static bool OpenDocumentTab(const std::wstring& path) {
    const std::wstring normalized = NormalizeDocumentPath(path);
    if (normalized.empty()) return false;
    const DWORD attributes = GetFileAttributesW(normalized.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
        return false;
    const int existing = FindOpenDocument(normalized);
    if (existing >= 0) {
        SelectDocument(existing);
        return true;
    }
    if(g_hwndEdit) SnapshotRecovery();
    HWND editor=CreateWindowExW(WS_EX_ACCEPTFILES,L"Scintilla",L"",
        WS_CHILD|WS_TABSTOP|WS_CLIPCHILDREN|WS_VSCROLL|WS_HSCROLL,
        0,0,0,0,g_hwndMain,nullptr,g_hInst,nullptr);
    if(!editor) return false;
    DragAcceptFiles(editor, TRUE);
    InstallEditorDropTarget(editor);
    WNDPROC old=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(editor,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(EditWndProc)));
    if(!g_originalEditWndProc) g_originalEditWndProc=old;
    HWND previous=g_hwndEdit;
    g_hwndEdit=editor;
    ConfigureEditor();
    g_notes.push_back({normalized,editor,0});
    g_activeDoc=static_cast<int>(g_notes.size())-1;
    SendMessageW(g_hwndFiles,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(FileNamePtr(normalized)));
    LoadDocument(normalized);
    if(previous) ShowWindow(previous,SW_HIDE);
    LayoutEditors(g_hwndMain);
    ShowWindow(editor,SW_SHOW);
    SetFocus(editor);
    return true;
}

// WM_DROPFILES owns its HDROP, but an OLE STGMEDIUM owns its HGLOBAL.
// Both use the same path enumeration while releasing only their own resource.
static void OpenDroppedPaths(HDROP drop) {
    if (!drop) return;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count; ++i) {
        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
        if (!length || length > 32767) continue;
        std::vector<wchar_t> path(static_cast<size_t>(length) + 1, L'\0');
        if (DragQueryFileW(drop, i, path.data(), length + 1) != length) continue;
        const DWORD attributes = GetFileAttributesW(path.data());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        OpenDocumentTab(path.data());
    }
}

static void HandleDroppedFiles(HDROP drop) {
    if (!drop) return;
    OpenDroppedPaths(drop);
    DragFinish(drop);
}

class EditorFileDropTarget final : public IDropTarget {
    volatile LONG references_ = 1;
    HWND editor_ = nullptr;
    bool files_ = false;
    bool text_ = false;
    bool allowText_ = true;

    static FORMATETC Format(CLIPFORMAT kind) {
        FORMATETC format = {};
        format.cfFormat = kind;
        format.dwAspect = DVASPECT_CONTENT;
        format.lindex = -1;
        format.tymed = TYMED_HGLOBAL;
        return format;
    }

public:
    explicit EditorFileDropTarget(HWND editor, bool allowText = true)
        : editor_(editor), allowText_(allowText) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result) return E_POINTER;
        *result = nullptr;
        if (IsEqualIID(iid, IID_IUnknown) || IsEqualIID(iid, IID_IDropTarget)) {
            *result = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&references_);
        if (!count) delete this;
        return static_cast<ULONG>(count);
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL,
                                        DWORD* effect) override {
        if (!effect) return E_POINTER;
        if (!data) { *effect = DROPEFFECT_NONE; return E_INVALIDARG; }
        FORMATETC files = Format(CF_HDROP);
        FORMATETC text = Format(CF_UNICODETEXT);
        files_ = SUCCEEDED(data->QueryGetData(&files));
        text_ = allowText_ && !files_ && SUCCEEDED(data->QueryGetData(&text));
        *effect = (files_ || text_) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL, DWORD* effect) override {
        if (!effect) return E_POINTER;
        *effect = (files_ || text_) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        files_ = text_ = false;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL point,
                                    DWORD* effect) override {
        if (!effect) return E_POINTER;
        *effect = DROPEFFECT_NONE;
        if (!data) return E_INVALIDARG;
        if (files_) {
            FORMATETC format = Format(CF_HDROP);
            STGMEDIUM medium = {};
            if (SUCCEEDED(data->GetData(&format, &medium))) {
                if (medium.tymed == TYMED_HGLOBAL) {
                    OpenDroppedPaths(reinterpret_cast<HDROP>(medium.hGlobal));
                    *effect = DROPEFFECT_COPY;
                }
                ReleaseStgMedium(&medium);
            }
        } else if (text_) {
            FORMATETC format = Format(CF_UNICODETEXT);
            STGMEDIUM medium = {};
            if (SUCCEEDED(data->GetData(&format, &medium))) {
                if (medium.tymed == TYMED_HGLOBAL) {
                    const wchar_t* value = static_cast<const wchar_t*>(
                        GlobalLock(medium.hGlobal));
                    if (value) {
                        const SIZE_T capacity = GlobalSize(medium.hGlobal) / sizeof(wchar_t);
                        const wchar_t* end = std::find(value, value + capacity, L'\0');
                        if (end != value + capacity) {
                            std::wstring wide(value, end);
                            std::string utf8 = WideToUtf8(wide.c_str());
                            POINT local = {point.x, point.y};
                            ScreenToClient(editor_, &local);
                            LRESULT pos = SendMessageW(editor_, SCI_POSITIONFROMPOINTCLOSE,
                                                       local.x, local.y);
                            if (pos < 0) pos = SendMessageW(editor_, SCI_GETCURRENTPOS, 0, 0);
                            SendMessageW(editor_, SCI_GOTOPOS, pos, 0);
                            SendMessageW(editor_, SCI_REPLACESEL, 0,
                                         reinterpret_cast<LPARAM>(utf8.c_str()));
                            *effect = DROPEFFECT_COPY;
                        }
                        GlobalUnlock(medium.hGlobal);
                    }
                }
                ReleaseStgMedium(&medium);
            }
        }
        files_ = text_ = false;
        return S_OK;
    }
};

static void InstallEditorDropTarget(HWND editor) {
    // Scintilla's original OLE target only recognizes Unicode text, not CF_HDROP.
    // Replace it with a target that opens Explorer files and also accepts text.
    RevokeDragDrop(editor);
    auto* target = new EditorFileDropTarget(editor);
    const HRESULT registered = RegisterDragDrop(editor, target);
    target->Release(); // OLE retains its own reference until RevokeDragDrop.
    if (FAILED(registered)) DragAcceptFiles(editor, TRUE);
}

static void InstallSidebarDropTarget(HWND list) {
    // Unlike Scintilla, a native LISTBOX has no OLE file drop target by default.
    auto* target = new EditorFileDropTarget(list, false);
    const HRESULT registered = RegisterDragDrop(list, target);
    target->Release();
    if (FAILED(registered)) DragAcceptFiles(list, TRUE);
}

static bool CloseDocumentAt(int index) {
    if(index<0 || index>=static_cast<int>(g_notes.size())) return false;
    SelectDocument(index);
    if(!ConfirmPendingChanges()) return false;
    HWND gone=g_notes[index].editor;
    g_notes.erase(g_notes.begin()+index);
    SendMessageW(g_hwndFiles,LB_DELETESTRING,index,0);
    ShowWindow(gone,SW_HIDE);
    DestroyWindow(gone);
    g_hwndEdit=nullptr; g_activeDoc=-1;
    if(!g_notes.empty()) SelectDocument(std::min(index,static_cast<int>(g_notes.size())-1));
    else OpenDocumentTab(NewNotePath());
    return true;
}
static void MoveDocument(int from,int to) {
    if(from<0||to<0||from>=static_cast<int>(g_notes.size())||to>=static_cast<int>(g_notes.size())||from==to)return;
    auto note=g_notes[from];
    g_notes.erase(g_notes.begin()+from);
    g_notes.insert(g_notes.begin()+to,note);
    g_activeDoc=to;
    std::wstring label=FileNamePtr(note.path);
    if(SendMessageW(note.editor,SCI_GETMODIFY,0,0))label+=L"  *";
    SendMessageW(g_hwndFiles,LB_DELETESTRING,from,0);
    SendMessageW(g_hwndFiles,LB_INSERTSTRING,to,reinterpret_cast<LPARAM>(label.c_str()));
    SendMessageW(g_hwndFiles,LB_SETCURSEL,to,0);
}
static LRESULT CALLBACK ListWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_DROPFILES) {
        SendMessageW(g_hwndMain, WM_DROPFILES, wParam, lParam);
        return 0;
    }
    if (msg == WM_CONTEXTMENU) {
        POINT screen = {};
        int index = -1;
        if (lParam == -1) {
            index = static_cast<int>(SendMessageW(hwnd, LB_GETCURSEL, 0, 0));
            RECT row = {};
            if (index < 0 || index >= static_cast<int>(g_notes.size()) ||
                SendMessageW(hwnd, LB_GETITEMRECT, index,
                             reinterpret_cast<LPARAM>(&row)) == LB_ERR) return 0;
            screen = {row.left + 16, row.bottom};
            ClientToScreen(hwnd, &screen);
        } else {
            screen.x = static_cast<short>(LOWORD(lParam));
            screen.y = static_cast<short>(HIWORD(lParam));
            POINT local = screen;
            ScreenToClient(hwnd, &local);
            const LPARAM packed = MAKELPARAM(local.x, local.y);
            const LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, packed);
            if (!HIWORD(hit)) index = static_cast<int>(LOWORD(hit));
        }
        if (index < 0 || index >= static_cast<int>(g_notes.size())) return 0;
        SelectDocument(index);
        HMENU menu = CreatePopupMenu();
        if (!menu) return 0;
        AppendMenuW(menu, MF_STRING, IDM_RENAME_DOC, L"重命名\tF2");
        const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                                            screen.x, screen.y, 0, g_hwndMain, nullptr);
        DestroyMenu(menu);
        if (command == IDM_RENAME_DOC) BeginRenameDocument(index);
        return 0;
    }
    if (msg == WM_KEYDOWN && wParam == VK_F2) {
        BeginRenameDocument(g_activeDoc);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN) {
        const LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lParam);
        const int index = HIWORD(hit) ? -1 : static_cast<int>(LOWORD(hit));
        if (index >= 0 && index < static_cast<int>(g_notes.size())) {
            g_dragStart = index;
            SelectDocument(index);
            SetCapture(hwnd);
            return 0; // Do not let the native list box start its own drag loop.
        }
    } else if (msg == WM_LBUTTONUP && g_dragStart >= 0) {
        const int from = g_dragStart;
        g_dragStart = -1;
        const LRESULT hit = SendMessageW(hwnd, LB_ITEMFROMPOINT, 0, lParam);
        const int to = HIWORD(hit) ? -1 : static_cast<int>(LOWORD(hit));
        if (GetCapture() == hwnd) ReleaseCapture();
        if (to >= 0 && to < static_cast<int>(g_notes.size()) && from != to) {
            MoveDocument(from, to);
        }
        return 0;
    } else if (msg == WM_CAPTURECHANGED) {
        g_dragStart = -1;
    }
    return CallWindowProcW(g_originalListWndProc, hwnd, msg, wParam, lParam);
}
static void NewNote() {
    OpenDocumentTab(NewNotePath());
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
    if (GetOpenFileNameW(&ofn)) OpenDocumentTab(path);
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
        const std::wstring destination = NormalizeDocumentPath(path);
        if (destination.empty()) return;
        const int other = FindOpenDocument(destination);
        if (other >= 0 && other != g_activeDoc) {
            MessageBoxW(g_hwndMain, L"这个文件已经在左侧打开，不能让两个文档同时指向同一个 TXT。",
                        L"LightNote", MB_OK | MB_ICONINFORMATION);
            SelectDocument(other);
            return;
        }
        const std::wstring previous = g_currentPath;
        const std::wstring oldRecovery = RecoveryPath();
        g_currentPath = destination;
        if (SaveDocument(true)) {
            if (g_activeDoc >= 0) g_notes[g_activeDoc].path = destination;
            if (_wcsicmp(oldRecovery.c_str(), RecoveryPath().c_str()) != 0)
                DeleteFileW(oldRecovery.c_str());
            UpdateTitle(false);
        } else {
            g_currentPath = previous;
            UpdateTitle(Sci(SCI_GETMODIFY) != 0);
        }
    }
}

// Rename changes the file itself, not just the label shown in the sidebar.
static bool ValidDocumentFileName(const std::wstring& name) {
    if (name.empty() || name.size() > 255 || name == L"." || name == L"..") return false;
    if (name.back() == L' ' || name.back() == L'.') return false;
    for (wchar_t ch : name) {
        if (ch < 32 || wcschr(L"<>:\"/\\|?*", ch)) return false;
    }
    std::wstring stem = name.substr(0, name.find(L'.'));
    static const wchar_t* reserved[] = {
        L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4",
        L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1", L"LPT2",
        L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"
    };
    for (const wchar_t* device : reserved)
        if (_wcsicmp(stem.c_str(), device) == 0) return false;
    return true;
}

static bool RenameDocument(int index, const std::wstring& newName) {
    if (index < 0 || index >= static_cast<int>(g_notes.size())) return false;
    if (!ValidDocumentFileName(newName)) {
        MessageBoxW(g_hwndMain,
            L"文件名无效。不能使用 \\ / : * ? \" < > |、末尾空格或句点以及系统保留名称。",
            L"LightNote 重命名", MB_OK | MB_ICONWARNING);
        return false;
    }
    SelectDocument(index);
    const std::wstring original = g_notes[index].path;
    const std::wstring parent = original.substr(0, original.find_last_of(L"\\"));
    const std::wstring target = NormalizeDocumentPath(JoinPath(parent, newName));
    if (target.empty()) return false;
    if (original == target) return true;
    const int alreadyOpen = FindOpenDocument(target);
    if (alreadyOpen >= 0 && alreadyOpen != index) {
        MessageBoxW(g_hwndMain, L"左侧已经打开同一个目标文件，不能重复重命名。",
                    L"LightNote 重命名", MB_OK | MB_ICONWARNING);
        return false;
    }
    const DWORD targetAttributes = GetFileAttributesW(target.c_str());
    if (_wcsicmp(original.c_str(), target.c_str()) != 0 &&
        targetAttributes != INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(g_hwndMain, L"目标文件名已存在，未覆盖原有文件。",
                    L"LightNote 重命名", MB_OK | MB_ICONWARNING);
        return false;
    }

    const std::wstring oldMeta = MetaPath();
    const std::wstring oldRecovery = RecoveryPath();
    const std::wstring oldBackupDir = BackupDir();
    const DWORD originalAttributes = GetFileAttributesW(original.c_str());
    const bool originalExists = originalAttributes != INVALID_FILE_ATTRIBUTES;
    if (!originalExists &&
        GetLastError() != ERROR_FILE_NOT_FOUND &&
        GetLastError() != ERROR_PATH_NOT_FOUND) {
        MessageBoxW(g_hwndMain, L"无法确认原文件状态，重命名已取消。",
                    L"LightNote 重命名", MB_OK | MB_ICONWARNING);
        return false;
    }
    if (originalExists && !MoveFileExW(original.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD error = GetLastError();
        std::wstring msg = L"磁盘上的文件重命名失败（Windows 错误代码 ";
        msg += std::to_wstring(error) + L"）。原文件未改动。";
        MessageBoxW(g_hwndMain, msg.c_str(), L"LightNote 重命名",
                    MB_OK | MB_ICONERROR);
        return false;
    }

    // Keep Scintilla's existing buffer, selection and undo stack in place.
    g_notes[index].path = target;
    g_currentPath = target;

    // If the startup QuickNote was renamed, reopen its new name next time.
    wchar_t preferred[MAX_PATH * 4] = {};
    GetPrivateProfileStringW(L"Session", L"QuickNoteDocument", L"",
                             preferred, static_cast<DWORD>(std::size(preferred)),
                             SettingsPath().c_str());
    const std::wstring originalQuickNote =
        JoinPath(JoinPath(g_appDir, L"data"), L"QuickNote.txt");
    if (_wcsicmp(original.c_str(), originalQuickNote.c_str()) == 0 ||
        (preferred[0] && _wcsicmp(original.c_str(), preferred) == 0)) {
        WritePrivateProfileStringW(L"Session", L"QuickNoteDocument",
                                   target.c_str(), SettingsPath().c_str());
    }

    const std::wstring newMeta = MetaPath();
    bool metadataOK = true;
    if (_wcsicmp(oldMeta.c_str(), newMeta.c_str()) != 0 &&
        GetFileAttributesW(oldMeta.c_str()) != INVALID_FILE_ATTRIBUTES) {
        metadataOK = MoveFileExW(oldMeta.c_str(), newMeta.c_str(),
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!metadataOK) SaveHighlights();
    }

    const std::wstring newRecovery = RecoveryPath();
    bool recoveryOK = true;
    if (Sci(SCI_GETMODIFY)) {
        recoveryOK = WriteAllBytesAtomic(newRecovery, GetDocumentText());
        if (recoveryOK && _wcsicmp(oldRecovery.c_str(), newRecovery.c_str()) != 0)
            DeleteFileW(oldRecovery.c_str());
    } else if (_wcsicmp(oldRecovery.c_str(), newRecovery.c_str()) != 0) {
        DeleteFileW(oldRecovery.c_str());
    }

    wchar_t backupHash[32] = {};
    swprintf(backupHash, std::size(backupHash), L"%016llx",
             static_cast<unsigned long long>(HashPath(target)));
    const std::wstring newBackupDir = JoinPath(JoinPath(g_appDir, L"backups"), backupHash);
    bool backupsOK = true;
    if (_wcsicmp(oldBackupDir.c_str(), newBackupDir.c_str()) != 0 &&
        GetFileAttributesW(oldBackupDir.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (GetFileAttributesW(newBackupDir.c_str()) == INVALID_FILE_ATTRIBUTES) {
            backupsOK = MoveFileExW(oldBackupDir.c_str(), newBackupDir.c_str(),
                                    MOVEFILE_WRITE_THROUGH) != FALSE;
        } else {
            backupsOK = false; // Keep both history directories; never overwrite a backup.
        }
    }

    UpdateTitle(Sci(SCI_GETMODIFY) != 0);
    if (!metadataOK || !recoveryOK || !backupsOK) {
        MessageBoxW(g_hwndMain,
            L"文件名已修改，但部分旧的高亮、恢复或历史备份文件未能迁移。原关联文件已保留，请检查程序目录。",
            L"LightNote 重命名", MB_OK | MB_ICONWARNING);
    }
    return true;
}

static void EndRenameEdit(bool accept) {
    if (!g_hwndRenameEdit) return;
    HWND editor = g_hwndRenameEdit;
    const int index = g_renameIndex;
    std::wstring name;
    if (accept) {
        const int length = GetWindowTextLengthW(editor);
        name.resize(static_cast<size_t>(length) + 1);
        GetWindowTextW(editor, &name[0], length + 1);
        name.resize(static_cast<size_t>(length));
    }
    g_hwndRenameEdit = nullptr;
    g_renameIndex = -1;
    DestroyWindow(editor);
    if (accept) RenameDocument(index, name);
    if (g_hwndEdit) SetFocus(g_hwndEdit);
}

static LRESULT CALLBACK RenameEditWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_KEYDOWN) {
        if (wParam == VK_RETURN) { EndRenameEdit(true); return 0; }
        if (wParam == VK_ESCAPE) { EndRenameEdit(false); return 0; }
    } else if (msg == WM_KILLFOCUS && g_hwndRenameEdit == hwnd) {
        EndRenameEdit(false);
        return 0;
    }
    return CallWindowProcW(g_originalRenameWndProc, hwnd, msg, wParam, lParam);
}

static void BeginRenameDocument(int index) {
    if (index < 0 || index >= static_cast<int>(g_notes.size())) return;
    if (g_hwndRenameEdit) EndRenameEdit(false);
    SelectDocument(index);
    RECT row = {};
    if (SendMessageW(g_hwndFiles, LB_GETITEMRECT, index,
                     reinterpret_cast<LPARAM>(&row)) == LB_ERR) return;
    const wchar_t* file = FileNamePtr(g_notes[index].path);
    POINT location = {row.left + 2, row.top};
    MapWindowPoints(g_hwndFiles, g_hwndMain, &location, 1);
    g_renameIndex = index;
    g_hwndRenameEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", file,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        location.x, location.y,
        std::max(70, static_cast<int>(row.right - row.left - 6)),
        std::max(22, static_cast<int>(row.bottom - row.top + 3)),
        g_hwndMain, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDM_RENAME_EDIT)),
        g_hInst, nullptr);
    if (!g_hwndRenameEdit) { g_renameIndex = -1; return; }
    SendMessageW(g_hwndRenameEdit, EM_LIMITTEXT, 255, 0);
    SendMessageW(g_hwndRenameEdit, WM_SETFONT,
                 SendMessageW(g_hwndFiles, WM_GETFONT, 0, 0), TRUE);
    g_originalRenameWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
        g_hwndRenameEdit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(RenameEditWndProc)));
    SetFocus(g_hwndRenameEdit);
    const wchar_t* dot = wcsrchr(file, L'.');
    const int stemLength = (dot && dot != file) ? static_cast<int>(dot - file) : -1;
    SendMessageW(g_hwndRenameEdit, EM_SETSEL, 0, stemLength);
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

static bool IsAsciiCopyTokenChar(int ch) {
    return (ch >= 'a' && ch <= 'z') ||
           (ch >= 'A' && ch <= 'Z') ||
           (ch >= '0' && ch <= '9') ||
           ch == '_' || ch == '@' || ch == '.' || ch == '-' || ch == '+';
}

static void NormalizeDoubleClickSelection(LRESULT clicked) {
    const LRESULT length = Sci(SCI_GETLENGTH);
    const LRESULT start = Sci(SCI_GETSELECTIONSTART);
    const LRESULT end = Sci(SCI_GETSELECTIONEND);
    if (start < end) {
        LRESULT trimmedEnd = end;
        while (trimmedEnd > start) {
            const int ch = static_cast<int>(Sci(SCI_GETCHARAT, static_cast<WPARAM>(trimmedEnd - 1)));
            if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') break;
            --trimmedEnd;
        }
        if (trimmedEnd > start && trimmedEnd != end) {
            Sci(SCI_SETSEL, static_cast<WPARAM>(start), static_cast<LPARAM>(trimmedEnd));
        }
    }

    // Scintilla splits an ASCII code or email at '@', '.' and other symbols.
    // Select the entire token on a double-click, but never its adjacent spaces.
    if (clicked < 0 || clicked >= length ||
        !IsAsciiCopyTokenChar(static_cast<int>(Sci(SCI_GETCHARAT, static_cast<WPARAM>(clicked))))) {
        return;
    }
    LRESULT first = clicked;
    LRESULT last = clicked + 1;
    while (first > 0 &&
           IsAsciiCopyTokenChar(static_cast<int>(Sci(SCI_GETCHARAT, static_cast<WPARAM>(first - 1))))) {
        --first;
    }
    while (last < length &&
           IsAsciiCopyTokenChar(static_cast<int>(Sci(SCI_GETCHARAT, static_cast<WPARAM>(last))))) {
        ++last;
    }
    bool hasWordChar = false;
    bool hasJoiner = false;
    for (LRESULT pos = first; pos < last; ++pos) {
        const int ch = static_cast<int>(Sci(SCI_GETCHARAT, static_cast<WPARAM>(pos)));
        hasWordChar |= (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9');
        hasJoiner |= ch == '@' || ch == '.' || ch == '_' || ch == '-' || ch == '+';
    }
    if (hasWordChar && hasJoiner) {
        Sci(SCI_SETSEL, static_cast<WPARAM>(first), static_cast<LPARAM>(last));
    }
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
    if (msg == WM_DROPFILES) {
        SendMessageW(g_hwndMain, WM_DROPFILES, wParam, lParam);
        return 0;
    }
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
    AppendMenuW(file, MF_STRING, IDM_CLOSE_DOC, L"关闭当前文档\tCtrl+W");
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
        g_hwndMain = hwnd;
        DragAcceptFiles(hwnd, TRUE);
        g_hwndFiles = CreateWindowExW(WS_EX_CLIENTEDGE | WS_EX_ACCEPTFILES,
            L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL |
            LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, 0, 0, g_sidebarWidth, 0,
            hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDM_FILELIST)), g_hInst, nullptr);
        if (!g_hwndFiles) return -1;
        g_originalListWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            g_hwndFiles, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ListWndProc)));
        DragAcceptFiles(g_hwndFiles, TRUE);
        g_hwndSplitter = CreateWindowExW(WS_EX_ACCEPTFILES, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_NOTIFY, 0, 0, SPLITTER_WIDTH, 0,
            hwnd, nullptr, g_hInst, nullptr);
        if (!g_hwndSplitter) return -1;
        g_originalSplitterWndProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
            g_hwndSplitter, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(SplitterWndProc)));
        DragAcceptFiles(g_hwndSplitter, TRUE);
        g_hwndEdit = nullptr;
        g_hwndUndoButton = CreateWindowW(L"BUTTON", L"撤销", WS_CHILD | BS_OWNERDRAW,
            6, 4, 82, 27, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDM_UNDO)), g_hInst, nullptr);
        g_hwndRedoButton = CreateWindowW(L"BUTTON", L"重做", WS_CHILD | BS_OWNERDRAW,
            94, 4, 82, 27, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDM_REDO)), g_hInst, nullptr);
        g_hwndSaveButton = CreateWindowW(L"BUTTON", L"保存", WS_CHILD | BS_OWNERDRAW,
            182, 4, 72, 27, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDM_SAVE)), g_hInst, nullptr);

        if (!g_startPath.empty()) {
            OpenDocumentTab(g_startPath);
        } else {
            std::wstring dataDir = JoinPath(g_appDir, L"data");
            EnsureDir(dataDir);
            wchar_t preferred[MAX_PATH * 4] = {};
            GetPrivateProfileStringW(L"Session", L"QuickNoteDocument", L"",
                preferred, static_cast<DWORD>(std::size(preferred)),
                SettingsPath().c_str());
            const std::wstring saved = NormalizeDocumentPath(preferred);
            const DWORD attributes = GetFileAttributesW(saved.c_str());
            if (!saved.empty() && attributes != INVALID_FILE_ATTRIBUTES &&
                !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
                OpenDocumentTab(saved);
            } else {
                std::wstring quick = JoinPath(dataDir, L"QuickNote.txt");
                if (GetFileAttributesW(quick.c_str()) == INVALID_FILE_ATTRIBUTES) {
                    WriteAllBytesAtomic(quick, std::string());
                }
                OpenDocumentTab(quick);
            }
        }
        InstallSidebarDropTarget(g_hwndFiles);
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
        LayoutEditors(hwnd);
        return 0;

    case WM_SETFOCUS:
        if (g_hwndEdit) SetFocus(g_hwndEdit);
        return 0;

    case WM_NOTIFY: {
        SCNotification* scn = reinterpret_cast<SCNotification*>(lParam);
        if (scn && scn->nmhdr.hwndFrom == g_hwndEdit) {
            if (scn->nmhdr.code == SCN_DOUBLECLICK) {
                NormalizeDoubleClickSelection(scn->position);
            }
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
        if(LOWORD(wParam)==IDM_FILELIST && HIWORD(wParam)==LBN_SELCHANGE) {
            int selected=static_cast<int>(SendMessageW(g_hwndFiles,LB_GETCURSEL,0,0));
            SelectDocument(selected);return 0;
        }
        switch (LOWORD(wParam)) {
        case IDM_UNDO: Sci(SCI_UNDO); return 0;
        case IDM_REDO: Sci(SCI_REDO); return 0;
        case IDM_NEW: NewNote(); return 0;
        case IDM_OPEN: OpenDocumentDialog(); return 0;
        case IDM_SAVE: SaveDocument(true); return 0;
        case IDM_SAVEAS: SaveAsDialog(); return 0;
        case IDM_CLOSE_DOC: CloseDocumentAt(g_activeDoc); return 0;
        case IDM_RENAME_DOC: BeginRenameDocument(g_activeDoc); return 0;
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

    case WM_DROPFILES:
        HandleDroppedFiles(reinterpret_cast<HDROP>(wParam));
        return 0;

    case WM_COPYDATA: {
        auto* cds=reinterpret_cast<COPYDATASTRUCT*>(lParam);
        if (cds && cds->lpData && cds->cbData >= sizeof(wchar_t) &&
            cds->cbData <= 32768 * sizeof(wchar_t) &&
            cds->cbData % sizeof(wchar_t) == 0) {
            const size_t chars = cds->cbData / sizeof(wchar_t);
            const wchar_t* begin = static_cast<const wchar_t*>(cds->lpData);
            const wchar_t* end = std::find(begin, begin + chars, L'\0');
            if (end == begin + chars || end == begin) return FALSE;
            if (!OpenDocumentTab(std::wstring(begin, end))) return FALSE;
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            return TRUE;
        }
        return FALSE;
    }
    case WM_CLOSE:
        if (g_hwndRenameEdit) EndRenameEdit(false);
        for(int i=0;i<static_cast<int>(g_notes.size());++i) {
            SelectDocument(i);
            if(!ConfirmPendingChanges())return 0;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (g_hwndFiles) RevokeDragDrop(g_hwndFiles);
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
        { FCONTROL | FVIRTKEY, 'W', IDM_CLOSE_DOC },
        { FVIRTKEY, VK_F2, IDM_RENAME_DOC },
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
    g_sidebarWidth = std::max(SIDEBAR_MIN, std::min(SIDEBAR_MAX,
        static_cast<int>(GetPrivateProfileIntW(
            L"Layout", L"SidebarWidth", SIDEBAR_DEFAULT, SettingsPath().c_str()))));
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

    HANDLE instanceMutex = CreateMutexW(nullptr, FALSE,
                                        L"Local\\LightNoteV083SingleInstance");
    if (!instanceMutex) return 5;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND running = nullptr;
        for (int attempt = 0; attempt < 40 && !running; ++attempt) {
            running = FindWindowW(L"LightNoteV083MainWindow", nullptr);
            if (!running) Sleep(100);
        }
        bool delivered = running != nullptr;
        if (running && !g_startPath.empty()) {
            COPYDATASTRUCT cds = {};
            cds.cbData = static_cast<DWORD>((g_startPath.size() + 1) * sizeof(wchar_t));
            cds.lpData = const_cast<wchar_t*>(g_startPath.c_str());
            DWORD_PTR acknowledged = 0;
            delivered = SendMessageTimeoutW(running, WM_COPYDATA, 0,
                        reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG,
                        3000, &acknowledged) != 0 && acknowledged != 0;
        }
        if (delivered) {
            ShowWindow(running, SW_RESTORE);
            SetForegroundWindow(running);
            CloseHandle(instanceMutex);
            return 0;
        }
        MessageBoxW(nullptr, L"LightNote 已在运行，但现有窗口没有响应文件打开请求。",
                    L"LightNote", MB_OK | MB_ICONWARNING);
        CloseHandle(instanceMutex);
        return 5;
    }

    if (!Scintilla_RegisterClasses(hInstance)) {
        MessageBoxW(nullptr, L"Scintilla 初始化失败。", L"LightNote", MB_OK | MB_ICONERROR);
        CloseHandle(instanceMutex);
        return 1;
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.hInstance = hInstance;
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = L"LightNoteV083MainWindow";
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCEW(1));
    wc.hIconSm = static_cast<HICON>(LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0));
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&wc)) { CloseHandle(instanceMutex); return 2; }

    g_hwndMain = CreateWindowExW(
        0, wc.lpszClassName, L"LightNote",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 900, 700,
        nullptr, CreateMainMenu(), hInstance, nullptr);

    if (!g_hwndMain) { CloseHandle(instanceMutex); return 3; }

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
    CloseHandle(instanceMutex);

    return static_cast<int>(msg.wParam);
}
