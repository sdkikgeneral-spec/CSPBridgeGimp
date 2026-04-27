project('CSPBridgeGimp', 'cpp',
    version: '0.1.0',
    default_options: [
        'warning_level=3',
    ]
)

# ----------------------------------------------------------------------------
# プラットフォーム・コンパイラ検出
# ----------------------------------------------------------------------------
host_os  = host_machine.system()     # 'windows' or 'darwin'
cpu      = host_machine.cpu_family() # 'x86_64' or 'aarch64'
compiler = meson.get_compiler('cpp')
is_msvc  = compiler.get_id() == 'msvc' or compiler.get_id() == 'clang-cl'
fs       = import('fs')

# C++ 標準: MSVC は /std:c++latest (= C++23), GCC/Clang は -std=c++23
if is_msvc
    add_project_arguments('/std:c++latest', language: 'cpp')
else
    add_project_arguments('-std=c++23', language: 'cpp')
endif

# ----------------------------------------------------------------------------
# プラットフォーム別コンパイルフラグ・定義
# ----------------------------------------------------------------------------
platform_args = []
platform_link_args = []
platform_deps = []

if host_os == 'windows'
    if is_msvc
        platform_args += [
            '/DUNICODE',
            '/D_UNICODE',
            '/DWIN32_LEAN_AND_MEAN',
            '/DNOMINMAX',
        ]
    else  # MinGW / Clang
        platform_args += [
            '-DUNICODE',
            '-D_UNICODE',
            '-DWIN32_LEAN_AND_MEAN',
            '-DNOMINMAX',
        ]
    endif
    # Boost.Process / ネットワーク系が必要とする Windows システムライブラリ
    platform_deps += [
        compiler.find_library('ws2_32',  required: true),
        compiler.find_library('mswsock', required: true),
    ]
elif host_os == 'darwin'
    platform_args += [
        '-DTARGET_OS_MAC',
    ]
    # macOS フレームワーク（必要に応じて追加）
    platform_link_args += [
        '-framework', 'CoreFoundation',
    ]
endif

# ----------------------------------------------------------------------------
# SIMD フラグ
# ----------------------------------------------------------------------------
if cpu == 'x86_64'
    if is_msvc
        platform_args += ['/arch:AVX2']
    else
        platform_args += ['-mavx2', '-msse4.1']
    endif
elif cpu == 'aarch64'
    platform_args += ['-march=armv8-a+simd']
endif

add_project_arguments(platform_args, language: 'cpp')
add_project_link_arguments(platform_link_args, language: 'cpp')

# ----------------------------------------------------------------------------
# config/bridge_config.json から csp_plugin_output_dir を取得
# ----------------------------------------------------------------------------
py            = import('python').find_installation('python3', required: true)
config_reader = files('scripts/read_config.py')

csp_output_dir = run_command(
    py, config_reader,
    '--config',   meson.project_source_root() / 'config' / 'bridge_config.json',
    '--platform', host_os,
    '--key',      'csp_plugin_output_dir',
    check: true,
).stdout().strip()

# ----------------------------------------------------------------------------
# 依存ライブラリ
#
# 方針（PoC 現フェーズ）:
#   - nlohmann_json / catch2 は WrapDB の wrap で取得（subprojects/*.wrap）
#   - Boost は system インストール（MSYS2 / vcpkg / Homebrew 等）を期待し、
#     pkg-config / cmake 経由で引く。見つからない場合は setup を止めず warning
#     のみ。Boost.Process を使うモジュールは src/ 実装フェーズで必須化する
#   - GIMP (libgimpwire / libgimpbase) はシステムインストール済み GIMP 3 の
#     pkg-config ファイルから引く。見つからない場合は warning で先に進める。
#     subprojects/gimp.wrap は将来のフル wrap 化用 placeholder（詳細はファイル
#     冒頭コメント参照）
# ----------------------------------------------------------------------------
nlohmann_json_dep = dependency('nlohmann_json', fallback: ['nlohmann_json', 'nlohmann_json_dep'])
catch2_dep        = dependency('catch2-with-main', fallback: ['catch2', 'catch2_with_main_dep'])

boost_dep = dependency('boost',
    modules:  ['filesystem', 'system'],
    required: false,
)
if not boost_dep.found()
    warning('Boost (filesystem, system) が見つかりません。' +
            'Boost.Process を利用する src/ipc 実装フェーズまでに ' +
            'system インストール (MSYS2 / vcpkg / Homebrew 等) してください。')
endif

libgimpwire_dep = dependency('gimp-wire-3.0', required: false)
if not libgimpwire_dep.found()
    libgimpwire_dep = dependency('libgimpwire-3.0', required: false)
