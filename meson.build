project('CSPBridgeGimp', 'cpp',
    version: '0.1.0',
    default_options: [
        'cpp_std=c++23',
        'warning_level=3',
    ]
)

# ----------------------------------------------------------------------------
# プラットフォーム・コンパイラ検出
# ----------------------------------------------------------------------------
host_os  = host_machine.system()     # 'windows' or 'darwin'
cpu      = host_machine.cpu_family() # 'x86_64' or 'aarch64'
compiler = meson.get_compiler('cpp')

# ----------------------------------------------------------------------------
# プラットフォーム別コンパイルフラグ・定義
# ----------------------------------------------------------------------------
platform_args = []
platform_link_args = []
platform_deps = []

if host_os == 'windows'
    platform_args += [
        '/DUNICODE',
        '/D_UNICODE',
        '/DWIN32_LEAN_AND_MEAN',
        '/DNOMINMAX',
    ]
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
    if compiler.get_id() == 'msvc'
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
    '--config',   meson.source_root() / 'config' / 'bridge_config.json',
    '--platform', host_os,
    '--key',      'csp_plugin_output_dir',
    check: true,
).stdout().strip()

# ----------------------------------------------------------------------------
# 依存ライブラリ
# ----------------------------------------------------------------------------
libgimpwire_dep   = dependency('libgimpwire',   fallback: ['gimp',          'libgimpwire_dep'])
libgimpbase_dep   = dependency('libgimpbase',   fallback: ['gimp',          'libgimpbase_dep'])
nlohmann_json_dep = dependency('nlohmann_json', fallback: ['nlohmann_json', 'nlohmann_json_dep'])
boost_dep         = dependency('boost',
    modules:  ['filesystem', 'system'],
    required: true,
)
catch2_dep        = dependency('catch2-with-main', fallback: ['catch2', 'catch2_with_main_dep'])

common_deps = [
    libgimpwire_dep,
    libgimpbase_dep,
    nlohmann_json_dep,
    boost_dep,
] + platform_deps

# ----------------------------------------------------------------------------
# ソース
# ----------------------------------------------------------------------------
srcs = files(
    'src/config/config.cpp',
    'src/host/host.cpp',
    'src/host/pdb_stubs.cpp',
    'src/host/tile_transfer.cpp',
    'src/ipc/process.cpp',
    'src/ipc/wire_io.cpp',
    'src/csp/plugin_entry.cpp',
    'src/csp/buffer.cpp',
)

# ----------------------------------------------------------------------------
# MyGimpHost 共有ライブラリ
# ----------------------------------------------------------------------------
myGimpHost_lib = shared_library('MyGimpHost', srcs,
    dependencies: common_deps,
    install:      true,
    install_dir:  csp_output_dir,
)

# ----------------------------------------------------------------------------
# テスト
# ----------------------------------------------------------------------------
test_srcs = files(
    'tests/test_config.cpp',
    'tests/test_tile_transfer.cpp',
    'tests/test_buffer.cpp',
    'tests/test_wire_io.cpp',
    'tests/test_concurrency.cpp',
)

test_exe = executable('cspbridge_tests', test_srcs,
    dependencies: [catch2_dep, nlohmann_json_dep, boost_dep] + platform_deps,
    link_with:    myGimpHost_lib,
)

test('unit tests', test_exe)
