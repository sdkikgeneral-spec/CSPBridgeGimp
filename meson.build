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
host_os  = host_machine.system()   # 'windows' or 'darwin'
cpu      = host_machine.cpu_family() # 'x86_64' or 'aarch64'
compiler = meson.get_compiler('cpp')

# SIMD フラグ
if cpu == 'x86_64'
    if compiler.get_id() == 'msvc'
        add_project_arguments('/arch:AVX2', language: 'cpp')
    else
        add_project_arguments('-mavx2', '-msse4.1', language: 'cpp')
    endif
elif cpu == 'aarch64'
    add_project_arguments('-march=armv8-a+simd', language: 'cpp')
endif

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
libgimpwire_dep    = dependency('libgimpwire',    fallback: ['gimp',          'libgimpwire_dep'])
libgimpbase_dep    = dependency('libgimpbase',    fallback: ['gimp',          'libgimpbase_dep'])
nlohmann_json_dep  = dependency('nlohmann_json',  fallback: ['nlohmann_json', 'nlohmann_json_dep'])
boost_dep          = dependency('boost',
    modules:  ['filesystem', 'system'],
    required: true,
)
catch2_dep         = dependency('catch2-with-main', fallback: ['catch2', 'catch2_with_main_dep'])

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
    dependencies: [
        libgimpwire_dep,
        libgimpbase_dep,
        nlohmann_json_dep,
        boost_dep,
    ],
    install:     true,
    install_dir: csp_output_dir,
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
    dependencies: [
        catch2_dep,
        nlohmann_json_dep,
        boost_dep,
    ],
    link_with: myGimpHost_lib,
)

test('unit tests', test_exe)
