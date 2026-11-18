project('wayland-share-bridge',
        'c',
        version : '1.4.1',
        license : 'EUPL-1.2',
        meson_version : '>= 0.60',
        default_options : ['warning_level=3', 'c_std=gnu11'])

cc = meson.get_compiler('c')
if cc.has_argument('-Wno-pedantic')
    add_project_arguments('-Wno-pedantic', language : 'c')
endif
# Ubuntu's gcc enables this by default; keep local and CI builds in sync.
if cc.has_argument('-Wformat-security')
    add_project_arguments('-Wformat-security', language : 'c')
endif

# The project version above is the only place the version is defined; the
# binary and the manual page are generated from it.
version_conf = configuration_data()
version_conf.set_quoted('PROGRAM_VERSION', meson.project_version())
configure_file(output : 'version.h', configuration : version_conf)

man_conf = configuration_data()
man_conf.set('VERSION', meson.project_version())

deps = [
    dependency('dbus-1'),
    # PW_KEY_TARGET_OBJECT was introduced in 0.3.44.
    dependency('libpipewire-0.3', version : '>=0.3.44'),
    dependency('wayland-client'),
    dependency('x11'),
    dependency('xext'),
    dependency('xfixes'),
]

wayland_scanner = find_program('wayland-scanner')

protocols = {
    'ext-image-capture-source-v1' : files('protocols/ext-image-capture-source-v1.xml'),
    'ext-image-copy-capture-v1' : files('protocols/ext-image-copy-capture-v1.xml'),
    'ext-foreign-toplevel-list-v1' : files('protocols/ext-foreign-toplevel-list-v1.xml'),
}

generated = []
foreach name, xml : protocols
    generated += custom_target(
        name + '-client-header',
        input : xml,
        output : name + '-client-protocol.h',
        command : [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
    )
    generated += custom_target(
        name + '-private-code',
        input : xml,
        output : name + '-protocol.c',
        command : [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
    )
endforeach

sources = files([
    'src/main.c',
    'src/dbus_handler.c',
    'src/i18n.c',
    'src/image_convert.c',
    'src/pipewire_handler.c',
    'src/source_spec.c',
    'src/tray_handler.c',
    'src/util.c',
    'src/wayland_capture.c',
    'src/x11_handler.c',
    'src/state_file.c',
])

executable('wayland-share-bridge',
           sources + generated,
           dependencies : deps,
           install : true)

install_data('data/wayland-share-bridge-launch',
             install_dir : get_option('bindir'),
             install_mode : 'rwxr-xr-x')

install_data(configure_file(input : 'data/wayland-share-bridge.desktop.in',
                            output : 'wayland-share-bridge.desktop',
                            configuration : man_conf),
             install_dir : get_option('datadir') / 'applications')

install_data('data/wayland-share-bridge.service',
             install_dir : get_option('datadir') / 'systemd/user')

install_data('data/wayland-share-bridge.metainfo.xml',
             install_dir : get_option('datadir') / 'metainfo')

install_man(configure_file(input : 'man/wayland-share-bridge.1.in',
                           output : 'wayland-share-bridge.1',
                           configuration : man_conf))

# Unit tests cover the pure helpers: UTF-8 handling, source spec parsing,
# frame conversion and the state file.
unit_tests = executable('unit-tests',
                        files([
                            'tests/unit.c',
                            'src/util.c',
                            'src/source_spec.c',
                            'src/image_convert.c',
                            'src/state_file.c',
                        ]),
                        include_directories : 'src')

test('unit', unit_tests, timeout : 120)
