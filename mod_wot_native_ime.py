# -*- coding: utf-8 -*-
"""Attach Windows IME to focused stock Scaleform chat and search fields."""
import imp
import os
import sys
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
LIBRARY = os.path.join('mods', '2.4.1.1', '_wot_native_ime.pyd')
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
_diag_budget = 40
_diag_ticks = 0
_diag_last = None
_candidate_display = None
_EMPTY_CANDIDATES = (0, -1, False, [])


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
            rects = tuple(map(int, snapshot[5:]))
            if generation > 0 and 0 <= first <= last <= len(text):
                if _binding != (view, generation):
                    _token = _token % 4294967295 + 1
                    _binding = (view, generation)
                _library.ImeSnapshot(_token, text, first, last, int(bool(snapshot[4])), rects)
                return
    except Exception:
        _error('Could not sample the original field')
    _binding = None
    _library.ImeClear()


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


def _load_library():
    executable_dir = os.path.dirname(os.path.abspath(sys.executable)) if sys.executable else os.getcwd()
    for root in (executable_dir, os.path.dirname(executable_dir), os.getcwd()):
        path = os.path.join(root, LIBRARY)
        if os.path.isfile(path):
            return imp.load_dynamic('_wot_native_ime', path)
    raise IOError('_wot_native_ime.pyd is missing from mods/2.4.1.1')


def _update_candidates(view, page):
    global _candidate_display
    if _candidate_display == (view, page) or (_candidate_display is None and not page[0]):
        return
    previous = _candidate_display
    _candidate_display = None
    try:
        if previous and previous[0] is not view:
            previous[0].flashObject.as_candidates(*_EMPTY_CANDIDATES)
        if page[0] or (previous and previous[0] is view):
            view.flashObject.as_candidates(*page)
        if page[0]:
            _candidate_display = (view, page)
    except Exception:
        _error('Could not update the candidate panel')


def _tick():
    global _tick_id, _status, _diag_budget, _diag_ticks, _diag_last
    _tick_id = None
    view = _active()
    if not _enabled or view is None:
        if _library is not None:
            _library.ImeStop()
        _update_candidates(None, _EMPTY_CANDIDATES)
        return
    try:
        attached = _library.ImeStart(_refresh, _commit)
        if attached:
            choice = view.flashObject.as_takeChoice() if (
                _candidate_display and _candidate_display[0] is view and _candidate_display[1][2]) else None
            if choice is None:
                _library.ImeTick()
            else:
                serial, row = map(int, choice)
                _library.ImeTick(serial, row)
        _update_candidates(view, _library.ImeCandidates() if attached else _EMPTY_CANDIDATES)
        status = _library.ImeStatus()
        if status != _status:
            _status = status
            print('[WotNativeIme] ' + _status_labels[status])
        if status in (2, 3, 4) and _diag_budget:
            # Sample about twice a second and log only changes.
            if _diag_ticks == 0:
                report = _library.ImeDiagnostics()
                if report != _diag_last:
                    print('[WotNativeIme] TSF diagnostic ' + report)
                    _diag_last = report
                    _diag_budget -= 1
            _diag_ticks = (_diag_ticks + 1) % 32
        else:
            _diag_ticks = 0
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
        global _lobby, _battle, _binding, _candidate_display
        if _candidate_display and _candidate_display[0] is self:
            _candidate_display = None
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
        print('[WotNativeIme] V8 mod loaded')
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
    _update_candidates(None, _EMPTY_CANDIDATES)
