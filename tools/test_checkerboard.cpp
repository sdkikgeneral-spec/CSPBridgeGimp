/**
 * @file   test_checkerboard.cpp
 * @brief  checkerboard.exe 直接起動による Wire Protocol 検証ツール
 * @author CSPBridgeGimp
 * @date   2026-05-01
 *
 * CSP 経由ではなく standalone で checkerboard.exe を spawn し、
 * Wire Protocol の各ステップ（spawn / GP_CONFIG / GP_PROC_RUN / メッセージループ）
 * で生存確認とメッセージダンプを行う。
 *
 * 用途: どのステップで "Plugin signal handler: ... fatal error" が発生するかを特定する。
 *
 * ビルド: meson compile -C build test_checkerboard
 * 実行:   build\test_checkerboard.exe
 *         (カレントディレクトリに config/bridge_config.json が必要)
 */

#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#endif

#include "config/config.h"
#include "host/pdb_stubs.h"
#include "host/tile_transfer.h"
#include "ipc/process.h"
#include "ipc/wire_io.h"

// ---------------------------------------------------------------------------
// Platform helpers
// ---------------------------------------------------------------------------

static bool IsAlive(const PluginProcess& proc)
{
#ifdef _WIN32
    return WaitForSingleObject(proc.m_hProcess, 0) == WAIT_TIMEOUT;
#else
    return true;
#endif
}

static uint32_t GetExitCode(const PluginProcess& proc)
{
#ifdef _WIN32
    DWORD code = 0xFFFFFFFFu;
    GetExitCodeProcess(proc.m_hProcess, &code);
    return static_cast<uint32_t>(code);
#else
    return 0u;
#endif
}

// Poll read pipe for available data.
// waitMs == 0: immediate check only; waitMs > 0: poll until data arrives or deadline.
static bool HasData(int readFd, int waitMs = 0)
{
#ifdef _WIN32
    HANDLE h = reinterpret_cast<HANDLE>(_get_osfhandle(readFd));
    if (h == INVALID_HANDLE_VALUE)
        return false;

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(waitMs);
    do
    {
        DWORD avail = 0;
        if (PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr) && avail > 0)
            return true;
        if (waitMs > 0)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (std::chrono::steady_clock::now() < deadline);

    return false;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Wire Protocol helpers
// ---------------------------------------------------------------------------

static const char* MsgTypeName(uint32_t t); // forward declaration

static const char* ParamDefTypeName(GpParamDefType t)
{
    switch (t)
    {
    case GpParamDefType::Default:       return "Default";
    case GpParamDefType::Int:           return "Int";
    case GpParamDefType::Unit:          return "Unit";
    case GpParamDefType::Enum:          return "Enum";
    case GpParamDefType::Choice:        return "Choice";
    case GpParamDefType::Boolean:       return "Boolean";
    case GpParamDefType::Double:        return "Double";
    case GpParamDefType::String:        return "String";
    case GpParamDefType::GeglColor:     return "GeglColor";
    case GpParamDefType::Id:            return "Id";
    case GpParamDefType::IdArray:       return "IdArray";
    case GpParamDefType::ExportOptions: return "ExportOptions";
    case GpParamDefType::Resource:      return "Resource";
    case GpParamDefType::File:          return "File";
    case GpParamDefType::Curve:         return "Curve";
    default:                            return "UNKNOWN";
    }
}

static void PrintProcInstall(const GpProcInstall& pi)
{
    printf("  name='%s' procType=%u\n", pi.name.c_str(), pi.procType);
    printf("  params (%zu):\n", pi.params.size());
    for (size_t i = 0; i < pi.params.size(); ++i)
    {
        const auto& pd = pi.params[i];
        printf("    [%zu] defType=%-13s typeName='%s' valueTypeName='%s' name='%s'\n",
            i, ParamDefTypeName(pd.paramDefType),
            pd.typeName.c_str(), pd.valueTypeName.c_str(), pd.name.c_str());
    }
    printf("  returnVals (%zu):\n", pi.returnVals.size());
    for (size_t i = 0; i < pi.returnVals.size(); ++i)
    {
        const auto& pd = pi.returnVals[i];
        printf("    [%zu] defType=%-13s typeName='%s' valueTypeName='%s' name='%s'\n",
            i, ParamDefTypeName(pd.paramDefType),
            pd.typeName.c_str(), pd.valueTypeName.c_str(), pd.name.c_str());
    }
}

// ---------------------------------------------------------------------------
// Query phase — spawn with -query to dump GP_PROC_INSTALL param definitions
// ---------------------------------------------------------------------------

static GpProcInstall RunQueryPhase(
    const std::string& exePath,
    const std::string& gimpLibDir)
{
    printf("=== QUERY PHASE ===\n");

    PluginProcess proc{};
    try
    {
        proc = SpawnPlugin(exePath, gimpLibDir, PluginMode::Query,
            static_cast<int>(GIMP_PROTOCOL_VERSION_3_2), "");
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "[query] SpawnPlugin failed: %s\n", e.what());
        return {};
    }

    WireChannel ch(proc.readFd, proc.writeFd);
    GpProcInstall result;

    try
    {
        printf("[query] Sending GP_CONFIG...\n");
        ch.WriteConfig(64u, 64u);

        bool running = true;
        while (running)
        {
            if (!HasData(proc.readFd, 3000))
            {
                printf("[query] Timeout — no data for 3 s\n");
                break;
            }

            const uint32_t msgType = ch.ReadUint32();
            printf("[query] recv %u (%s)\n", msgType, MsgTypeName(msgType));

            switch (static_cast<GpMessageType>(msgType))
            {
            case GpMessageType::HasInit:
                break;

            case GpMessageType::ProcInstall:
                result = ch.ReadProcInstall();
                PrintProcInstall(result);
                break;

            case GpMessageType::ProcUninstall:
                ch.ReadString();
                break;

            case GpMessageType::ProcRun:
            {
                auto msg = ch.ReadProcRun();
                printf("[query]   PDB call: '%s'\n", msg.name.c_str());
                ch.WriteProcReturn(msg.name);
                break;
            }

            case GpMessageType::Quit:
                printf("[query] GP_QUIT\n");
                running = false;
                break;

            default:
                printf("[query] unexpected msgType=%u — stopping\n", msgType);
                running = false;
                break;
            }
        }
    }
    catch (const WireError& e)
    {
        printf("[query] WireError: %s\n", e.what());
    }

    _close(proc.writeFd);
    proc.writeFd = -1;
#ifdef _WIN32
    WaitForSingleObject(proc.m_hProcess, 2000);
    printf("[query] exitCode=0x%08X\n", GetExitCode(proc));
#endif
    _close(proc.readFd);
    proc.readFd = -1;
    ClosePlugin(proc);

    printf("=== QUERY PHASE DONE ===\n\n");
    return result;
}

