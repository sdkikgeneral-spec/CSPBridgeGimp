/**
 * @file   process.h
 * @brief  GIMP プラグイン EXE の子プロセス起動とパイプ管理
 * @author CSPBridgeGimp
 * @date   2026-04-27
 *
 * Windows: CreateProcessW + STARTUPINFO.lpReserved2 による MSVCRT fd 継承方式。
 *          tools/scanner/scan_and_select.py の _spawn_plugin_windows() / _build_msvcrt_inherit_block()
 *          を C++ に移植したもの。Boost.Process では lpReserved2 を扱えないため不使用。
 * Mac:     fork + execv + POSIX pipe 方式。FD_CLOEXEC で親側 fd を保護する。
 *
 * spec.md §8 参照。
 */

#pragma once

#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/types.h>
#endif

// ---------------------------------------------------------------------------
// 公開型
// ---------------------------------------------------------------------------

/**
 * @brief GIMP プラグイン起動モード
 */
enum class PluginMode
{
    Query, ///< -query モード。プラグインがプロシージャ定義を送信して即終了する
    Run,   ///< -run モード。タイル転送フェーズまで実行する
};

/**
 * @brief 起動中の GIMP プラグインプロセスを保持する構造体
 *
 * SpawnPlugin() が返す。readFd / writeFd は親プロセス側の fd。
 * WaitPlugin() または TerminatePlugin() を呼んだ後に ClosePlugin() でリソースを解放する。
 *
 * @note m_ プレフィックスのメンバーはプロセスハンドル / PID であり、
 *       wire_io 層から直接参照してはならない。ClosePlugin() に委譲すること。
 */
struct PluginProcess
{
    int readFd;  ///< ホストがプラグインから読む fd（親側）
    int writeFd; ///< ホストがプラグインへ書く fd（親側）

#ifdef _WIN32
    HANDLE m_hProcess; ///< プロセスハンドル（Wait / Terminate 用）
    HANDLE m_hThread;  ///< スレッドハンドル（Close 用）
#else
    pid_t m_pid; ///< 子プロセス PID
#endif
};

// ---------------------------------------------------------------------------
// 公開 API
// ---------------------------------------------------------------------------

/**
 * @brief  GIMP プラグイン EXE をパイプ付きで起動する
 *
 * Windows では CreateProcessW + STARTUPINFO.lpReserved2 を使って
 * MSVCRT fd-inherit ブロックを渡す（spec.md §5.6 / §8 参照）。
 * Mac では fork + execv + POSIX pipe で fd を 3/4 に dup2 してから exec する。
 *
 * 子への argv フォーマット:
 *   <progname> -gimp <protocolVersion> 3 4 <-query|-run> 0
 *
 * gimp_lib_dir は子プロセスの環境変数に追加する:
 *   Windows: PATH 先頭に追加（STATUS_DLL_NOT_FOUND 防止）
 *   Mac:     DYLD_LIBRARY_PATH 先頭に追加
 *
 * @param  exePath          プラグイン EXE のフルパス
 * @param  gimpLibDir       libgimp-3.0-0.dll 等のあるディレクトリ
 * @param  mode             起動モード（Query / Run）
 * @param  protocolVersion  Wire Protocol バージョン（GIMP 3.2 = 0x0117 = 279）
 * @return 起動済みの PluginProcess
 * @throws std::runtime_error 起動に失敗した場合
 */
PluginProcess SpawnPlugin(
    const std::string& exePath,
    const std::string& gimpLibDir,
    PluginMode mode,
    int protocolVersion = 0x0117);

/**
 * @brief  プラグインプロセスの終了を待つ
 *
 * タイムアウトを超えて子が終了しない場合は std::runtime_error を投げる。
 * 正常終了後は ClosePlugin() を呼んでハンドルを解放すること。
 *
 * @param  proc       対象プロセス
 * @param  timeoutMs  タイムアウト（ミリ秒）。-1 で無制限
 * @return 子プロセスの終了コード
 * @throws std::runtime_error タイムアウトした場合
 */
int WaitPlugin(PluginProcess& proc, int timeoutMs = -1);

/**
 * @brief  プラグインプロセスを強制終了する
 *
 * WaitPlugin() を使わずに即座に子を終了させたい場合に呼ぶ。
 * 呼び出し後は ClosePlugin() でリソースを解放すること。
 *
 * @param  proc  対象プロセス
 */
void TerminatePlugin(PluginProcess& proc);

/**
 * @brief  プロセスリソースを解放する（WaitPlugin / TerminatePlugin の後に呼ぶ）
 *
 * Windows: m_hProcess / m_hThread を CloseHandle する。
 * Mac:     特になし（waitpid は WaitPlugin 内で完了済み）。
 * readFd / writeFd は呼び出し側が事前に close しておくこと（wire_io 層の責務）。
 *
 * @param  proc  対象プロセス
 */
void ClosePlugin(PluginProcess& proc);