endif
if not libgimpwire_dep.found()
    warning('libgimpwire (gimp-wire-3.0 / libgimpwire-3.0) が pkg-config から ' +
            '見つかりません。GIMP 3 をシステムインストールするか、将来的に ' +
            'subprojects/gimp.wrap を有効化してください。')
endif

libgimpbase_dep = dependency('gimp-base-3.0', required: false)
if not libgimpbase_dep.found()
    libgimpbase_dep = dependency('libgimpbase-3.0', required: false)
endif
if not libgimpbase_dep.found()
    warning('libgimpbase (gimp-base-3.0 / libgimpbase-3.0) が pkg-config から ' +
            '見つかりません。GIMP 3 をシステムインストールするか、将来的に ' +
            'subprojects/gimp.wrap を有効化してください。')
endif

common_deps = [
    nlohmann_json_dep,
]
if boost_dep.found()
    common_deps += boost_dep
endif
if libgimpwire_dep.found()
    common_deps += libgimpwire_dep
endif
if libgimpbase_dep.found()
    common_deps += libgimpbase_dep
endif
common_deps += platform_deps

# ----------------------------------------------------------------------------
# コアソース（全 DLL およびテストで共有するスタティックライブラリ）
#
# フェーズ 1 は段階的に実装するため、まだ存在しないファイルは fs.exists() で
# スキップする。実装が追加されるたびに自動的にビルド対象に加わる。
# ----------------------------------------------------------------------------
all_src_candidates = [
    'src/config/config.cpp',
    'src/host/host.cpp',
    'src/host/pdb_stubs.cpp',
    'src/host/tile_transfer.cpp',
    'src/ipc/process.cpp',
    'src/ipc/wire_io.cpp',
    'src/csp/plugin_entry.cpp',
    'src/csp/buffer.cpp',
]
srcs = []
foreach f : all_src_candidates
    if fs.exists(f)
        srcs += files(f)
    endif
endforeach

core_lib = static_library('CSPBridgeGimpCore', srcs,
    dependencies: common_deps,
)

# ----------------------------------------------------------------------------
# plugins.json から GIMP プラグイン id 一覧を取得し、DLL を生成
#
# 事前準備: meson setup の前に以下を実行して plugins.json を生成すること。
#   python tools/scanner/scan_and_select.py
#
# plugins.json が存在しない場合は警告を表示してスキップする。
# ----------------------------------------------------------------------------
plugins_json = meson.project_source_root() / 'plugins.json'

scan_result = run_command(
    py, meson.project_source_root() / 'scripts' / 'list_plugin_ids.py',
    plugins_json,
    check: false,
)

bridge_libs = []

if scan_result.returncode() != 0
    warning('plugins.json が見つからないか不正です。' +
            'tools/scanner/scan_and_select.py を実行してから meson setup を再実行してください。')
else
    raw_ids = scan_result.stdout().strip()
    if raw_ids != ''
        plugin_ids = raw_ids.split('\n')
        foreach plugin_id : plugin_ids
            if plugin_id != ''
                if is_msvc
                    plugin_macro_arg = '/DGIMP_PLUGIN_ID=\"' + plugin_id + '\"'
                else
                    plugin_macro_arg = '-DGIMP_PLUGIN_ID="' + plugin_id + '"'
                endif
                lib = shared_library(
                    'CSPBridgeGimp_' + plugin_id,
                    [],
                    cpp_args:     [plugin_macro_arg],
                    dependencies: common_deps,
                    link_with:    core_lib,
                    install:      true,
                    install_dir:  csp_output_dir,
                )
                bridge_libs += lib
            endif
        endforeach
    else
        warning('plugins.json にエントリがありません。' +
                'tools/scanner/scan_and_select.py でプラグインを選択してください。')
    endif
endif

# ----------------------------------------------------------------------------
# テスト（実装済みのテストファイルのみ順次追加）
# ----------------------------------------------------------------------------
all_test_candidates = [
    'tests/test_config.cpp',
    'tests/test_tile_transfer.cpp',
    'tests/test_buffer.cpp',
    'tests/test_wire_io.cpp',
    'tests/test_concurrency.cpp',
]
test_srcs = []
foreach f : all_test_candidates
    if fs.exists(f)
        test_srcs += files(f)
    endif
endforeach

if test_srcs.length() > 0
    test_exe = executable('cspbridge_tests', test_srcs,
        dependencies: [catch2_dep, nlohmann_json_dep, boost_dep] + platform_deps,
        link_with:    core_lib,
    )
    test('unit tests', test_exe)
else
    warning('テストファイルが見つかりません。tests/ 配下のファイルを実装してください。')
endif
