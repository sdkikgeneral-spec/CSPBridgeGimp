/**
 * @file   process.cpp
 * @brief  GIMP プラグイン EXE の子プロセス起動とパイプ管理
 * @author CSPBridgeGimp
 * @date   2026-04-27
 *
 * tools/scanner/scan_and_select.py の _spawn_plugin_windows() /
 * _spawn_plugin_posix() / _build_msvcrt_inherit_block() を C++ に移植。
 *
 * Windows 実装の要点:
 *   - _pipe() で匿名パイプを作成し _O_BINARY で binary mode に固定
 *   - 親側 fd の HANDLE を SetHandleInformation で継承不可にする
 *   - lpReserved2 に MSVCRT fd-inherit ブロックを埋め込んで CreateProcessW を呼ぶ
 *   - 子側 fd は CreateProcessW 成功後に _close() する
 *   - 子の PATH 先頭に gimpLibDir を追加（STATUS_DLL_NOT_FOUND 防止）
 *
 * Mac 実装の要点:
 *   - pipe() で匿名パイプを作成し fcntl(FD_CLOEXEC) で親側を保護
 *   - fork 後の子で dup2() によって fd を 3/4 に移動して execv する
 *   - 子の DYLD_LIBRARY_PATH 先頭に gimpLibDir を追加
 */

#include "process.h"

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32

// ---------------------------------------------------------------------------
// Windows ヘッダー
// ---------------------------------------------------------------------------
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <fcntl.h>  // _O_BINARY
#include <io.h>     // _pipe, _close, _get_osfhandle

// ---------------------------------------------------------------------------
// 定数 — MSVCRT ioinfo フラグ（公式非公開、GIMP 本体互換）
// ---------------------------------------------------------------------------
static constexpr BYTE MSVCRT_FOPEN      = 0x01;
static constexpr BYTE MSVCRT_FPIPE      = 0x08;
// FTEXT (0x80) は立てない。立てると子側で CRLF 変換が入り Wire Protocol が壊れる。
static constexpr BYTE PIPE_FLAGS        = MSVCRT_FOPEN | MSVCRT_FPIPE; // = 0x09

// ---------------------------------------------------------------------------
// ヘルパー: Windows エラーコードを std::runtime_error に変換
// ---------------------------------------------------------------------------
static std::runtime_error MakeWin32Error(const char* context, DWORD errCode)
{
    char buf[256];
    FormatMessageA(
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, errCode, 0, buf, sizeof(buf), nullptr);
    return std::runtime_error(std::string(context) + ": " + buf);
}

// ---------------------------------------------------------------------------
// _build_msvcrt_inherit_block
//
// MSVCRT が子プロセス起動時に STARTUPINFO.lpReserved2 から読む
// fd-inherit ブロックを構築する。
//
// レイアウト:
//   uint32   count              登録する fd の総数
//   uint8    flags[count]       各 fd の MSVCRT ioinfo フラグ
//   HANDLE   handles[count]     各 fd の Win32 HANDLE（64-bit = 8 bytes/個）
//
// entries[i] = (handle, flags)。index が子側の fd 番号。
// handle = INVALID_HANDLE_VALUE は「未割当」を意味する。
// ---------------------------------------------------------------------------
struct InheritBlockEntry
{
    HANDLE handle;
    BYTE   flags;
};

static std::vector<BYTE> BuildMsvcrtInheritBlock(
    const std::vector<InheritBlockEntry>& entries)
{
    const DWORD count       = static_cast<DWORD>(entries.size());
    const SIZE_T handleSize = sizeof(HANDLE); // 64-bit = 8
    const SIZE_T totalSize  = 4 + count + count * handleSize;

    std::vector<BYTE> buf(totalSize, 0);

    // count フィールド（リトルエンディアン — MSVCRT は自身のバイト順で読む）
    std::memcpy(buf.data(), &count, 4);

    // flags 配列
    for (DWORD i = 0; i < count; ++i)
    {
        buf[4 + i] = entries[i].flags;
    }

    // handles 配列
    const SIZE_T handlesOffset = 4 + count;
    for (DWORD i = 0; i < count; ++i)
    {
        HANDLE h = entries[i].handle;
        std::memcpy(buf.data() + handlesOffset + i * handleSize, &h, handleSize);
    }

    return buf;
}

