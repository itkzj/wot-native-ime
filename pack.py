from cStringIO import StringIO
import os
import py_compile
import sys
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile

from wot_native_ime_paths import mods_mount

VERSION = '1.0.8-beta'


def write_archive(destination, entries, compression):
    with ZipFile(destination, 'w', compression) as bundle:
        for path, data in entries:
            bundle.writestr(path, data)
    with ZipFile(destination) as bundle:
        assert bundle.namelist() == [path for path, _ in entries]
        assert all(bundle.getinfo(path).compress_type == compression and bundle.read(path) == data
                   for path, data in entries)


def read(path):
    with open(path, 'rb') as source:
        return source.read()


if len(sys.argv) != 2:
    sys.exit('usage: python pack.py PATH_TO_GUI_PART3_PKG')
ROOT = mods_mount(os.path.abspath(os.path.join(os.path.dirname(sys.argv[1]), '..', '..'))) + '/'
scripts = []
for name in ('mod_wot_native_ime', 'wot_native_ime_paths'):
    output = 'build/' + name + '.pyc'
    py_compile.compile(name + '.py', output, name + '.py', doraise=True)
    scripts.append(('res/scripts/client/gui/mods/' + name + '.pyc', read(output)))
loose_movie = 'res_mods/' + ROOT.split('/', 1)[1] + 'gui/flash/ime.swf'
metadata = '<root><id>wot.native.ime</id><version>%s</version><name>WoT Native IME</name></root>' % VERSION
mod = StringIO()
write_archive(mod, [('meta.xml', metadata),
                    ('res/gui/flash/NativeImeBridge.swf', read('build/NativeImeBridge.swf')),
                    ('LICENSE', read('LICENSE'))] + scripts, ZIP_STORED)
output = 'dist/WotNativeIme_V%s.zip' % VERSION
write_archive(output, [(ROOT + 'wot.native.ime_' + VERSION + '.wotmod', mod.getvalue()),
                       (ROOT + '_wot_native_ime.pyd', read('build/_wot_native_ime.pyd')),
                       (loose_movie, read('build/ime.swf'))], ZIP_DEFLATED)
print(output)
