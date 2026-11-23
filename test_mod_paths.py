import os
import shutil
import tempfile
from wot_native_ime_paths import native_library

root = tempfile.mkdtemp()
try:
    exe = os.path.join(root, 'win64', 'WorldOfTanks.exe')
    def config(*mounts):
        with open(os.path.join(root, 'paths.xml'), 'w') as f:
            f.write('<root><Paths><Path>./res_mods/1.0</Path>')
            for mount in mounts:
                f.write('<Path mask="*.wotmod" root="res">%s</Path>' % mount)
            f.write('</Paths></root>')

    for version in ('1.0', '2.5.0.1', '99.0'):
        folder = os.path.join(root, 'mods', version)
        os.makedirs(folder)
        open(os.path.join(folder, '_wot_native_ime.pyd'), 'wb').close()
    config('./mods/2.5.0.1')
    expected = os.path.join(root, 'mods', '2.5.0.1', '_wot_native_ime.pyd')
    assert native_library(exe, os.path.dirname(root)) == expected
    assert native_library('', root) == expected
    os.remove(expected)
    try:
        native_library(exe, root)
    except IOError as e:
        assert expected in str(e)
    else:
        raise AssertionError('Loaded a stale module')
    for mounts in [(), ('./mods/1.0', './mods/99.0'), ('../mods/1.0',)]:
        config(*mounts)
        try:
            native_library(exe, root)
        except ValueError:
            pass
        else:
            raise AssertionError('Accepted missing, ambiguous or unsafe configuration')
finally:
    shutil.rmtree(root)
print('Active mod path regression passed')