static const char* MsgTypeName(uint32_t t)
{
    switch (static_cast<GpMessageType>(t))
    {
    case GpMessageType::Quit:           return "GP_QUIT";
    case GpMessageType::Config:         return "GP_CONFIG";
    case GpMessageType::TileReq:        return "GP_TILE_REQ";
    case GpMessageType::TileAck:        return "GP_TILE_ACK";
    case GpMessageType::TileData:       return "GP_TILE_DATA";
    case GpMessageType::ProcRun:        return "GP_PROC_RUN";
    case GpMessageType::ProcReturn:     return "GP_PROC_RETURN";
    case GpMessageType::TempProcRun:    return "GP_TEMP_PROC_RUN";
    case GpMessageType::TempProcReturn: return "GP_TEMP_PROC_RETURN";
    case GpMessageType::ProcInstall:    return "GP_PROC_INSTALL";
    case GpMessageType::ProcUninstall:  return "GP_PROC_UNINSTALL";
    case GpMessageType::ExtensionAck:   return "GP_EXTENSION_ACK";
    case GpMessageType::HasInit:        return "GP_HAS_INIT";
    default:                             return "UNKNOWN";
    }
}

// Write GP_PROC_RUN to invoke plug-in-checkerboard noninteractively.
//
// Params confirmed from GP_PROC_INSTALL (Query phase) + GIMP 3.0.0 source:
//   [0] Enum      "GimpRunMode" = 1  (NONINTERACTIVE)
//   [1] Id        "GimpImage"   = IMAGE_ID
//   [2] IdArray   drawables     = [DRAWABLE_ID]
//   [3] Boolean   "gboolean"    = 0  (psychobilly = FALSE)
//   [4] Int       "gint"        = 16 (check-size in pixels)
//
// IdArray wire format (libgimpbase/gimpprotocol.c _gp_params_read, confirmed GIMP 3.0.0):
//   uint32  param_type           = GP_PARAM_TYPE_ID_ARRAY (=11)   [outer loop]
//   string  params[i].type_name  = "GimpCoreObjectArray"          [outer loop]
//   string  d_id_array.type_name = "GimpItem"  (element GType,    [IdArray case]
//                                   determined by gimp_value_to_gp_param:
//                                   GimpDrawable → GIMP_IS_ITEM → "GimpItem")
//   uint32  d_id_array.size
//   int32[] d_id_array.data[size]
static void WriteCheckerboardRun(WireChannel& ch)
{
    ch.WriteUint32(static_cast<uint32_t>(GpMessageType::ProcRun));
    ch.WriteString("plug-in-checkerboard");
    ch.WriteUint32(5u);

    // [0] run-mode (Enum → wire type Int)
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
    ch.WriteString("GimpRunMode");
    ch.WriteInt32(1); // GIMP_RUN_NONINTERACTIVE

    // [1] image (Id → wire type Int)
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
    ch.WriteString("GimpImage");
    ch.WriteInt32(HostContext::IMAGE_ID);

    // [2] drawables (IdArray, two strings per _gp_params_read outer+inner)
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::IdArray));
    ch.WriteString("GimpCoreObjectArray"); // params[i].type_name  (outer GType)
    ch.WriteString("GimpItem");            // d_id_array.type_name (element GType)
    ch.WriteUint32(1u);                    // size = 1
    ch.WriteInt32(HostContext::DRAWABLE_ID);

    // [3] psychobilly (Boolean → wire type Int, 0 = FALSE)
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
    ch.WriteString("gboolean");
    ch.WriteInt32(0); // FALSE

    // [4] check-size (Int, default = 10 per GIMP source; use 16 for test)
    ch.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
    ch.WriteString("gint");
    ch.WriteInt32(16);
}

