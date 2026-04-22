"""
@file   test_wire.py
@brief  scan_and_select.py の Wire Protocol I/O 部分のユニットテスト
@author CSPBridgeGimp
@date   2026-04-22

GIMP 3 実機が検証環境にないため、pack / unpack の正当性を
オフラインで担保するためのテスト群。標準ライブラリ (`unittest`
+ `io.BytesIO`) のみで動き、pytest 等の外部依存は使わない。

実行方法:
    python -m unittest tools.scanner.test_wire
    または
    python tools/scanner/test_wire.py

C++ 側のテスト (Catch2 / `meson test`) とは別系統。
詳細は docs/spec_test.md §2 を参照。
"""

from __future__ import annotations

import io
import pathlib
import struct
import sys
import unittest

# scan_and_select.py をモジュールとして取り込む
sys.path.insert(0, str(pathlib.Path(__file__).parent))

from scan_and_select import (  # noqa: E402
    GIMP_PDB_SUCCESS,
    GP_PARAM_DEF_TYPE_DEFAULT,
    GP_PARAM_DEF_TYPE_INT,
    GP_PARAM_TYPE_INT,
    GP_PROC_INSTALL,
    GP_PROC_RETURN,
    ParamDef,
    PluginMetadata,
    read_param_def,
    read_params,
    read_proc_install,
    read_proc_run,
    read_string,
    read_uint32,
    write_proc_return,
    write_string,
    write_uint32,
)


# ---------------------------------------------------------------------------
# 共通ヘルパ
# ---------------------------------------------------------------------------


def _pack_uint32(value: int) -> bytes:
    """big-endian uint32 を bytes で返す。"""
    return struct.pack(">I", value & 0xFFFFFFFF)


def _pack_int32(value: int) -> bytes:
    """big-endian int32 を bytes で返す。"""
    return struct.pack(">i", value)


def _pack_int64(value: int) -> bytes:
    """big-endian int64 を bytes で返す。"""
    return struct.pack(">q", value)


def _pack_string(s):
    """
    @brief  Wire Protocol 文字列を bytes にパックするヘルパ。

    `s is None` → `uint32 0`
    `s == ""`   → `uint32 1` + `b"\\x00"`
    それ以外     → `uint32 len(utf8 + NUL)` + `utf8 + NUL`
    """
    if s is None:
        return _pack_uint32(0)
    encoded = s.encode("utf-8") + b"\x00"
    return _pack_uint32(len(encoded)) + encoded


# ---------------------------------------------------------------------------
# uint32 / string プリミティブ
# ---------------------------------------------------------------------------


class TestUint32BigEndian(unittest.TestCase):
    """uint32 が big-endian で読み書きされることを明示検証する。"""

    def test_uint32_big_endian_bytes(self) -> None:
        stream = io.BytesIO()
        write_uint32(stream, 0x11223344)
        # big-endian 固定であることを生バイトで検証
        self.assertEqual(stream.getvalue(), b"\x11\x22\x33\x44")

    def test_uint32_roundtrip(self) -> None:
        stream = io.BytesIO()
        for v in (0, 1, 0xFF, 0x100, 0x11223344, 0xFFFFFFFF):
            stream.seek(0)
            stream.truncate(0)
            write_uint32(stream, v)
            stream.seek(0)
            self.assertEqual(read_uint32(stream), v)


class TestStringRoundtrip(unittest.TestCase):
    """Wire Protocol 文字列（length + NUL 終端）の往復動作を検証。"""

    def test_ascii_roundtrip(self) -> None:
        stream = io.BytesIO()
        write_string(stream, "abc")
        # length フィールドは NUL 込み = 4, payload = b"abc\x00"
        self.assertEqual(
            stream.getvalue(),
            _pack_uint32(4) + b"abc\x00",
        )
        stream.seek(0)
        self.assertEqual(read_string(stream), "abc")

    def test_utf8_japanese_roundtrip(self) -> None:
        stream = io.BytesIO()
        write_string(stream, "日本語")
        stream.seek(0)
        self.assertEqual(read_string(stream), "日本語")

        # length フィールドは UTF-8 バイト数 + 1 (NUL)
        stream.seek(0)
        length = read_uint32(stream)
        expected_len = len("日本語".encode("utf-8")) + 1
        self.assertEqual(length, expected_len)

    def test_string_null(self) -> None:
        """None → length=0、read_string は None を返す。"""
        stream = io.BytesIO()
        write_string(stream, None)
        self.assertEqual(stream.getvalue(), _pack_uint32(0))
        stream.seek(0)
        self.assertIsNone(read_string(stream))

    def test_string_empty(self) -> None:
        """"" → length=1, payload=b"\\x00"、read は "" を返す。"""
        stream = io.BytesIO()
        write_string(stream, "")
        self.assertEqual(stream.getvalue(), _pack_uint32(1) + b"\x00")
        stream.seek(0)
        self.assertEqual(read_string(stream), "")


