"""Resolve native mods against the same mount the WoT client configures."""
import os
import re
from xml.etree import ElementTree


def mods_mount(root):
    config = os.path.join(root, 'paths.xml')
    mounts = set()
    for entry in ElementTree.parse(config).findall('./Paths/Path'):
        if entry.get('mask') != '*.wotmod':
            continue
        path = (entry.text or '').strip().replace('\\', '/')
        if path.startswith('./'):
            path = path[2:]
        if not re.match(r'mods/[0-9]+(?:\.[0-9]+)*\Z', path):
            raise ValueError('Unexpected .wotmod mount %r in %s' % (path, config))
        mounts.add(path)
    if len(mounts) != 1:
        raise ValueError('Expected one active .wotmod mount in %s, found %d' % (config, len(mounts)))
    return mounts.pop()


def native_library(executable, cwd):
    root = os.path.dirname(executable) if executable and os.path.isabs(executable) else cwd
    if os.path.basename(root).lower() in ('win32', 'win64'):
        root = os.path.dirname(root)
    path = os.path.normpath(os.path.join(root, mods_mount(root), '_wot_native_ime.pyd'))
    if not os.path.isfile(path):
        raise IOError('Native IME module missing from active mod directory: %s' % path)
    return path
