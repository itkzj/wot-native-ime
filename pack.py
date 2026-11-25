from cStringIO import StringIO
import os
import py_compile
import sys
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile

from pack_stock_filter import RESOURCE, stock_registration
from wot_native_ime_paths import mods_mount

VERSION = '1.0.5'


def wotmod(identifier, name, files):
    metadata = '<root><id>%s</id><version>%s</version><name>%s</name></root>' % (identifier, VERSION, name)
    entries = [('meta.xml', metadata)] + files
    payload = StringIO()
    with ZipFile(payload, 'w', ZIP_STORED) as mod:
        for path, data in entries:
            mod.writestr(path, data)
    with ZipFile(payload) as mod:
        assert mod.namelist() == [path for path, _ in entries]
        assert all(mod.getinfo(path).compress_type == ZIP_STORED and mod.read(path) == data for path, data in entries)
    return ROOT + identifier + '_' + VERSION + '.wotmod', payload.getvalue()


def read(path):
    with open(path, 'rb') as source:
        return source.read()


if len(sys.argv) > 2:
    sys.exit('usage: python pack.py [PATH_TO_GUI_PART3_PKG]')
ROOT = (mods_mount(os.path.abspath(os.path.join(os.path.dirname(sys.argv[1]), '..', '..')))
        if len(sys.argv) == 2 else 'mods/2.4.2.0') + '/'
scripts = []
for name in ('mod_wot_native_ime', 'wot_native_ime_paths'):
    output = 'build/' + name + '.pyc'
    py_compile.compile(name + '.py', output, name + '.py', doraise=True)
    scripts.append(('res/scripts/client/gui/mods/' + name + '.pyc', read(output)))
files = [wotmod('wot.native.ime', 'WoT Native IME', [
    ('res/gui/flash/NativeImeBridge.swf', read('build/NativeImeBridge.swf')),
    ('LICENSE', read('LICENSE')),
] + scripts), (ROOT + '_wot_native_ime.pyd', read('build/_wot_native_ime.pyd'))]
if len(sys.argv) == 2:
    files.append(wotmod('wot.native.ime.stock-filter', 'WoT Native IME Stock Filter', [
        ('res/' + RESOURCE, stock_registration(sys.argv[1]))]))
output = 'dist/WotNativeIme_V%s%s.zip' % (VERSION, '' if len(sys.argv) == 2 else '_Core')
with ZipFile(output, 'w', ZIP_DEFLATED) as release:
    for path, data in files:
        release.writestr(path, data)
with ZipFile(output) as release:
    assert release.namelist() == [path for path, _ in files]
    assert all(release.read(path) == data for path, data in files)
print(output)