# ---------------------------------------------------------------------------
# ParamDef
# ---------------------------------------------------------------------------


class TestParamDef(unittest.TestCase):
    """GPParamDef 読み取りの境界ケース。"""

    def _build_param_def_header(
        self,
        param_def_type: int,
        type_name: str,
        value_type_name: str,
        name: str,
        nick: str,
        blurb: str,
        flags: int,
    ) -> bytes:
        """ParamDef の共通ヘッダ部分（メタデータ直前まで）を組み立てる。"""
        return (
            _pack_uint32(param_def_type)
            + _pack_string(type_name)
            + _pack_string(value_type_name)
            + _pack_string(name)
            + _pack_string(nick)
            + _pack_string(blurb)
            + _pack_uint32(flags)
        )

    def test_param_def_int_roundtrip(self) -> None:
        """GP_PARAM_DEF_TYPE_INT: min/max/default の int64 x3 を消費。"""
        header = self._build_param_def_header(
            param_def_type=GP_PARAM_DEF_TYPE_INT,
            type_name="GParamInt",
            value_type_name="gint",
            name="radius",
            nick="Radius",
            blurb="Blur radius",
            flags=0,
        )
        meta_bytes = (
            _pack_int64(-100)   # min
            + _pack_int64(100)  # max
            + _pack_int64(5)    # default
        )
        # 末尾にセンチネルを入れ、余分に消費していないことを検証
        sentinel = b"\xDE\xAD\xBE\xEF"
        stream = io.BytesIO(header + meta_bytes + sentinel)

        pd = read_param_def(stream)
        self.assertEqual(pd.param_def_type, GP_PARAM_DEF_TYPE_INT)
        self.assertEqual(pd.type_name, "GParamInt")
        self.assertEqual(pd.value_type_name, "gint")
        self.assertEqual(pd.name, "radius")
        self.assertEqual(pd.nick, "Radius")
        self.assertEqual(pd.blurb, "Blur radius")
        self.assertEqual(pd.flags, 0)
        # 残バイトはセンチネルのみ
        self.assertEqual(stream.read(), sentinel)

    def test_param_def_default_no_meta(self) -> None:
        """GP_PARAM_DEF_TYPE_DEFAULT: meta 追加バイト無し。"""
        header = self._build_param_def_header(
            param_def_type=GP_PARAM_DEF_TYPE_DEFAULT,
            type_name="GParamBoxed",
            value_type_name="gpointer",
            name="opaque",
            nick="Opaque",
            blurb="",
            flags=0,
        )
        sentinel = b"\xCA\xFE\xBA\xBE"
        stream = io.BytesIO(header + sentinel)

        pd = read_param_def(stream)
        self.assertEqual(pd.param_def_type, GP_PARAM_DEF_TYPE_DEFAULT)
        self.assertEqual(pd.name, "opaque")
        # meta は 0 バイトなので、直後にセンチネルが残っているはず
        self.assertEqual(stream.read(), sentinel)


# ---------------------------------------------------------------------------
# GP_PROC_INSTALL
# ---------------------------------------------------------------------------


class TestProcInstall(unittest.TestCase):
    """GP_PROC_INSTALL を手組みバイト列から食わせて PluginMetadata に反映。"""

    def _build_int_param_def(self, name: str) -> bytes:
        return (
            _pack_uint32(GP_PARAM_DEF_TYPE_INT)
            + _pack_string("GParamInt")
            + _pack_string("gint")
            + _pack_string(name)
            + _pack_string(name.capitalize())
            + _pack_string("")     # blurb
            + _pack_uint32(0)      # flags
            + _pack_int64(0)       # min
            + _pack_int64(100)     # max
            + _pack_int64(0)       # default
        )

    def test_proc_install_roundtrip(self) -> None:
        """plug-in-gauss 相当: 6 個の INT ParamDef、return_vals なし。"""
        proc_name = "plug-in-gauss"
        n_params = 6
        n_return_vals = 0

        payload = (
            _pack_string(proc_name)
            + _pack_uint32(1)             # proc_type (適当な値)
            + _pack_uint32(n_params)
            + _pack_uint32(n_return_vals)
        )
        for i in range(n_params):
            payload += self._build_int_param_def(f"arg{i}")

        stream = io.BytesIO(payload)
        meta = PluginMetadata(exe_path="dummy.exe")
        read_proc_install(stream, meta)

        self.assertEqual(meta.procedure, proc_name)
        self.assertEqual(meta.proc_type, 1)
        self.assertEqual(len(meta.params), n_params)
        self.assertEqual(len(meta.return_vals), 0)
        for i, p in enumerate(meta.params):
            self.assertEqual(p.param_def_type, GP_PARAM_DEF_TYPE_INT)
            self.assertEqual(p.name, f"arg{i}")
            self.assertEqual(p.type_name, "GParamInt")
        # ストリームを完全に消費している
        self.assertEqual(stream.read(), b"")


