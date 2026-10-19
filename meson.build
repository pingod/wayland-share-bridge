project('wayland-share-bridge',
        'c',
        version : '1.0.0',
        license : 'EUPL-1.2',
        meson_version : '>= 0.60',
        default_options : ['warning_level=3', 'c_std=gnu11'])

cc = meson.get_compiler('c')
if cc.has_argument('-Wno-pedantic')
    add_project_arguments('-Wno-pedantic', language : 'c')
endif

deps = [
    dependency('dbus-1'),
    dependency('libpipewire-0.3'),
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
    'src/pipewire_handler.c',
    'src/tray_handler.c',
    'src/wayland_capture.c',
    'src/x11_handler.c',
])

executable('wayland-share-bridge',
           sources + generated,
           dependencies : deps,
           install : true)

install_data('data/wayland-share-bridge-launch',
             install_dir : get_option('bindir'),
             install_mode : 'rwxr-xr-x')

install_data('data/wayland-share-bridge.desktop',
             install_dir : get_option('datadir') / 'applications')

install_man('man/wayland-share-bridge.1')
