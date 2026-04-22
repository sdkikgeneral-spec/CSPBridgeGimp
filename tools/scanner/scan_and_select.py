"""
@file   scan_and_select.py
@brief  GIMP プラグインをスキャンして plugins.json を生成する独立ツール
@author CSPBridgeGimp
@date   2026-04-22

このスクリプトは `meson setup` の前に **手動で 1 回** 実行する。
`config/bridge_config.json` に定義された検索パスを走査し、
見つかった GIMP プラグイン EXE を query モードで起動して
Wire Protocol のメタデータを収集、tkinter チェックリスト GUI で
ユーザーが選択したプラグインを `plugins.json` に書き出す。

処理フロー:
    1. bridge_config.json を読み込み、プラットフォーム別の
       plugin_search_paths を取得（プレースホルダー展開）
    2. 各パスを走査して *.exe を列挙
    3. 各 EXE に対し Wire Protocol クエリフェーズを実行：
       a. 匿名パイプを作成（継承可能な OS ハンドル／fd）
       b. 子プロセス起動: EXE -gimp <version> <read_fd> <write_fd> -query 0
       c. プラグインは GP_PROC_INSTALL と PDB 呼び出し
          (GP_PROC_RUN) を連続送信してくる
       d. スキャナー側は GP_PROC_RUN に対し空の成功応答
          (GP_PROC_RETURN, n_params=1, status=GIMP_PDB_SUCCESS) を返す
       e. EOF か timeout まで読み続ける
    4. tkinter で選択ダイアログを表示
    5. plugins.json を書き出し

使用するメッセージ型（libgimpbase/gimpprotocol.h, GIMP 3.0 tag = 0x0115）:
    GP_QUIT           = 0
    GP_CONFIG         = 1
    GP_TILE_REQ       = 2
    GP_TILE_ACK       = 3
    GP_TILE_DATA      = 4
    GP_PROC_RUN       = 5
    GP_PROC_RETURN    = 6
    GP_TEMP_PROC_RUN  = 7
    GP_TEMP_PROC_RETURN = 8
    GP_PROC_INSTALL   = 9
    GP_PROC_UNINSTALL = 10
    GP_EXTENSION_ACK  = 11
    GP_HAS_INIT       = 12

ワイヤーフォーマット:
    * すべての整数はネットワークバイトオーダー (big-endian)
    * 文字列 = uint32(length_with_nul) + length bytes (末尾 NUL)
      length == 0 は NULL 文字列
    * メッセージ = uint32(type) + 型ごとのペイロード（長さ前置なし）

プラグイン起動引数（libgimp/gimp.c の gimp_main 冒頭を参照）:
    progname -gimp <protocol_version> <read_fd> <write_fd> <mode> <stack_trace>
    例: plug-in-gauss.exe -gimp 277 3 4 -query 0
        277 = GIMP_PROTOCOL_VERSION (0x0115)
        read_fd/write_fd はプラグイン視点の fd
        mode: -query / -init / -run
        stack_trace: 0 = GIMP_STACK_TRACE_NEVER

備考・既知の制約:
    * GIMP 3.0 では GP_QUERY メッセージは存在しない。query モードは
      起動時引数で指定する。プラグインは GP_CONFIG を待たず
      いきなり GP_PROC_INSTALL を書いてくる。
    * menu_label / menu_path / blurb は GP_PROC_INSTALL には含まれず、
      プラグインは PDB 手続き (gimp-pdb-set-proc-menu-label 等) を
      GP_PROC_RUN で呼び出して送ってくる。スキャナーはこれを傍受。
    * Mac (POSIX) の FD 継承は `os.set_inheritable` + `pass_fds` で
      素直に渡るが、本 PoC では Windows 実装を優先。
      Mac 版は sys.platform 分岐 + TODO コメントで対応。
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import platform
import struct
import subprocess
import sys
import traceback
import tkinter as tk
from dataclasses import dataclass, field
from tkinter import messagebox, ttk
from typing import Any, BinaryIO, Optional

# ---------------------------------------------------------------------------
# 定数
# ---------------------------------------------------------------------------

GIMP_PROTOCOL_VERSION: int = 0x0115  # = 277 decimal, GIMP 3.0.x

# メッセージタイプ番号 (libgimpbase/gimpprotocol.h の enum 順)
GP_QUIT: int = 0
GP_CONFIG: int = 1
GP_TILE_REQ: int = 2
GP_TILE_ACK: int = 3
GP_TILE_DATA: int = 4
GP_PROC_RUN: int = 5
GP_PROC_RETURN: int = 6
GP_TEMP_PROC_RUN: int = 7
GP_TEMP_PROC_RETURN: int = 8
GP_PROC_INSTALL: int = 9
GP_PROC_UNINSTALL: int = 10
GP_EXTENSION_ACK: int = 11
GP_HAS_INIT: int = 12

# GPParamType (libgimpbase/gimpprotocol.h)
GP_PARAM_TYPE_INT: int = 0
GP_PARAM_TYPE_DOUBLE: int = 1
GP_PARAM_TYPE_STRING: int = 2
GP_PARAM_TYPE_STRV: int = 3
GP_PARAM_TYPE_BYTES: int = 4
GP_PARAM_TYPE_FILE: int = 5
GP_PARAM_TYPE_BABL_FORMAT: int = 6
GP_PARAM_TYPE_GEGL_COLOR: int = 7
GP_PARAM_TYPE_COLOR_ARRAY: int = 8
GP_PARAM_TYPE_PARASITE: int = 9
GP_PARAM_TYPE_ARRAY: int = 10
GP_PARAM_TYPE_ID_ARRAY: int = 11
GP_PARAM_TYPE_EXPORT_OPTIONS: int = 12
GP_PARAM_TYPE_PARAM_DEF: int = 13
GP_PARAM_TYPE_VALUE_ARRAY: int = 14

# GPParamDefType
GP_PARAM_DEF_TYPE_DEFAULT: int = 0
GP_PARAM_DEF_TYPE_INT: int = 1
GP_PARAM_DEF_TYPE_UNIT: int = 2
GP_PARAM_DEF_TYPE_ENUM: int = 3
GP_PARAM_DEF_TYPE_CHOICE: int = 4
GP_PARAM_DEF_TYPE_BOOLEAN: int = 5
GP_PARAM_DEF_TYPE_DOUBLE: int = 6
GP_PARAM_DEF_TYPE_STRING: int = 7
GP_PARAM_DEF_TYPE_GEGL_COLOR: int = 8
GP_PARAM_DEF_TYPE_ID: int = 9
GP_PARAM_DEF_TYPE_ID_ARRAY: int = 10
GP_PARAM_DEF_TYPE_EXPORT_OPTIONS: int = 11
GP_PARAM_DEF_TYPE_RESOURCE: int = 12
GP_PARAM_DEF_TYPE_FILE: int = 13

# PDB status
GIMP_PDB_SUCCESS: int = 0

# タイムアウト（秒）。PoC ではプラグインごとに 5s 程度で十分
SCAN_TIMEOUT_SEC: float = 5.0

# GP_PARAM_TYPE を人間可読名にマップ（plugins.json 出力用）
PARAM_TYPE_NAMES: dict[int, str] = {
    GP_PARAM_TYPE_INT: "INT32",
    GP_PARAM_TYPE_DOUBLE: "DOUBLE",
    GP_PARAM_TYPE_STRING: "STRING",
    GP_PARAM_TYPE_STRV: "STRV",
    GP_PARAM_TYPE_BYTES: "BYTES",
    GP_PARAM_TYPE_FILE: "FILE",
    GP_PARAM_TYPE_BABL_FORMAT: "BABL_FORMAT",
    GP_PARAM_TYPE_GEGL_COLOR: "GEGL_COLOR",
    GP_PARAM_TYPE_COLOR_ARRAY: "COLOR_ARRAY",
    GP_PARAM_TYPE_PARASITE: "PARASITE",
    GP_PARAM_TYPE_ARRAY: "ARRAY",
    GP_PARAM_TYPE_ID_ARRAY: "ID_ARRAY",
    GP_PARAM_TYPE_EXPORT_OPTIONS: "EXPORT_OPTIONS",
    GP_PARAM_TYPE_PARAM_DEF: "PARAM_DEF",
    GP_PARAM_TYPE_VALUE_ARRAY: "VALUE_ARRAY",
}


# ---------------------------------------------------------------------------
# データクラス
# ---------------------------------------------------------------------------


@dataclass
class ParamDef:
    """GP_PROC_INSTALL の param/return_val 1 エントリ。"""

    param_def_type: int
    type_name: str
    value_type_name: str
    name: str
    nick: str
    blurb: str
    flags: int


@dataclass
class PluginMetadata:
    """1 つの EXE から取得した全メタデータ。"""

    exe_path: str
    procedure: str = ""
    proc_type: int = 0
    params: list[ParamDef] = field(default_factory=list)
    return_vals: list[ParamDef] = field(default_factory=list)
    menu_label: str = ""
    menu_paths: list[str] = field(default_factory=list)
    blurb: str = ""
    help: str = ""
    help_id: str = ""
    authors: str = ""
    copyright: str = ""
    date: str = ""
    error: Optional[str] = None

    @property
    def is_valid(self) -> bool:
        return bool(self.procedure) and self.error is None

    @property
    def display_menu(self) -> str:
        if self.menu_paths and self.menu_label:
            return f"{self.menu_paths[0]}/{self.menu_label}"
        if self.menu_paths:
            return self.menu_paths[0]
        return self.menu_label

    @property
    def suggested_id(self) -> str:
        """plugins.json 用の id。procedure 名から英数字のみ抽出。"""
        raw = self.procedure or pathlib.Path(self.exe_path).stem
        out: list[str] = []
        next_upper = True
        for ch in raw:
            if ch.isalnum():
                out.append(ch.upper() if next_upper else ch)
                next_upper = False
            else:
                next_upper = True
        return "".join(out) or "Plugin"


# ---------------------------------------------------------------------------
# Wire I/O primitives
# ---------------------------------------------------------------------------


class WireError(Exception):
    """Wire Protocol 読み書き中の異常。EOF / 不正データ / タイムアウト。"""


def _read_exact(stream: BinaryIO, n: int) -> bytes:
    """@brief  n バイト読み切る。足りなければ WireError。"""
    if n < 0:
        raise WireError(f"negative read length: {n}")
    if n == 0:
        return b""
    buf = bytearray()
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            raise WireError(
                f"unexpected EOF: wanted {n} bytes, got {len(buf)}"
            )
        buf.extend(chunk)
    return bytes(buf)


def read_uint32(stream: BinaryIO) -> int:
    """@brief  ネットワークバイトオーダー (big-endian) の uint32 を読む。"""
    return struct.unpack(">I", _read_exact(stream, 4))[0]


def read_int32(stream: BinaryIO) -> int:
    """@brief  ネットワークバイトオーダーの int32 を読む（符号付き）。"""
    return struct.unpack(">i", _read_exact(stream, 4))[0]


def read_int64(stream: BinaryIO) -> int:
    """@brief  ネットワークバイトオーダーの int64 を読む（符号付き）。"""
    return struct.unpack(">q", _read_exact(stream, 8))[0]


def read_double(stream: BinaryIO) -> float:
    """@brief  ネットワークバイトオーダーの IEEE 754 double を読む。"""
    return struct.unpack(">d", _read_exact(stream, 8))[0]


def read_uint8(stream: BinaryIO) -> int:
    """@brief  1 バイト unsigned。"""
    return _read_exact(stream, 1)[0]


def read_string(stream: BinaryIO) -> Optional[str]:
    """
    @brief  Wire Protocol 文字列を読む。

    フォーマット: `uint32 length` + `length bytes`（最後は NUL）。
    length == 0 は NULL（None）を意味する。

    @return デコード済み文字列。NULL のときは None
    """
    length = read_uint32(stream)
    if length == 0:
        return None
    raw = _read_exact(stream, length)
    # 末尾の NUL を除去
    if raw.endswith(b"\x00"):
        raw = raw[:-1]
    try:
        return raw.decode("utf-8", errors="replace")
    except Exception:
        return raw.decode("latin-1", errors="replace")


def read_bytes_blob(stream: BinaryIO) -> bytes:
    """@brief  uint32 長 + 生バイト列を読む。"""
    length = read_uint32(stream)
    return _read_exact(stream, length)


def write_uint32(stream: BinaryIO, value: int) -> None:
    """@brief  uint32 を big-endian で書く。"""
    stream.write(struct.pack(">I", value & 0xFFFFFFFF))


def write_int32(stream: BinaryIO, value: int) -> None:
    """@brief  int32 を big-endian で書く。"""
    stream.write(struct.pack(">i", value))


def write_string(stream: BinaryIO, value: Optional[str]) -> None:
    """
    @brief  Wire Protocol 文字列を書く。None は length=0。
    """
    if value is None:
        write_uint32(stream, 0)
        return
    encoded = value.encode("utf-8") + b"\x00"
    write_uint32(stream, len(encoded))
    stream.write(encoded)


# ---------------------------------------------------------------------------
# GPParamDef / GPParam readers
# ---------------------------------------------------------------------------


def read_gegl_color(stream: BinaryIO) -> None:
    """
    @brief  GeglColor 構造を読み飛ばす。内容は本ツールでは使わない。

    フォーマット: uint32 size (<=40) + size bytes + string encoding +
                 uint32 icc_length + icc_length bytes
    """
    size = read_uint32(stream)
    if size > 40:
        raise WireError(f"gegl color size too large: {size}")
    _read_exact(stream, size)
    _ = read_string(stream)  # encoding
    icc_length = read_uint32(stream)
    _read_exact(stream, icc_length)


def read_param_def(stream: BinaryIO) -> ParamDef:
    """
    @brief  GPParamDef を読む（libgimpbase/gimpprotocol.c _gp_param_def_read 準拠）
    """
    param_def_type = read_uint32(stream)
    type_name = read_string(stream) or ""
    value_type_name = read_string(stream) or ""
    name = read_string(stream) or ""
    nick = read_string(stream) or ""
    blurb = read_string(stream) or ""
    flags = read_uint32(stream)

    # param_def_type ごとの meta データを読み飛ばす
    if param_def_type in (GP_PARAM_DEF_TYPE_DEFAULT,
                          GP_PARAM_DEF_TYPE_EXPORT_OPTIONS):
        pass
    elif param_def_type == GP_PARAM_DEF_TYPE_INT:
        read_int64(stream)  # min
        read_int64(stream)  # max
        read_int64(stream)  # default
    elif param_def_type == GP_PARAM_DEF_TYPE_UNIT:
        read_uint32(stream)  # allow_pixels
        read_uint32(stream)  # allow_percent
        read_uint32(stream)  # default
    elif param_def_type == GP_PARAM_DEF_TYPE_ENUM:
        read_uint32(stream)
    elif param_def_type == GP_PARAM_DEF_TYPE_BOOLEAN:
        read_uint32(stream)
    elif param_def_type == GP_PARAM_DEF_TYPE_DOUBLE:
        read_double(stream)
        read_double(stream)
        read_double(stream)
    elif param_def_type == GP_PARAM_DEF_TYPE_STRING:
        _ = read_string(stream)
    elif param_def_type == GP_PARAM_DEF_TYPE_CHOICE:
        _ = read_string(stream)  # default_val nick
        size = read_uint32(stream)
        for _i in range(size):
            _ = read_string(stream)  # nick
            read_uint32(stream)       # id
            _ = read_string(stream)  # label
            _ = read_string(stream)  # help
    elif param_def_type == GP_PARAM_DEF_TYPE_GEGL_COLOR:
        read_uint32(stream)  # has_alpha
        read_gegl_color(stream)
    elif param_def_type == GP_PARAM_DEF_TYPE_ID:
        read_uint32(stream)  # none_ok
    elif param_def_type == GP_PARAM_DEF_TYPE_ID_ARRAY:
        _ = read_string(stream)  # type_name
    elif param_def_type == GP_PARAM_DEF_TYPE_RESOURCE:
        read_uint32(stream)  # none_ok
        read_uint32(stream)  # default_to_context
        read_uint32(stream)  # default_resource_id
    elif param_def_type == GP_PARAM_DEF_TYPE_FILE:
        read_uint32(stream)  # action
        read_uint32(stream)  # none_ok
        _ = read_string(stream)  # default_uri
    else:
        raise WireError(f"unknown param_def_type: {param_def_type}")

    return ParamDef(
        param_def_type=param_def_type,
        type_name=type_name,
        value_type_name=value_type_name,
        name=name,
        nick=nick,
        blurb=blurb,
        flags=flags,
    )


def read_param_value(stream: BinaryIO) -> Any:
    """
    @brief  GPParam を 1 つ読み、data のペイロードを Python 値で返す。

    PDB 呼び出しの引数を抽出するために最低限の型だけ中身を取り出す。
    不要な型は読み飛ばす。

    @return param_type と value のタプル (int, Any)
    """
    param_type = read_uint32(stream)
    type_name = read_string(stream)  # 使わないが読み飛ばす
    _ = type_name

    value: Any = None

    if param_type == GP_PARAM_TYPE_INT:
        value = read_int32(stream)
    elif param_type == GP_PARAM_TYPE_DOUBLE:
        value = read_double(stream)
    elif param_type in (GP_PARAM_TYPE_STRING, GP_PARAM_TYPE_FILE):
        value = read_string(stream)
    elif param_type == GP_PARAM_TYPE_BABL_FORMAT:
        _ = read_string(stream)      # encoding
        size = read_uint32(stream)
        _read_exact(stream, size)    # profile_data
        value = None
    elif param_type == GP_PARAM_TYPE_GEGL_COLOR:
        size = read_uint32(stream)
        if size > 40:
            raise WireError(f"gegl color size too large: {size}")
        _read_exact(stream, size)
        _ = read_string(stream)           # encoding
        profile_size = read_uint32(stream)
        _read_exact(stream, profile_size)
        value = None
    elif param_type == GP_PARAM_TYPE_COLOR_ARRAY:
        count = read_uint32(stream)
        for _i in range(count):
            size = read_uint32(stream)
            if size > 40:
                raise WireError(f"gegl color size too large: {size}")
            _read_exact(stream, size)
            _ = read_string(stream)
            profile_size = read_uint32(stream)
            _read_exact(stream, profile_size)
        value = None
    elif param_type == GP_PARAM_TYPE_ARRAY:
        size = read_uint32(stream)
        _read_exact(stream, size)
        value = None
    elif param_type == GP_PARAM_TYPE_BYTES:
        data_len = read_uint32(stream)
        value = _read_exact(stream, data_len)
    elif param_type == GP_PARAM_TYPE_STRV:
        count = read_uint32(stream)
        strs: list[Optional[str]] = [read_string(stream) for _ in range(count)]
        value = strs
    elif param_type == GP_PARAM_TYPE_ID_ARRAY:
        _ = read_string(stream)  # type_name
        size = read_uint32(stream)
        for _i in range(size):
            read_int32(stream)
        value = None
    elif param_type == GP_PARAM_TYPE_PARASITE:
        name = read_string(stream)
        if name is not None:
            read_uint32(stream)  # flags
            size = read_uint32(stream)
            if size > 0:
                _read_exact(stream, size)
        value = None
    elif param_type == GP_PARAM_TYPE_EXPORT_OPTIONS:
        # 空の構造（現時点でフィールドなし）
        value = None
    elif param_type == GP_PARAM_TYPE_PARAM_DEF:
        value = read_param_def(stream)
    elif param_type == GP_PARAM_TYPE_VALUE_ARRAY:
        value = read_params(stream)
    else:
        raise WireError(f"unknown param_type: {param_type}")

    return (param_type, value)


def read_params(stream: BinaryIO) -> list[tuple[int, Any]]:
    """@brief  uint32 n_params + n_params 個の GPParam を読む。"""
    n = read_uint32(stream)
    return [read_param_value(stream) for _ in range(n)]


# ---------------------------------------------------------------------------
# メッセージごとのハンドラ
# ---------------------------------------------------------------------------


def read_proc_install(stream: BinaryIO, meta: PluginMetadata) -> None:
    """
    @brief  GP_PROC_INSTALL を読み取って meta に格納する。
    """
    name = read_string(stream) or ""
    proc_type = read_uint32(stream)
    n_params = read_uint32(stream)
    n_return_vals = read_uint32(stream)

    params = [read_param_def(stream) for _ in range(n_params)]
    return_vals = [read_param_def(stream) for _ in range(n_return_vals)]

    # 最初の 1 つだけ採用（フィルター系 EXE は 1 手続き 1 EXE が標準）
    if not meta.procedure:
        meta.procedure = name
        meta.proc_type = proc_type
        meta.params = params
        meta.return_vals = return_vals


def read_proc_run(stream: BinaryIO, meta: PluginMetadata) -> str:
    """
    @brief  GP_PROC_RUN を読み取って、必要ならメタデータを抽出する。

    @return プラグインが呼び出した PDB 手続き名（応答のために呼び出し元が使う）
    """
    name = read_string(stream) or ""
    params = read_params(stream)

    # PDB 手続きごとに引数を解釈
    # 形式: (param_type, value) のリスト
    def _str_at(idx: int) -> str:
        if idx < len(params):
            _pt, v = params[idx]
            if isinstance(v, str):
                return v
        return ""

    if name == "gimp-pdb-set-proc-menu-label":
        # (procedure_name, menu_label)
        if _str_at(0) == meta.procedure or not meta.procedure:
            meta.menu_label = _str_at(1)
    elif name == "gimp-pdb-add-proc-menu-path":
        # (procedure_name, menu_path)
        if _str_at(0) == meta.procedure or not meta.procedure:
            meta.menu_paths.append(_str_at(1))
    elif name == "gimp-pdb-set-proc-documentation":
        # (procedure_name, blurb, help, help_id)
        if _str_at(0) == meta.procedure or not meta.procedure:
            meta.blurb = _str_at(1)
            meta.help = _str_at(2)
            meta.help_id = _str_at(3)
    elif name == "gimp-pdb-set-proc-attribution":
        # (procedure_name, authors, copyright, date)
        if _str_at(0) == meta.procedure or not meta.procedure:
            meta.authors = _str_at(1)
            meta.copyright = _str_at(2)
            meta.date = _str_at(3)

    return name


def read_generic_message(stream: BinaryIO, msg_type: int) -> None:
    """
    @brief  関心のないメッセージ種別を読み飛ばす。

    本スキャナーではクエリ時に飛んでくる可能性のある補助メッセージ
    （GP_HAS_INIT など）を静かに読み飛ばすために使う。未知の型は例外。
    """
    if msg_type in (GP_QUIT, GP_TILE_ACK, GP_EXTENSION_ACK, GP_HAS_INIT):
        # ペイロード無し
        return

    if msg_type == GP_PROC_UNINSTALL:
        _ = read_string(stream)
        return

    # それ以外（クエリ時には通常来ない）はサイズ不明のため
    # 安全に続行できない。WireError を上げてスキャン打ち切り。
    raise WireError(f"unexpected message type during query: {msg_type}")


def write_proc_return(
    stream: BinaryIO,
    name: str,
    status: int = GIMP_PDB_SUCCESS,
) -> None:
    """
    @brief  空の GP_PROC_RETURN を書いて送信する。

    プラグインは PDB 呼び出しに対する応答を同期的に待っているので、
    スキャナーとしては呼び出し内容によらず成功ステータス 1 件だけを
    返して進行させる。

    ペイロード形式:
        uint32 msg_type (=6)
        string proc_name
        uint32 n_params (=1)
            uint32 param_type (=GP_PARAM_TYPE_INT)
            string type_name  (="GimpPDBStatusType")
            int32  d_int      (=GIMP_PDB_SUCCESS)
    """
    write_uint32(stream, GP_PROC_RETURN)
    write_string(stream, name)
    write_uint32(stream, 1)  # n_params
    write_uint32(stream, GP_PARAM_TYPE_INT)
    write_string(stream, "GimpPDBStatusType")
    write_int32(stream, status)
    stream.flush()


# ---------------------------------------------------------------------------
# 子プロセス起動 (Windows / POSIX)
# ---------------------------------------------------------------------------


@dataclass
class PluginProcess:
    """子プロセスと、親側から見た read/write ストリームの組。"""

    process: subprocess.Popen
    read_from_plugin: BinaryIO   # プラグイン stdout 相当。親が読む
    write_to_plugin: BinaryIO    # プラグイン stdin 相当。親が書く


def _spawn_plugin_windows(exe_path: str) -> PluginProcess:
    """
    @brief  Windows で GIMP プラグイン EXE をクエリモードで起動する。

    Python の os.pipe() は MSVCRT fd を返し、`os.set_inheritable(fd, True)`
    を立てた上で `subprocess.Popen(..., close_fds=False)` で渡せば
    子プロセスに fd 番号が引き継がれる。GIMP 3 の plug-in は
    `g_io_channel_win32_new_fd(atoi(argv[...]))` で fd を直接開くため、
    この方式で互換が取れる。

    既知リスク (GIMP 3 実機未検証):
        Python の subprocess.Popen(close_fds=False) は Win32 HANDLE を
        継承させるが、MSVCRT の fd テーブルへの再登録
        (STARTUPINFO.lpReserved2 / cbReserved2 経由) は行わない。
        GIMP プラグインは g_io_channel_win32_new_fd(atoi(argv[...])) で
        fd を開き直すが、これが成功するかは実測次第。
        もし失敗した場合の対処:
          (a) ctypes で CreateProcessW + lpReserved2 に MSVCRT fd マップを設定
          (b) 匿名パイプを named pipe に切り替え、argv にパイプ名を渡す
          (c) GIMP 側にパッチ (LGPL なので差分公開)
        詳細は docs/spec.md §5.6 参照。
    """
    # parent_read <- plugin_write
    parent_read_fd, plugin_write_fd = os.pipe()
    # plugin_read <- parent_write
    plugin_read_fd, parent_write_fd = os.pipe()

    # 子プロセス側の fd を継承可にする
    os.set_inheritable(plugin_read_fd, True)
    os.set_inheritable(plugin_write_fd, True)
    # 親側の fd は継承させない（CLOEXEC 相当）
    os.set_inheritable(parent_read_fd, False)
    os.set_inheritable(parent_write_fd, False)

    argv = [
        exe_path,
        "-gimp",
        str(GIMP_PROTOCOL_VERSION),
        str(plugin_read_fd),
        str(plugin_write_fd),
        "-query",
        "0",  # GIMP_STACK_TRACE_NEVER
    ]

    try:
        proc = subprocess.Popen(
            argv,
            close_fds=False,
            pass_fds=(plugin_read_fd, plugin_write_fd)
            if sys.platform != "win32"
            else (),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
    except Exception:
        os.close(parent_read_fd)
        os.close(parent_write_fd)
        os.close(plugin_read_fd)
        os.close(plugin_write_fd)
        raise

    # 親プロセス側では子側の fd は不要なので閉じる
    os.close(plugin_read_fd)
    os.close(plugin_write_fd)

    read_stream = os.fdopen(parent_read_fd, "rb", buffering=0)
    write_stream = os.fdopen(parent_write_fd, "wb", buffering=0)

    return PluginProcess(
        process=proc,
        read_from_plugin=read_stream,
        write_to_plugin=write_stream,
    )


def _spawn_plugin_posix(exe_path: str) -> PluginProcess:
    """
    @brief  POSIX (Mac/Linux) での起動。

    TODO: Mac 対応を本格化する際は、このパスを整える。
    現時点では Windows と同様に os.pipe + pass_fds で概ね動くはず。
    """
    # POSIX の os.pipe() は継承フラグがデフォルト False（Python 3.4+）
    parent_read_fd, plugin_write_fd = os.pipe()
    plugin_read_fd, parent_write_fd = os.pipe()

    os.set_inheritable(plugin_read_fd, True)
    os.set_inheritable(plugin_write_fd, True)

    argv = [
        exe_path,
        "-gimp",
        str(GIMP_PROTOCOL_VERSION),
        str(plugin_read_fd),
        str(plugin_write_fd),
        "-query",
        "0",
    ]

    try:
        proc = subprocess.Popen(
            argv,
            close_fds=True,
            pass_fds=(plugin_read_fd, plugin_write_fd),
            stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
    except Exception:
        os.close(parent_read_fd)
        os.close(parent_write_fd)
        os.close(plugin_read_fd)
        os.close(plugin_write_fd)
        raise

    os.close(plugin_read_fd)
    os.close(plugin_write_fd)

    read_stream = os.fdopen(parent_read_fd, "rb", buffering=0)
    write_stream = os.fdopen(parent_write_fd, "wb", buffering=0)

    return PluginProcess(
        process=proc,
        read_from_plugin=read_stream,
        write_to_plugin=write_stream,
    )


def spawn_plugin(exe_path: str) -> PluginProcess:
    """@brief  プラットフォーム別の起動関数ディスパッチ。"""
    if sys.platform.startswith("win"):
        return _spawn_plugin_windows(exe_path)
    return _spawn_plugin_posix(exe_path)


# ---------------------------------------------------------------------------
# スキャナー本体
# ---------------------------------------------------------------------------


def query_plugin(exe_path: str, verbose: bool = False) -> PluginMetadata:
    """
    @brief  1 つの EXE をクエリモードで起動してメタデータを収集する。

    @param  exe_path プラグイン EXE のフルパス
    @param  verbose  詳細ログを stderr に出すかどうか
    @return 収集済み PluginMetadata（失敗時は meta.error に理由）
    """
    meta = PluginMetadata(exe_path=exe_path)

    try:
        pp = spawn_plugin(exe_path)
    except Exception as exc:
        meta.error = f"spawn failed: {exc}"
        return meta

    def log(msg: str) -> None:
        if verbose:
            sys.stderr.write(
                f"[scan] {pathlib.Path(exe_path).name}: {msg}\n"
            )

    try:
        # メッセージループ。タイムアウトはプロセス単位で外から見張る。
        # 簡易的に、次の 4 バイト type が読めなければ EOF として終了。
        while True:
            try:
                msg_type = read_uint32(pp.read_from_plugin)
            except WireError:
                # 正常 EOF（プラグインがクエリ完了して exit した）
                break

            if msg_type == GP_PROC_INSTALL:
                read_proc_install(pp.read_from_plugin, meta)
                log(f"GP_PROC_INSTALL name={meta.procedure}")
            elif msg_type == GP_PROC_RUN:
                called = read_proc_run(pp.read_from_plugin, meta)
                log(f"GP_PROC_RUN {called}")
                # どんな呼び出しにも成功で返す（PoC の割り切り）
                write_proc_return(pp.write_to_plugin, called)
            elif msg_type == GP_HAS_INIT:
                log("GP_HAS_INIT")
                read_generic_message(pp.read_from_plugin, msg_type)
            elif msg_type == GP_PROC_UNINSTALL:
                read_generic_message(pp.read_from_plugin, msg_type)
            elif msg_type == GP_QUIT:
                log("GP_QUIT (plugin-initiated)")
                break
            else:
                # 予期しない型は読み飛ばせないのでここで打ち切り
                log(f"unexpected msg_type={msg_type}, bailing out")
                raise WireError(
                    f"unexpected msg_type {msg_type} during query"
                )
    except WireError as e:
        meta.error = f"wire error: {e}"
    except Exception as e:
        meta.error = f"query failed: {e}\n{traceback.format_exc()}"
    finally:
        try:
            pp.read_from_plugin.close()
        except Exception:
            pass
        try:
            pp.write_to_plugin.close()
        except Exception:
            pass
        try:
            pp.process.wait(timeout=SCAN_TIMEOUT_SEC)
        except subprocess.TimeoutExpired:
            pp.process.kill()
            pp.process.wait(timeout=2.0)
            if meta.error is None:
                meta.error = "plugin did not exit within timeout"

        # stderr を拾ってログに回す（失敗時のみ）
        if meta.error and pp.process.stderr:
            try:
                stderr_bytes = pp.process.stderr.read() or b""
                tail = stderr_bytes.decode("utf-8", errors="replace").strip()
                if tail:
                    meta.error += f"\nstderr: {tail[:1024]}"
            except Exception:
                pass

    return meta


def enumerate_exes(search_paths: list[str]) -> list[str]:
    """
    @brief  検索パス配下の EXE を列挙する。

    Windows では `*.exe` のみ。他プラットフォームでは実行可能ファイル全般。
    """
    found: list[str] = []
    seen: set[str] = set()
    for p in search_paths:
        root = pathlib.Path(p)
        if not root.exists() or not root.is_dir():
            continue
        if sys.platform.startswith("win"):
            patterns = ("*.exe",)
        else:
            patterns = ("*",)
        for pattern in patterns:
            for entry in root.rglob(pattern):
                if not entry.is_file():
                    continue
                abs_path = str(entry.resolve())
                if abs_path in seen:
                    continue
                # POSIX では実行可能判定
                if not sys.platform.startswith("win"):
                    if not os.access(abs_path, os.X_OK):
                        continue
                seen.add(abs_path)
                found.append(abs_path)
    return found


# ---------------------------------------------------------------------------
# 設定ファイル読み込み
# ---------------------------------------------------------------------------


def _expand_placeholders(path: str) -> str:
    """
    @brief  bridge_config.json のパスプレースホルダーを展開する。

    Windows: `%FOO%` を環境変数で展開（os.path.expandvars）
    Mac/Linux: `{HOME}` を `~` 展開で置換
    """
    expanded = os.path.expandvars(path)
    if "{HOME}" in expanded:
        expanded = expanded.replace("{HOME}", os.path.expanduser("~"))
    return expanded


def load_search_paths(config_path: str) -> list[str]:
    """
    @brief  bridge_config.json から plugin_search_paths を取得。

    ファイル不在時は OS 別のデフォルトパスを返す。
    """
    platform_key = "mac" if sys.platform == "darwin" else "windows"

    defaults_map = {
        "windows": [r"C:\Program Files\GIMP 3\lib\gimp\3.0\plug-ins"],
        "mac": [
            "/Applications/GIMP.app/Contents/Resources/lib/gimp/3.0/plug-ins"
        ],
    }

    try:
        with open(config_path, encoding="utf-8") as f:
            cfg = json.load(f)
        raw_paths = cfg.get(platform_key, {}).get("plugin_search_paths", [])
        if not raw_paths:
            raw_paths = defaults_map[platform_key]
    except FileNotFoundError:
        sys.stderr.write(
            f"[scan] config not found, using defaults: {config_path}\n"
        )
        raw_paths = defaults_map[platform_key]
    except Exception as e:
        sys.stderr.write(f"[scan] config parse error: {e}\n")
        raw_paths = defaults_map[platform_key]

    return [_expand_placeholders(p) for p in raw_paths]


# ---------------------------------------------------------------------------
# GUI
# ---------------------------------------------------------------------------


def select_plugins_gui(
    candidates: list[PluginMetadata],
) -> Optional[list[PluginMetadata]]:
    """
    @brief  tkinter チェックリストで選択 UI を出す。

    @return 選択された PluginMetadata のリスト。キャンセル時は None
    """
    if not candidates:
        # tkinter を使ってメッセージだけ出す
        root = tk.Tk()
        root.withdraw()
        messagebox.showinfo(
            "CSPBridgeGimp Scanner",
            "No valid GIMP plugins were found in the search paths."
        )
        root.destroy()
        return None

    state = {"result": None}  # type: dict[str, Optional[list[PluginMetadata]]]

    root = tk.Tk()
    root.title("CSPBridgeGimp Plugin Scanner")
    root.geometry("960x560")

    header = ttk.Label(
        root,
        text=(
            f"Found {len(candidates)} GIMP plugin(s). "
            "Check the ones to include in plugins.json."
        ),
    )
    header.pack(anchor="w", padx=8, pady=(8, 4))

    # スクロール可能な領域
    container = ttk.Frame(root)
    container.pack(fill="both", expand=True, padx=8, pady=4)

    canvas = tk.Canvas(container, highlightthickness=0)
    scrollbar = ttk.Scrollbar(
        container, orient="vertical", command=canvas.yview
    )
    inner = ttk.Frame(canvas)

    inner.bind(
        "<Configure>",
        lambda e: canvas.configure(scrollregion=canvas.bbox("all")),
    )
    canvas.create_window((0, 0), window=inner, anchor="nw")
    canvas.configure(yscrollcommand=scrollbar.set)

    canvas.pack(side="left", fill="both", expand=True)
    scrollbar.pack(side="right", fill="y")

    # カラムヘッダー
    header_frame = ttk.Frame(inner)
    header_frame.grid(row=0, column=0, sticky="ew", padx=2, pady=(0, 4))
    ttk.Label(header_frame, text="", width=3).grid(row=0, column=0)
    ttk.Label(
        header_frame, text="Procedure", width=28, anchor="w"
    ).grid(row=0, column=1, sticky="w")
    ttk.Label(
        header_frame, text="Menu", width=36, anchor="w"
    ).grid(row=0, column=2, sticky="w")
    ttk.Label(
        header_frame, text="Blurb", width=40, anchor="w"
    ).grid(row=0, column=3, sticky="w")
    ttk.Label(
        header_frame, text="EXE", width=40, anchor="w"
    ).grid(row=0, column=4, sticky="w")

    check_vars: list[tk.BooleanVar] = []
    for i, meta in enumerate(candidates):
        var = tk.BooleanVar(value=False)
        check_vars.append(var)
        row_frame = ttk.Frame(inner)
        row_frame.grid(row=i + 1, column=0, sticky="ew", padx=2, pady=1)

        cb = ttk.Checkbutton(row_frame, variable=var)
        cb.grid(row=0, column=0, sticky="w")
        ttk.Label(
            row_frame, text=meta.procedure, width=28, anchor="w"
        ).grid(row=0, column=1, sticky="w")
        ttk.Label(
            row_frame, text=meta.display_menu or "-",
            width=36, anchor="w",
        ).grid(row=0, column=2, sticky="w")
        ttk.Label(
            row_frame, text=(meta.blurb or "-")[:80],
            width=40, anchor="w",
        ).grid(row=0, column=3, sticky="w")
        ttk.Label(
            row_frame, text=pathlib.Path(meta.exe_path).name,
            width=40, anchor="w",
        ).grid(row=0, column=4, sticky="w")

    # ボタン
    btn_frame = ttk.Frame(root)
    btn_frame.pack(fill="x", padx=8, pady=8)

    def on_save() -> None:
        selected = [
            c for c, v in zip(candidates, check_vars) if v.get()
        ]
        if not selected:
            if not messagebox.askyesno(
                "Confirm",
                "No plugins selected. Save an empty plugins.json?",
            ):
                return
        state["result"] = selected
        root.destroy()

    def on_cancel() -> None:
        state["result"] = None
        root.destroy()

    def on_select_all() -> None:
        for v in check_vars:
            v.set(True)

    def on_select_none() -> None:
        for v in check_vars:
            v.set(False)

    ttk.Button(
        btn_frame, text="Select All", command=on_select_all
    ).pack(side="left")
    ttk.Button(
        btn_frame, text="Select None", command=on_select_none
    ).pack(side="left", padx=(4, 0))
    ttk.Button(
        btn_frame, text="Cancel", command=on_cancel
    ).pack(side="right")
    ttk.Button(
        btn_frame, text="Save plugins.json", command=on_save
    ).pack(side="right", padx=(0, 4))

    root.protocol("WM_DELETE_WINDOW", on_cancel)
    root.mainloop()

    return state["result"]


# ---------------------------------------------------------------------------
# plugins.json 書き出し
# ---------------------------------------------------------------------------


def build_plugin_entry(meta: PluginMetadata) -> dict[str, Any]:
    """
    @brief  PluginMetadata を plugins.json のエントリ dict に変換。
    """
    menu = meta.display_menu or meta.menu_label or ""
    params_out = [
        {
            "type": PARAM_TYPE_NAMES.get(
                _gp_param_type_from_def(p), "DEFAULT"
            ),
            "name": p.name,
        }
        for p in meta.params
    ]
    return {
        "id": meta.suggested_id,
        "exe": pathlib.Path(meta.exe_path).name,
        "procedure": meta.procedure,
        "menu": menu,
        "blurb": meta.blurb or "",
        "params": params_out,
    }


def _gp_param_type_from_def(p: ParamDef) -> int:
    """
    @brief  GPParamDef（宣言型）から概ね相当する GPParamType を推定する。

    GIMP 3.0 では param_def は `GPParamDefType`、実ランタイムは `GPParamType`
    と別 enum だが、plugins.json の params[].type は人間が読む用途なので
    簡易マップで十分。
    """
    mapping = {
        GP_PARAM_DEF_TYPE_INT: GP_PARAM_TYPE_INT,
        GP_PARAM_DEF_TYPE_ENUM: GP_PARAM_TYPE_INT,
        GP_PARAM_DEF_TYPE_BOOLEAN: GP_PARAM_TYPE_INT,
        GP_PARAM_DEF_TYPE_UNIT: GP_PARAM_TYPE_INT,
        GP_PARAM_DEF_TYPE_ID: GP_PARAM_TYPE_INT,
        GP_PARAM_DEF_TYPE_DOUBLE: GP_PARAM_TYPE_DOUBLE,
        GP_PARAM_DEF_TYPE_STRING: GP_PARAM_TYPE_STRING,
        GP_PARAM_DEF_TYPE_CHOICE: GP_PARAM_TYPE_STRING,
        GP_PARAM_DEF_TYPE_GEGL_COLOR: GP_PARAM_TYPE_GEGL_COLOR,
        GP_PARAM_DEF_TYPE_ID_ARRAY: GP_PARAM_TYPE_ID_ARRAY,
        GP_PARAM_DEF_TYPE_FILE: GP_PARAM_TYPE_FILE,
        GP_PARAM_DEF_TYPE_RESOURCE: GP_PARAM_TYPE_INT,
        GP_PARAM_DEF_TYPE_EXPORT_OPTIONS: GP_PARAM_TYPE_EXPORT_OPTIONS,
    }
    return mapping.get(p.param_def_type, GP_PARAM_TYPE_INT)


def write_plugins_json(
    output_path: str, selected: list[PluginMetadata]
) -> None:
    """@brief  plugins.json を書き出す（UTF-8、2 スペースインデント）。"""
    entries = [build_plugin_entry(m) for m in selected]
    with open(output_path, "w", encoding="utf-8", newline="\n") as f:
        json.dump(entries, f, ensure_ascii=False, indent=2)
        f.write("\n")


# ---------------------------------------------------------------------------
# エントリポイント
# ---------------------------------------------------------------------------


def main() -> int:
    """@brief  CLI エントリ。成功時 0、異常系でも非ゼロ以外は返さず GUI で通知。"""
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    default_config = repo_root / "config" / "bridge_config.json"
    default_output = repo_root / "plugins.json"

    parser = argparse.ArgumentParser(
        description=(
            "Scan GIMP plugin EXEs via the Wire Protocol query phase, "
            "then write plugins.json. Run this before `meson setup`."
        )
    )
    parser.add_argument(
        "--config",
        default=str(default_config),
        help="Path to bridge_config.json (default: %(default)s)",
    )
    parser.add_argument(
        "--output",
        default=str(default_output),
        help="Path to write plugins.json (default: %(default)s)",
    )
    parser.add_argument(
        "--no-gui",
        action="store_true",
        help=(
            "Skip the selection GUI and auto-select every valid plugin. "
            "Useful for headless CI / debugging."
        ),
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Verbose protocol trace to stderr.",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Scan and print results without writing plugins.json.",
    )

    args = parser.parse_args()

    print(f"[scan] platform={sys.platform} ({platform.platform()})",
          file=sys.stderr)
    print(f"[scan] config={args.config}", file=sys.stderr)

    search_paths = load_search_paths(args.config)
    print(
        f"[scan] search_paths={search_paths}", file=sys.stderr
    )

    exes = enumerate_exes(search_paths)
    print(f"[scan] found {len(exes)} candidate executables",
          file=sys.stderr)

    candidates: list[PluginMetadata] = []
    for exe in exes:
        print(f"[scan] querying {exe}", file=sys.stderr)
        meta = query_plugin(exe, verbose=args.verbose)
        if meta.is_valid:
            candidates.append(meta)
            print(
                f"[scan]   -> procedure={meta.procedure}, "
                f"menu={meta.display_menu!r}",
                file=sys.stderr,
            )
        else:
            print(
                f"[scan]   -> skipped: {meta.error}",
                file=sys.stderr,
            )

    if args.dry_run:
        print(
            json.dumps(
                [build_plugin_entry(m) for m in candidates],
                ensure_ascii=False,
                indent=2,
            )
        )
        return 0

    if args.no_gui:
        selected = candidates
    else:
        selected = select_plugins_gui(candidates)
        if selected is None:
            print("[scan] cancelled by user", file=sys.stderr)
            return 1

    write_plugins_json(args.output, selected)
    print(
        f"[scan] wrote {len(selected)} plugin(s) to {args.output}",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