// ---------------------------------------------------------------------------
// Message loop
// ---------------------------------------------------------------------------

static void RunMsgLoop(WireChannel& ch, PluginProcess& proc, HostContext& ctx)
{
    constexpr int POLL_INTERVAL_MS = 200;
    constexpr int MAX_NO_DATA      = 20; // 20 * 200ms = 4s

    int noDataCount = 0;

    while (IsAlive(proc))
    {
        if (!HasData(proc.readFd, POLL_INTERVAL_MS))
        {
            ++noDataCount;
            printf("  [loop] no data [%d/%d]\n", noDataCount, MAX_NO_DATA);
            if (noDataCount >= MAX_NO_DATA)
            {
                printf("  [loop] timeout — sending GP_QUIT\n");
                try { ch.WriteQuit(); } catch (...) {}
                break;
            }
            continue;
        }
        noDataCount = 0;

        uint32_t msgType = 0u;
        try
        {
            msgType = ch.ReadUint32();
        }
        catch (const WireError& e)
        {
            printf("  [loop] ReadUint32 WireError: %s\n", e.what());
            break;
        }

        printf("  [loop] recv %u (%s)\n", msgType, MsgTypeName(msgType));

        try
        {
            switch (static_cast<GpMessageType>(msgType))
            {
            case GpMessageType::HasInit:
                // no payload; host sends nothing in response
                break;

            case GpMessageType::ProcInstall:
            {
                auto inst = ch.ReadProcInstall();
                printf("    name='%s' params=%zu rets=%zu\n",
                    inst.name.c_str(), inst.params.size(), inst.returnVals.size());
                break;
            }

            case GpMessageType::ProcRun:
            {
                auto msg = ch.ReadProcRun();
                printf("    name='%s' n_params=%zu", msg.name.c_str(), msg.params.size());
                for (const auto& p : msg.params)
                {
                    if (p.paramType == GpParamType::Int)
                        printf("  [Int=%d]", p.intValue);
                    else if (p.paramType == GpParamType::String)
                        printf("  [Str='%s']", p.stringValue.c_str());
                }
                printf("\n");
                ctx.Dispatch(msg, ch);
                break;
            }

            case GpMessageType::TempProcRun:
            {
                auto msg = ch.ReadProcRun();
                printf("    name='%s' n_params=%zu\n",
                    msg.name.c_str(), msg.params.size());
                // Send GP_TEMP_PROC_RETURN = 8 with status=SUCCESS
                ch.WriteUint32(static_cast<uint32_t>(GpMessageType::TempProcReturn));
                ch.WriteString(msg.name);
                ch.WriteUint32(1u);
                ch.WriteUint32(static_cast<uint32_t>(GpParamType::Int));
                ch.WriteString("GimpPDBStatusType");
                ch.WriteInt32(GIMP_PDB_SUCCESS);
                break;
            }

            case GpMessageType::TileReq:
                HandleTileRequest(ch, ctx);
                printf("    TileGET=%d TilePUT=%d\n",
                    g_tileGetCount.load(), g_tilePutCount.load());
                break;

            case GpMessageType::ProcReturn:
            {
                // Final result from the run procedure: parse and print status.
                auto ret = ch.ReadProcRun(); // same wire format as GP_PROC_RUN
                printf("    name='%s' n_params=%zu\n",
                    ret.name.c_str(), ret.params.size());
                for (size_t i = 0; i < ret.params.size(); ++i)
                {
                    const auto& p = ret.params[i];
                    if (p.paramType == GpParamType::Int)
                        printf("    [%zu] Int = %d\n", i, p.intValue);
                    else if (p.paramType == GpParamType::String)
                        printf("    [%zu] String = '%s'\n", i, p.stringValue.c_str());
                    else
                        printf("    [%zu] paramType=%u\n", i, static_cast<uint32_t>(p.paramType));
                }
                printf("    -> filter complete\n");
                return;
            }

            case GpMessageType::Quit:
                printf("    -> plugin done\n");
                return;

            default:
                printf("    -> unparseable, stopping loop\n");
                return;
            }
        }
        catch (const WireError& e)
        {
            printf("  [loop] WireError during handling: %s\n", e.what());
            break;
        }
    }

    if (!IsAlive(proc))
        printf("  [loop] plugin died\n");
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main()
{
    // -----------------------------------------------------------------------
    // Step 1: Load config
    // -----------------------------------------------------------------------
    const std::string configPath = "config/bridge_config.json";
    BridgeConfig cfg = LoadConfig(configPath);
    printf("[cfg] gimpLibDir : %s\n", cfg.gimpLibDir.c_str());

    // -----------------------------------------------------------------------
    // Step 2: Find checkerboard.exe
    // -----------------------------------------------------------------------
    const std::string exePath = FindPluginExe(cfg, "checkerboard");
    if (exePath.empty())
    {
        fprintf(stderr, "ERROR: checkerboard.exe not found in plugin search paths\n");
        return 1;
    }
    printf("[cfg] exePath    : %s\n\n", exePath.c_str());

    // -----------------------------------------------------------------------
    // Step 2.5: Query phase — read GP_PROC_INSTALL to verify param definitions
    // -----------------------------------------------------------------------
    const GpProcInstall queryResult = RunQueryPhase(exePath, cfg.gimpLibDir);
    if (queryResult.name.empty())
        printf("[warn] Query phase returned no GP_PROC_INSTALL — continuing with hardcoded params\n\n");

    // -----------------------------------------------------------------------
    // Step 3: Spawn — stderr goes to this console so we see GLib crash messages
    // -----------------------------------------------------------------------
    const std::string runStderrLog = "build\\checkerboard_run_stderr.log";
    printf("[spawn] SpawnPlugin(PluginMode::Run, protocol=0x%04X)...\n",
        GIMP_PROTOCOL_VERSION_3_2);
    printf("[spawn] Plugin stderr → %s\n", runStderrLog.c_str());
    PluginProcess proc{};
    try
    {
        proc = SpawnPlugin(exePath, cfg.gimpLibDir, PluginMode::Run,
            static_cast<int>(GIMP_PROTOCOL_VERSION_3_2), runStderrLog);
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "ERROR: SpawnPlugin failed: %s\n", e.what());
        return 1;
    }
    printf("[spawn] readFd=%d writeFd=%d\n", proc.readFd, proc.writeFd);

    WireChannel ch(proc.readFd, proc.writeFd);

    // -----------------------------------------------------------------------
    // Step 4: Alive check — immediately after spawn (100 ms)
    // -----------------------------------------------------------------------
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!IsAlive(proc))
    {
        printf("[alive] DEAD immediately (exitCode=0x%08X)\n", GetExitCode(proc));
        printf("  -> Crash before reading GP_CONFIG\n");
        _close(proc.readFd);
        _close(proc.writeFd);
        ClosePlugin(proc);
        return 1;
    }
    printf("[alive] OK (100ms after spawn)\n\n");

    // -----------------------------------------------------------------------
    // Step 5: Send GP_CONFIG
    // -----------------------------------------------------------------------
    printf("[send] GP_CONFIG\n");
    try
    {
        ch.WriteConfig(64u, 64u);
    }
    catch (const WireError& e)
    {
        printf("[send] GP_CONFIG WireError: %s\n", e.what());
    }

    // -----------------------------------------------------------------------
    // Step 6: Alive check after GP_CONFIG (500 ms — gegl_init can be slow)
    // -----------------------------------------------------------------------
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    if (!IsAlive(proc))
    {
        printf("[alive] DEAD after GP_CONFIG (exitCode=0x%08X)\n", GetExitCode(proc));
        printf("  -> Crash during early plugin init (gegl_init / gimp_plug_in_init)\n");
        _close(proc.readFd);
        _close(proc.writeFd);
        ClosePlugin(proc);
        return 1;
    }
    printf("[alive] OK (500ms after GP_CONFIG)\n");

    // Peek: did the plugin send GP_HAS_INIT already?
    if (HasData(proc.readFd))
        printf("[recv] Plugin has data pending (likely GP_HAS_INIT)\n");
    else
        printf("[recv] No data from plugin yet\n");
    printf("\n");

    // -----------------------------------------------------------------------
    // Step 7: Send GP_PROC_RUN for plug-in-checkerboard
    // -----------------------------------------------------------------------
    HostContext ctx(200u, 200u); // small test image, all zeros (transparent)
    ctx.SetLogCallback([](const char* msg) { printf("    [pdb] %s", msg); });
    g_tileGetCount.store(0);
    g_tilePutCount.store(0);

    printf("[send] GP_PROC_RUN plug-in-checkerboard (5 params)\n");
    printf("  [0] Int     GimpRunMode  = 1 (NONINTERACTIVE)\n");
    printf("  [1] Int     GimpImage    = %d\n", HostContext::IMAGE_ID);
    printf("  [2] IdArray GimpCoreObjectArray/GimpItem = [%d]\n", HostContext::DRAWABLE_ID);
    printf("  [3] Int     gboolean (psychobilly) = 0 (FALSE)\n");
    printf("  [4] Int     gint     (check-size)  = 16\n");
    try
    {
        WriteCheckerboardRun(ch);
    }
    catch (const WireError& e)
    {
        printf("[send] GP_PROC_RUN WireError: %s\n", e.what());
    }

    // -----------------------------------------------------------------------
    // Step 8+9: Enter message loop immediately (no sleep) so we can respond
    //            to any PDB calls the plugin makes while processing the call.
    //            The loop itself checks alive after each no-data poll interval.
    // -----------------------------------------------------------------------
    printf("[loop] Entering message loop immediately after GP_PROC_RUN...\n");
    RunMsgLoop(ch, proc, ctx);

    printf("\n[stats] TileGET=%d TilePUT=%d\n",
        g_tileGetCount.load(), g_tilePutCount.load());

    // -----------------------------------------------------------------------
    // Step 10: Cleanup
    // -----------------------------------------------------------------------
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Close write fd first to send EOF to child
    _close(proc.writeFd);
    proc.writeFd = -1;

#ifdef _WIN32
    WaitForSingleObject(proc.m_hProcess, 3000);
    printf("[exit] exitCode=0x%08X\n", GetExitCode(proc));
#endif

    _close(proc.readFd);
    proc.readFd = -1;

    ClosePlugin(proc);

    // -----------------------------------------------------------------------
    // Step 11: Print plugin stderr log (GLib error messages)
    // -----------------------------------------------------------------------
    printf("\n[stderr] Plugin stderr log (%s):\n", runStderrLog.c_str());
    if (FILE* fp = std::fopen(runStderrLog.c_str(), "r"))
    {
        char line[512];
        int lineCount = 0;
        while (std::fgets(line, sizeof(line), fp))
        {
            printf("  %s", line);
            ++lineCount;
        }
        std::fclose(fp);
        if (lineCount == 0)
            printf("  (empty)\n");
    }
    else
    {
        printf("  (file not found)\n");
    }

    return 0;
}