# ---------------------------------------------------------------------------
# GP_PROC_RETURN (write)
# ---------------------------------------------------------------------------


class TestProcReturnWrite(unittest.TestCase):
    """write_proc_return が吐くバイト列の生仕様検証。"""

    def test_proc_return_write_format(self) -> None:
        stream = io.BytesIO()
        write_proc_return(stream, "foo")

        expected = (
            _pack_uint32(GP_PROC_RETURN)             # = 6
            + _pack_uint32(4) + b"foo\x00"           # string "foo"
            + _pack_uint32(1)                        # n_params = 1
            + _pack_uint32(GP_PARAM_TYPE_INT)        # param_type = INT
            # type_name "GimpPDBStatusType"
            + _pack_string("GimpPDBStatusType")
            + _pack_int32(GIMP_PDB_SUCCESS)          # status = 0
        )
        self.assertEqual(stream.getvalue(), expected)


# ---------------------------------------------------------------------------
# GP_PROC_RUN PDB intercept
# ---------------------------------------------------------------------------


class TestProcRunIntercept(unittest.TestCase):
    """GP_PROC_RUN で傍受すべき PDB 手続き呼び出しの挙動を検証。"""

    def _build_string_param(self, value) -> bytes:
        """GP_PARAM_TYPE_STRING の GPParam を組み立てる。"""
        from scan_and_select import GP_PARAM_TYPE_STRING
        return (
            _pack_uint32(GP_PARAM_TYPE_STRING)
            + _pack_string("gchararray")   # type_name (使われない)
            + _pack_string(value)          # 実データ
        )

    def _build_proc_run_payload(
        self, proc_name: str, string_args: list
    ) -> bytes:
        payload = _pack_string(proc_name)
        payload += _pack_uint32(len(string_args))  # n_params
        for a in string_args:
            payload += self._build_string_param(a)
        return payload

    def test_proc_run_intercept_menu_label(self) -> None:
        """gimp-pdb-set-proc-menu-label を読んで meta.menu_label に反映。"""
        payload = self._build_proc_run_payload(
            "gimp-pdb-set-proc-menu-label",
            ["plug-in-gauss", "_Gaussian Blur..."],
        )
        stream = io.BytesIO(payload)
        meta = PluginMetadata(
            exe_path="dummy.exe",
            procedure="plug-in-gauss",
        )
        called = read_proc_run(stream, meta)

        self.assertEqual(called, "gimp-pdb-set-proc-menu-label")
        self.assertEqual(meta.menu_label, "_Gaussian Blur...")
        # 全バイト消費
        self.assertEqual(stream.read(), b"")

    def test_proc_run_intercept_attribution(self) -> None:
        """gimp-pdb-set-proc-attribution で authors / copyright / date を取得。"""
        payload = self._build_proc_run_payload(
            "gimp-pdb-set-proc-attribution",
            [
                "plug-in-gauss",
                "Spencer Kimball & Peter Mattis",
                "Spencer Kimball & Peter Mattis",
                "1995-1996",
            ],
        )
        stream = io.BytesIO(payload)
        meta = PluginMetadata(
            exe_path="dummy.exe",
            procedure="plug-in-gauss",
        )
        called = read_proc_run(stream, meta)

        self.assertEqual(called, "gimp-pdb-set-proc-attribution")
        self.assertEqual(meta.authors, "Spencer Kimball & Peter Mattis")
        self.assertEqual(meta.copyright, "Spencer Kimball & Peter Mattis")
        self.assertEqual(meta.date, "1995-1996")
        self.assertEqual(stream.read(), b"")

    def test_proc_run_intercept_menu_path(self) -> None:
        """gimp-pdb-add-proc-menu-path で menu_paths に追記。"""
        payload = self._build_proc_run_payload(
            "gimp-pdb-add-proc-menu-path",
            ["plug-in-gauss", "<Image>/Filters/Blur"],
        )
        stream = io.BytesIO(payload)
        meta = PluginMetadata(
            exe_path="dummy.exe",
            procedure="plug-in-gauss",
        )
        read_proc_run(stream, meta)

        self.assertEqual(meta.menu_paths, ["<Image>/Filters/Blur"])


# ---------------------------------------------------------------------------
# エントリポイント
# ---------------------------------------------------------------------------


if __name__ == "__main__":
    unittest.main(verbosity=2)