// ---------------------------------------------------------------------------
// ヘルパー: コマンドライン引数を CreateProcessW 用文字列にクォート結合する
// ---------------------------------------------------------------------------
static std::wstring BuildCommandLine(const std::vector<std::wstring>& argv)
{
    std::wstring result;
    for (const auto& arg : argv)
    {
        if (!result.empty())
        {
            result += L' ';
        }

        // スペース・タブ・二重引用符を含む場合のみクォート
        const bool needsQuote =
            arg.find_first_of(L" \t\"") != std::wstring::npos;

        if (needsQuote)
        {
            result += L'"';
            for (wchar_t c : arg)
            {
                if (c == L'"')
                {
                    result += L'\\';
                }
                result += c;
            }
            result += L'"';
        }
        else
        {
            result += arg;
        }
    }
    return result;
}

// ---------------------------------------------------------------------------
// ヘルパー: 現在の環境変数ブロックを取得してダブル NUL 終端文字列に変換
//          PATH の先頭に gimpLibDir を追加する
// ---------------------------------------------------------------------------
static std::vector<wchar_t> BuildEnvBlock(const std::string& gimpLibDir)
{
    // 現在の環境変数ブロック（ダブル NUL 終端、Unicode）を取得
    wchar_t* rawEnv = GetEnvironmentStringsW();
    if (!rawEnv)
    {
        throw MakeWin32Error("GetEnvironmentStringsW", GetLastError());
    }

    // ブロックをエントリごとに分解
    std::vector<std::wstring> entries;
    const wchar_t* cursor = rawEnv;
    while (*cursor != L'\0')
    {
        entries.emplace_back(cursor);
        cursor += entries.back().size() + 1;
    }
    FreeEnvironmentStringsW(rawEnv);

    // PATH エントリを探して gimpLibDir を先頭に追加
    const std::wstring gimpLibDirW(gimpLibDir.begin(), gimpLibDir.end());
    bool pathFound = false;
    for (auto& entry : entries)
    {
        // 大文字・小文字を区別しないで "PATH=" を検索
        if (entry.size() >= 5 &&
            CompareStringOrdinal(
                entry.c_str(), 5, L"PATH=", 5, TRUE) == CSTR_EQUAL)
        {
            const std::wstring currentPath = entry.substr(5);
            entry = L"PATH=" + gimpLibDirW + L";" + currentPath;
            pathFound = true;
            break;
        }
    }
    if (!pathFound)
    {
        entries.emplace_back(L"PATH=" + gimpLibDirW);
    }

    // ダブル NUL 終端のフラットバッファを作成
    std::vector<wchar_t> block;
    for (const auto& entry : entries)
    {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0'); // ブロック終端の NUL

    return block;
}

// ---------------------------------------------------------------------------
// Windows 実装本体
// ---------------------------------------------------------------------------
static PluginProcess SpawnPluginWindows(
    const std::string& exePath,
    const std::string& gimpLibDir,
    PluginMode mode,
    int protocolVersion)
{
    // ------------------------------------------------------------------
    // 1. パイプ作成（binary mode）
    //    pluginReadFd  → 子が読む（親は書く）: parent_write_fd
    //    pluginWriteFd → 子が書く（親は読む）: parent_read_fd
    // ------------------------------------------------------------------
    int parentReadFd  = -1;
    int pluginWriteFd = -1;
    int pluginReadFd  = -1;
    int parentWriteFd = -1;

    {
        int fds[2];
        // plugin → parent
        if (_pipe(fds, 4096, _O_BINARY) != 0)
        {
            throw std::runtime_error("_pipe failed (plugin->parent)");
        }
        parentReadFd  = fds[0];
        pluginWriteFd = fds[1];

        // parent → plugin
        if (_pipe(fds, 4096, _O_BINARY) != 0)
        {
            _close(parentReadFd);
            _close(pluginWriteFd);
            throw std::runtime_error("_pipe failed (parent->plugin)");
        }
        pluginReadFd  = fds[0];
        parentWriteFd = fds[1];
    }

    // ------------------------------------------------------------------
    // 2. 継承フラグ設定
    //    親側 fd → 継承不可（CLOEXEC 相当）
    //    子側 fd → 継承可
    // ------------------------------------------------------------------
    auto setInherit = [](int fd, bool inherit) -> bool
    {
        const HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(fd));
        if (h == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        const DWORD flag = inherit ? HANDLE_FLAG_INHERIT : 0;
        return SetHandleInformation(h, HANDLE_FLAG_INHERIT, flag) != FALSE;
    };

    if (!setInherit(parentReadFd, false) || !setInherit(parentWriteFd, false))
    {
        _close(parentReadFd);
        _close(pluginWriteFd);
        _close(pluginReadFd);
        _close(parentWriteFd);
        throw MakeWin32Error("SetHandleInformation (parent side)", GetLastError());
    }
    if (!setInherit(pluginReadFd, true) || !setInherit(pluginWriteFd, true))
    {
        _close(parentReadFd);
        _close(pluginWriteFd);
        _close(pluginReadFd);
        _close(parentWriteFd);
        throw MakeWin32Error("SetHandleInformation (plugin side)", GetLastError());
    }

    // ------------------------------------------------------------------
    // 3. lpReserved2 ブロック構築
    //    fd 0/1/2: INVALID_HANDLE_VALUE + flags=0（未割当）
    //    fd 3: 子が read する（pluginReadFd の HANDLE）
    //    fd 4: 子が write する（pluginWriteFd の HANDLE）
    // ------------------------------------------------------------------
    const HANDLE hPluginRead  =
        reinterpret_cast<HANDLE>(_get_osfhandle(pluginReadFd));
    const HANDLE hPluginWrite =
        reinterpret_cast<HANDLE>(_get_osfhandle(pluginWriteFd));

    const std::vector<InheritBlockEntry> entries = {
        { INVALID_HANDLE_VALUE, 0          }, // fd 0
        { INVALID_HANDLE_VALUE, 0          }, // fd 1
        { INVALID_HANDLE_VALUE, 0          }, // fd 2
        { hPluginRead,          PIPE_FLAGS }, // fd 3: plugin reads
        { hPluginWrite,         PIPE_FLAGS }, // fd 4: plugin writes
    };
    std::vector<BYTE> reserved2Buf = BuildMsvcrtInheritBlock(entries);

    // ------------------------------------------------------------------
    // 4. 環境変数ブロック（PATH に gimpLibDir を追加）
    // ------------------------------------------------------------------
    std::vector<wchar_t> envBlock = BuildEnvBlock(gimpLibDir);

    // ------------------------------------------------------------------
    // 5. コマンドライン構築
    //    <progname> -gimp <version> 3 4 <-query|-run> 0
    // ------------------------------------------------------------------
    const wchar_t* modeStr = (mode == PluginMode::Query) ? L"-query" : L"-run";
    const std::wstring exePathW(exePath.begin(), exePath.end());
    const std::wstring versionStr = std::to_wstring(protocolVersion);

    const std::vector<std::wstring> argv = {
        exePathW,
        L"-gimp",
        versionStr,
        L"3",
        L"4",
        modeStr,
        L"0",
    };
    std::wstring cmdLine = BuildCommandLine(argv);

    // CreateProcessW は lpCommandLine を mutable バッファとして要求する
    std::vector<wchar_t> cmdLineBuf(cmdLine.begin(), cmdLine.end());
    cmdLineBuf.push_back(L'\0');

    // ------------------------------------------------------------------
    // 6. STARTUPINFOW 設定
    // ------------------------------------------------------------------
    STARTUPINFOW si{};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.cbReserved2 = static_cast<WORD>(reserved2Buf.size());
    si.lpReserved2 = reserved2Buf.data();
    si.hStdInput   = INVALID_HANDLE_VALUE;
    si.hStdOutput  = INVALID_HANDLE_VALUE;

    // 親の stderr を継承させて GLib 警告を親コンソールに流す
    const HANDLE hStdErr = GetStdHandle(STD_ERROR_HANDLE);
    if (hStdErr != INVALID_HANDLE_VALUE && hStdErr != nullptr)
    {
        SetHandleInformation(hStdErr, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
        si.hStdError = hStdErr;
    }
    else
    {
        si.hStdError = INVALID_HANDLE_VALUE;
    }

    // ------------------------------------------------------------------
    // 7. CreateProcessW 呼び出し
    // ------------------------------------------------------------------
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(
        nullptr,              // lpApplicationName
        cmdLineBuf.data(),   // lpCommandLine
        nullptr,              // lpProcessAttributes
        nullptr,              // lpThreadAttributes
        TRUE,                 // bInheritHandles
        CREATE_UNICODE_ENVIRONMENT,
        envBlock.data(),     // lpEnvironment
        nullptr,              // lpCurrentDirectory
        &si,
        &pi);

    if (!ok)
    {
        const DWORD err = GetLastError();
        _close(parentReadFd);
        _close(pluginWriteFd);
        _close(pluginReadFd);
        _close(parentWriteFd);
        throw MakeWin32Error("CreateProcessW", err);
    }

    // ------------------------------------------------------------------
    // 8. 子側 fd を親プロセスで閉じる
    //    これにより親が持つのは parentReadFd / parentWriteFd のみになる
    // ------------------------------------------------------------------
    _close(pluginReadFd);
    _close(pluginWriteFd);

    PluginProcess proc{};
    proc.readFd    = parentReadFd;
    proc.writeFd   = parentWriteFd;
    proc.m_hProcess = pi.hProcess;
    proc.m_hThread  = pi.hThread;
    return proc;
}

// ---------------------------------------------------------------------------
// WaitPlugin — Windows 実装
// ---------------------------------------------------------------------------
int WaitPlugin(PluginProcess& proc, int timeoutMs)
{
    const DWORD ms =
        (timeoutMs < 0) ? INFINITE : static_cast<DWORD>(timeoutMs);

    const DWORD waitResult = WaitForSingleObject(proc.m_hProcess, ms);
    if (waitResult == WAIT_TIMEOUT)
    {
        throw std::runtime_error("WaitPlugin: timed out");
    }
    if (waitResult != WAIT_OBJECT_0)
    {
        throw MakeWin32Error("WaitForSingleObject", GetLastError());
    }

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(proc.m_hProcess, &exitCode))
    {
        throw MakeWin32Error("GetExitCodeProcess", GetLastError());
    }
    return static_cast<int>(exitCode);
}

// ---------------------------------------------------------------------------
// TerminatePlugin — Windows 実装
// ---------------------------------------------------------------------------
void TerminatePlugin(PluginProcess& proc)
{
    if (proc.m_hProcess != nullptr && proc.m_hProcess != INVALID_HANDLE_VALUE)
    {
        TerminateProcess(proc.m_hProcess, 1);
    }
}

// ---------------------------------------------------------------------------
// ClosePlugin — Windows 実装
// ---------------------------------------------------------------------------
void ClosePlugin(PluginProcess& proc)
{
    if (proc.m_hThread != nullptr && proc.m_hThread != INVALID_HANDLE_VALUE)
    {
        CloseHandle(proc.m_hThread);
        proc.m_hThread = INVALID_HANDLE_VALUE;
    }
    if (proc.m_hProcess != nullptr && proc.m_hProcess != INVALID_HANDLE_VALUE)
    {
        CloseHandle(proc.m_hProcess);
        proc.m_hProcess = INVALID_HANDLE_VALUE;
    }
}

#else // !_WIN32

// ---------------------------------------------------------------------------
// Mac / POSIX ヘッダー
// ---------------------------------------------------------------------------
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

// ---------------------------------------------------------------------------
// ヘルパー: errno を std::runtime_error に変換
// ---------------------------------------------------------------------------
static std::runtime_error MakePosixError(const char* context, int errNo)
{
    return std::runtime_error(
        std::string(context) + ": " + std::strerror(errNo));
}

// ---------------------------------------------------------------------------
// ヘルパー: DYLD_LIBRARY_PATH に gimpLibDir を追加してから fork する
// ---------------------------------------------------------------------------
static void PrependDyldLibraryPath(const std::string& gimpLibDir)
{
    const char* varName = "DYLD_LIBRARY_PATH";
    const char* current = ::getenv(varName);
    std::string newVal  = gimpLibDir;
    if (current && *current != '\0')
    {
        newVal += ':';
        newVal += current;
    }
    // setenv は fork 後の子でのみ呼ぶ（親への副作用なし）
    ::setenv(varName, newVal.c_str(), 1);
}

// ---------------------------------------------------------------------------
// Mac 実装本体
// ---------------------------------------------------------------------------
static PluginProcess SpawnPluginPosix(
    const std::string& exePath,
    const std::string& gimpLibDir,
    PluginMode mode,
    int protocolVersion)
{
    // ------------------------------------------------------------------
    // 1. パイプ作成（2 組）
    //    fdsPluginToParent[0] = 親が読む  (parentReadFd)
    //    fdsPluginToParent[1] = 子が書く  (pluginWriteFd)
    //    fdsParentToPlugin[0] = 子が読む  (pluginReadFd)
    //    fdsParentToPlugin[1] = 親が書く  (parentWriteFd)
    // ------------------------------------------------------------------
    int fdsPluginToParent[2] = { -1, -1 };
    int fdsParentToPlugin[2] = { -1, -1 };

    if (::pipe(fdsPluginToParent) != 0)
    {
        throw MakePosixError("pipe (plugin->parent)", errno);
    }
    if (::pipe(fdsParentToPlugin) != 0)
    {
        ::close(fdsPluginToParent[0]);
        ::close(fdsPluginToParent[1]);
        throw MakePosixError("pipe (parent->plugin)", errno);
    }

    const int parentReadFd  = fdsPluginToParent[0];
    const int pluginWriteFd = fdsPluginToParent[1];
    const int pluginReadFd  = fdsParentToPlugin[0];
    const int parentWriteFd = fdsParentToPlugin[1];

    // ------------------------------------------------------------------
    // 2. 親側 fd に FD_CLOEXEC を設定（exec 後に自動クローズ）
    // ------------------------------------------------------------------
    auto setCloexec = [](int fd) -> bool
    {
        return ::fcntl(fd, F_SETFD, FD_CLOEXEC) == 0;
    };

    if (!setCloexec(parentReadFd) || !setCloexec(parentWriteFd))
    {
        const int savedErrno = errno;
        ::close(parentReadFd);
        ::close(pluginWriteFd);
        ::close(pluginReadFd);
        ::close(parentWriteFd);
        throw MakePosixError("fcntl FD_CLOEXEC", savedErrno);
    }

    // ------------------------------------------------------------------
    // 3. fork
    // ------------------------------------------------------------------
    const ::pid_t pid = ::fork();
    if (pid < 0)
    {
        const int savedErrno = errno;
        ::close(parentReadFd);
        ::close(pluginWriteFd);
        ::close(pluginReadFd);
        ::close(parentWriteFd);
        throw MakePosixError("fork", savedErrno);
    }

    if (pid == 0)
    {
        // ------------------------------------------------------------------
        // 子プロセス
        // ------------------------------------------------------------------

        // gimpLibDir を DYLD_LIBRARY_PATH に追加
        if (!gimpLibDir.empty())
        {
            PrependDyldLibraryPath(gimpLibDir);
        }

        // 子側 fd を 3/4 に移動
        // fd 3: 子が読む
        if (::dup2(pluginReadFd, 3) < 0)
        {
            ::_exit(1);
        }
        // fd 4: 子が書く
        if (::dup2(pluginWriteFd, 4) < 0)
        {
            ::_exit(1);
        }

        // 元の fd を閉じる（dup2 先が 3/4 でない場合のみ）
        if (pluginReadFd != 3)
        {
            ::close(pluginReadFd);
        }
        if (pluginWriteFd != 4)
        {
            ::close(pluginWriteFd);
        }

        // 親側 fd は FD_CLOEXEC で exec 後に自動クローズされる
        // （念のため明示的にも閉じる）
        ::close(parentReadFd);
        ::close(parentWriteFd);

        // argv 構築
        const char* modeStr = (mode == PluginMode::Query) ? "-query" : "-run";
        char versionBuf[16];
        std::snprintf(versionBuf, sizeof(versionBuf), "%d", protocolVersion);

        // execv に渡す argv は非 const char* を要求する
        std::vector<std::string> argvStr = {
            exePath, "-gimp", versionBuf, "3", "4", modeStr, "0"
        };
        std::vector<char*> argvPtr;
        argvPtr.reserve(argvStr.size() + 1);
        for (auto& s : argvStr)
        {
            argvPtr.push_back(s.data());
        }
        argvPtr.push_back(nullptr);

        ::execv(exePath.c_str(), argvPtr.data());

        // execv が返ってきた場合は失敗
        ::_exit(1);
    }

    // ------------------------------------------------------------------
    // 親プロセス: 子側 fd を閉じる
    // ------------------------------------------------------------------
    ::close(pluginReadFd);
    ::close(pluginWriteFd);

    PluginProcess proc{};
    proc.readFd = parentReadFd;
    proc.writeFd = parentWriteFd;
    proc.m_pid   = pid;
    return proc;
}

// ---------------------------------------------------------------------------
// WaitPlugin — POSIX 実装
// ---------------------------------------------------------------------------
int WaitPlugin(PluginProcess& proc, int timeoutMs)
{
    if (timeoutMs < 0)
    {
        // 無制限待機
        int status = 0;
        if (::waitpid(proc.m_pid, &status, 0) < 0)
        {
            throw MakePosixError("waitpid", errno);
        }
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    // タイムアウト付き: 10ms 間隔でポーリング
    // PoC 用途のためシンプルなビジーウェイト方式
    const int intervalUs  = 10000; // 10ms
    int elapsedMs         = 0;

    while (elapsedMs < timeoutMs)
    {
        int status = 0;
        const ::pid_t result = ::waitpid(proc.m_pid, &status, WNOHANG);
        if (result < 0)
        {
            throw MakePosixError("waitpid", errno);
        }
        if (result > 0)
        {
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }
        ::usleep(static_cast<useconds_t>(intervalUs));
        elapsedMs += intervalUs / 1000;
    }

    throw std::runtime_error("WaitPlugin: timed out");
}

// ---------------------------------------------------------------------------
// TerminatePlugin — POSIX 実装
// ---------------------------------------------------------------------------
void TerminatePlugin(PluginProcess& proc)
{
    if (proc.m_pid > 0)
    {
        ::kill(proc.m_pid, SIGTERM);
    }
}

// ---------------------------------------------------------------------------
// ClosePlugin — POSIX 実装
// ---------------------------------------------------------------------------
void ClosePlugin(PluginProcess& proc)
{
    // waitpid は WaitPlugin 内で完了済みのため、ここでは何もしない。
    // TerminatePlugin 後に ClosePlugin を呼ぶ場合は waitpid でゾンビ回収する。
    if (proc.m_pid > 0)
    {
        int status = 0;
        ::waitpid(proc.m_pid, &status, WNOHANG);
        proc.m_pid = 0;
    }
}

#endif // _WIN32

// ---------------------------------------------------------------------------
// SpawnPlugin — プラットフォーム共通エントリポイント
// ---------------------------------------------------------------------------

PluginProcess SpawnPlugin(
    const std::string& exePath,
    const std::string& gimpLibDir,
    PluginMode mode,
    int protocolVersion)
{
#ifdef _WIN32
    return SpawnPluginWindows(exePath, gimpLibDir, mode, protocolVersion);
#else
    return SpawnPluginPosix(exePath, gimpLibDir, mode, protocolVersion);
#endif
}
