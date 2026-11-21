# -*- coding: utf-8 -*-
"""Attach Windows IME to focused stock Scaleform chat and search fields."""
import ctypes as C
import os
import traceback

import BigWorld
from frameworks.wulf import WindowLayer
from gui.Scaleform.framework import ScopeTemplates, ViewSettings
from gui.Scaleform.framework.entities.View import View
from gui.Scaleform.framework.managers.loaders import SFViewLoadParams
from gui.app_loader.settings import APP_NAME_SPACE
from gui.shared import events, g_eventBus
from helpers import dependency
from messenger.gui.Scaleform.view.battle.messenger_view import BattleMessengerView
from skeletons.gui.app_loader import IAppLoader
from skeletons.gui.impl import IGuiLoader

LOBBY = 'WotNativeImeLobby'
BATTLE = 'WotNativeImeBattle'
LIBRARY = os.path.join('mods', '2.4.1.1', 'WotNativeIme.dll')
Refresh = C.WINFUNCTYPE(None)
Commit = C.WINFUNCTYPE(C.c_int, C.c_uint, C.c_wchar_p, C.c_int, C.c_int, C.c_int,
                       C.c_wchar_p, C.c_int, C.c_int, C.c_int)
Rectangles = C.c_int * 8
_lobby = _battle = _battle_owner = None
_library = None
_tick_id = None
_binding = None
_token = 0
_enabled = _listening = False
_status = None
_status_labels = {0: 'waiting for game window', 1: 'hook ready', 2: 'TSF attached',
                  3: 'IMM attached', 4: 'TSF association failed'}
_errors = set()


def _error(where):
    if where not in _errors:
        _errors.add(where)
        print('[WotNativeIme] ' + where)
        traceback.print_exc()


def _text(value):
    if isinstance(value, str):
        value = value.decode('utf-8', 'strict')
    if not isinstance(value, unicode) or u'\0' in value or len(value) > 8192:
        raise ValueError('Unexpected Scaleform text')
    return value


def _active():
    return _battle if _battle_owner is not None else _lobby


def _refresh():
    global _binding, _token
    try:
        view = _active()
        obj = view.flashObject if _enabled and view is not None else None
        snapshot = obj.as_snapshot() if obj else None
        if snapshot is not None:
            if len(snapshot) != 13:
                raise ValueError('Unexpected Scaleform snapshot')
            generation = int(snapshot[0])
            text = _text(snapshot[1])
            first, last = map(int, snapshot[2:4])
            rects = Rectangles(*map(int, snapshot[5:]))
            if generation > 0 and 0 <= first <= last <= len(text):
                if _binding != (view, generation):
                    _token = _token % 4294967295 + 1
                    _binding = (view, generation)
                _library.ImeSnapshot(_token, text, first, last, int(bool(snapshot[4])), rects)
                return
    except Exception:
        _error('Could not sample the original field')
    _binding = None
    _library.ImeSnapshot(0, None, 0, 0, 0, None)


def _commit(token, before, first, last, leading, after, new_first, new_last, new_leading):
    view = _active()
    if not _enabled or not _binding or view is None or _binding[0] is not view or token != _token:
        return 0
    try:
        return int(bool(view.flashObject.as_commit(_binding[1], _text(before), first, last, bool(leading),
                                                    _text(after), new_first, new_last, bool(new_leading))))
    except Exception:
        _error('Could not apply IME result to the original field')
        return 0


# Both callbacks must outlive the native subclass and hook.
_refresh_callback = Refresh(_refresh)
_commit_callback = Commit(_commit)


def _load_library():
    filename = C.create_unicode_buffer(32768)
    get_filename = C.windll.kernel32.GetModuleFileNameW
    get_filename.argtypes = [C.c_void_p, C.POINTER(C.c_wchar), C.c_uint]
    get_filename.restype = C.c_uint
    if not 0 < get_filename(None, filename, len(filename)) < len(filename):
        raise IOError('Could not locate the game executable')
    executable_dir = os.path.dirname(filename.value)
    for root in (executable_dir, os.path.dirname(executable_dir)):
        path = os.path.join(root, LIBRARY)
        if os.path.isfile(path):
            library = C.WinDLL(path)
            library.ImeStart.argtypes = [Refresh, Commit]
            library.ImeStart.restype = C.c_int
            library.ImeStatus.argtypes = []
            library.ImeStatus.restype = C.c_int
            library.ImeSnapshot.argtypes = [C.c_uint, C.c_wchar_p, C.c_int, C.c_int, C.c_int, C.POINTER(C.c_int)]
            for fn in (library.ImeTick, library.ImeStop, library.ImeSnapshot):
                fn.restype = None
            library.ImeTick.argtypes = library.ImeStop.argtypes = []
            return library
    raise IOError('WotNativeIme.dll is missing from mods/2.4.1.1')


