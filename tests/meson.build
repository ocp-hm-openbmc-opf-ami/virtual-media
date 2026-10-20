
# tests/meson.build

# --- Googletest and GMock setup ---------------------------------------------
# Use gtest WITHOUT its main() because we compile our own src/main.cpp.
gtest_dep = dependency('gtest', main: false, version: '>=1.11.0', disabler: true, required: false)
gmock_dep = dependency('gmock', disabler: true, required: false)

if not gtest_dep.found() or not gmock_dep.found()
  cmake = import('cmake')
  opt_var = cmake.subproject_options()

  # Keep warning levels aligned with project policy
  opt_var.add_cmake_defines({'CMAKE_CXX_FLAGS': '-Wno-pedantic'})
  opt_var.set_override_option('warning_level', '3')
  opt_var.set_override_option('werror', 'true')

  # Bring in googletest via CMake subproject (wrap), if not present on system
  gtest_proj = cmake.subproject('gtest', options: opt_var, required: true)
  if gtest_proj.found()
    gtest_dep = declare_dependency(
      dependencies: [dependency('threads'), gtest_proj.dependency('gtest')]
    )
    gmock_dep = gtest_proj.dependency('gmock')
  else
    assert(not get_option('tests').enabled(), 'Googletest is required if tests are enabled')
  endif
endif

# --- Header-only JSON dependency --------------------------------------------
# Reuse the nlohmann_json dependency already resolved in the top-level meson.build.
nlohmann_json_dep = nlohmann_json
libudev_dep   = dependency('libudev', required: true)
# --- Runtime config path passed to tests -------------------------------------
# Use a test-local, writable JSON file path (the file can be created by tests).
vm_json_path = meson.current_build_dir() + '/virtual-media.json'
vm_json_path_act = meson.current_build_dir() + '/virtual-media-act.json'
# --- Common includes, deps, and args -----------------------------------------
inc = include_directories('../src', 'src')
deps = [
  boost,
  gmock_dep,
  gtest_dep,
  sdbusplus,
  nlohmann_json_dep,   # ensure this is available to tests
  libudev_dep ,
]

common_cpp_args = [
  # Enable test-only D-Bus stubs used by vm_interface_test.cpp and
  # state/activating_state_test.cpp.
  '-DINJECT_MOCKS',
  '-DVIRTUAL_MEDIA_CONFIG_PATH="' + vm_json_path + '"',
]
# --- Split tests: one executable and suite per test file ---------------------
tests = {
  'logger'		: 'src/logger_test.cpp',
  'events'		: 'src/events_test.cpp',
  'utils'		: 'src/utils_test.cpp',
  'credentials'	: 'src/credentials_test.cpp',
  'system'		: 'src/system_test.cpp',
  'configuration'	: 'src/configuration_test.cpp',
  'nfs'			: 'src/nfs_test.cpp' ,
  'smb'			: 'src/smb_test.cpp',
  'resources'		: 'src/resources_test.cpp' ,
  'credentials_destructor' : 'src/credentials_destructor_test.cpp',
  'credentials_pipe_fail' : 'src/credentials_pipe_fail_test.cpp' ,
  'vm_interface'	: 'src/vm_interface_test.cpp',
  'basic_state'		: 'src/state/basic_state_test.cpp',
  'activating_state'	: 'src/state/activating_state_test.cpp',
  'ready_state'		: 'src/state/ready_state_test.cpp',
  'deactivating_state'	: 'src/state/deactivating_state_test.cpp',
  'active_state'	: 'src/state/active_state_test.cpp',
  'initial_state'	: 'src/state/initial_state_test.cpp',
  'state_machine'	: 'src/state_machine_test.cpp',
}
foreach name, src_file : tests
  exe_name = 'virtual-media-ut-' + name
  sources = ['src/main.cpp', src_file]
  test_cpp_args = common_cpp_args

  # Common state test dependencies
  state_tests = ['activating_state', 'ready_state', 'deactivating_state', 
                 'active_state', 'initial_state', 'state_machine']
  
  if name in state_tests
    sources += [
      '../src/state/activating_state.cpp',
      'src/configuration_globals.cpp',
      '../src/vm_interface.cpp',
      '../src/resources.cpp',
      # activating_state.cpp defines creds[], so do NOT add creds_stub.cpp here
    ]
    # activating_state needs special JSON path
    if name == 'activating_state'
      test_cpp_args = [
        '-DINJECT_MOCKS',
        '-DVIRTUAL_MEDIA_CONFIG_PATH="' + vm_json_path_act + '"',
      ]
    endif
  elif name == 'configuration'
    sources += ['src/configuration_globals.cpp', 'src/creds_stub.cpp']
  elif name == 'vm_interface'
    sources += [
      '../src/vm_interface.cpp',
      'src/configuration_globals.cpp',
      'src/creds_stub.cpp',
    ]
  elif name == 'system'
    # system_test.cpp defines creds[], g_serviceName, g_basePath itself; no stubs needed
    sources += []
  else
    # events, basic_state, utils, nfs, smb, etc.
    sources += ['src/configuration_globals.cpp', 'src/creds_stub.cpp']
  endif

  ut_exe = executable(
    exe_name,
    sources,
    dependencies: deps,
    include_directories: inc,
    cpp_args: test_cpp_args,
    install: false,
  )

  test(
    exe_name,
    ut_exe,
    suite: name,    # allows: meson test --suite <name>
    timeout: 120,
  )
endforeach
