# -*- coding: utf-8 -*-
"""Check the on-disk native extension with ctypes unavailable."""
import imp
import os
import sys

sys.modules['_ctypes'] = None
path = os.path.abspath('build/_wot_native_ime.pyd')
ime = imp.load_dynamic('_wot_native_ime', path)
assert ime.ImeStatus() == 0
assert ime.ImeDiagnostics() == ('focus=0 tip=0 blocked=0 down=0 processKey=0 wanted=0 '
                                'handled=0 noSink=0 failed=0 composition=0 caret=0/0')
try:
    ime.ImeStart(None, None)
except TypeError:
    pass
else:
    raise AssertionError('Native extension accepted missing callbacks')
rects = (20, 20, 20, 40, 10, 10, 100, 100)
ime.ImeSnapshot(1, u'聊天', 2, 2, 0, rects)
try:
    ime.ImeSnapshot(1, u'a\0b', 0, 0, 0, rects)
except ValueError:
    pass
else:
    raise AssertionError('Native extension accepted an embedded NUL')
ime.ImeClear()
ime.ImeTick()
ime.ImeStop()
print('Native extension loaded without _ctypes on Python ' + sys.version.splitlines()[0])