def _tick():
    global _tick_id, _status
    _tick_id = None
    if not _enabled or _active() is None:
        if _library is not None:
            _library.ImeStop()
        return
    try:
        attached = _library.ImeStart(_refresh_callback, _commit_callback)
        if attached:
            _library.ImeTick()
        status = _library.ImeStatus()
        if status != _status:
            _status = status
            print('[WotNativeIme] ' + _status_labels[status])
    except Exception:
        _error('Native input host unavailable')
        attached = False
    _tick_id = BigWorld.callback(0.016 if attached else 0.25, _tick)


def _queue_tick():
    global _tick_id
    if _enabled and _tick_id is None:
        _tick_id = BigWorld.callback(0, _tick)


class NativeImeView(View):
    battle = False

    def _populate(self):
        global _lobby, _battle
        super(NativeImeView, self)._populate()
        if not _enabled or (self.battle and (_battle_owner is None or _battle_owner.app is not self.app)):
            return
        try:
            self.flashObject.as_start(self.battle)
            if self.battle:
                _battle = self
            else:
                _lobby = self
            _queue_tick()
        except Exception:
            _error('Could not initialize Scaleform input bridge')

    def _dispose(self):
        global _lobby, _battle, _binding
        if _binding and _binding[0] is self:
            _binding = None
        if _lobby is self:
            _lobby = None
        if _battle is self:
            _battle = None
        try:
            self.flashObject.as_stop()
        finally:
            super(NativeImeView, self)._dispose()


class BattleImeView(NativeImeView):
    battle = True


def _load_view(app, alias):
    manager = dependency.instance(IGuiLoader).windowsManager
    app.loadView(SFViewLoadParams(alias, parent=manager.getMainWindow()))


def _on_lobby_loaded(_event):
    if not _enabled or _lobby is not None:
        return
    try:
        app = dependency.instance(IAppLoader).getApp(APP_NAME_SPACE.SF_LOBBY)
        if app is not None:
            _load_view(app, LOBBY)
    except Exception:
        _error('Could not load lobby input bridge')


_populate = BattleMessengerView._populate
_dispose = BattleMessengerView._dispose


def _on_populate(view):
    global _battle_owner, _battle, _binding
    result = _populate(view)
    if _enabled:
        _battle_owner, _battle, _binding = view, None, None
        try:
            _load_view(view.app, BATTLE)
        except Exception:
            _error('Could not load battle input bridge')
    return result


def _on_dispose(view):
    global _battle_owner, _battle, _binding
    if _battle_owner is view:
        _battle_owner = _battle = _binding = None
        _queue_tick()
    return _dispose(view)


def init():
    global _library, _enabled, _listening
    try:
        _library = _load_library()
        factory = dependency.instance(IGuiLoader).entitiesFactory
        for alias, view in ((LOBBY, NativeImeView), (BATTLE, BattleImeView)):
            factory.addSettings(ViewSettings(alias, view, 'NativeImeBridge.swf', WindowLayer.SERVICE_LAYOUT,
                                             None, ScopeTemplates.DEFAULT_SCOPE, canDrag=False, canClose=False,
                                             isModal=False))
        g_eventBus.addListener(events.GUICommonEvent.LOBBY_VIEW_LOADED, _on_lobby_loaded)
        _listening = True
        BattleMessengerView._populate = _on_populate
        BattleMessengerView._dispose = _on_dispose
        _enabled = True
        print('[WotNativeIme] Mod loaded')
    except Exception:
        _error('Could not register native IME')


def fini():
    global _enabled, _listening, _tick_id, _binding
    _enabled = False
    _binding = None
    if _listening:
        g_eventBus.removeListener(events.GUICommonEvent.LOBBY_VIEW_LOADED, _on_lobby_loaded)
        _listening = False
    for name, hook, original in (('_populate', _on_populate, _populate), ('_dispose', _on_dispose, _dispose)):
        current = getattr(BattleMessengerView, name)
        if getattr(current, 'im_func', current) is hook:
            setattr(BattleMessengerView, name, original)
    if _tick_id is not None:
        BigWorld.cancelCallback(_tick_id)
        _tick_id = None
    if _library is not None:
        _library.ImeStop()
