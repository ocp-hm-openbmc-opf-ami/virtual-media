project(
    'nbd',
    'c',
    version: '3.24',
    meson_version: '>=0.55.0',
    license: 'GPL')

nbd = include_directories('.')

nbd_dep = declare_dependency(
    include_directories: nbd
)

meson.override_dependency('nbd', nbd_dep)