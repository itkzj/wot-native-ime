# -*- coding: utf-8 -*-
from cStringIO import StringIO
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile
import py_compile

py_compile.compile('mod_wot_native_ime.py', 'build/mod_wot_native_ime.pyc', 'mod_wot_native_ime.py', doraise=True)
metadata = '''<?xml version="1.0" encoding="utf-8"?>
<root>
    <id>wot.native.ime</id>
    <version>1.0.13</version>
    <name>WoT Native IME</name>
    <description>Windows IME for the original WoT CN 2.4.1.1 Scaleform fields</description>
</root>
'''
mod_bytes = StringIO()
mod_name = 'wot.native.ime_1.0.13.wotmod'
mod_entries = ['meta.xml', 'res/gui/flash/NativeImeBridge.swf', 'res/scripts/client/gui/mods/mod_wot_native_ime.pyc']
with ZipFile(mod_bytes, 'w', ZIP_STORED) as mod:
    mod.writestr(mod_entries[0], metadata)
    mod.write('build/NativeImeBridge.swf', mod_entries[1])
    mod.write('build/mod_wot_native_ime.pyc', mod_entries[2])
root = 'mods/2.4.1.1/'
output = 'dist/WotNativeIme_V14.zip'
extension = '_wot_native_ime.pyd'
with ZipFile(output, 'w', ZIP_DEFLATED) as release:
    release.writestr(root + mod_name, mod_bytes.getvalue())
    release.write('build/' + extension, root + extension)
with ZipFile(output) as release:
    assert release.testzip() is None
    assert release.namelist() == [root + mod_name, root + extension]
    with ZipFile(StringIO(release.read(root + mod_name))) as mod:
        assert mod.testzip() is None and mod.namelist() == mod_entries
        assert all(entry.compress_type == ZIP_STORED for entry in mod.infolist())
print(output)
