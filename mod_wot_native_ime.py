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

from gui.mods.wot_native_ime_paths import native_library

LOBBY = 'WotNativeImeLobby'
BATTLE = 'WotNativeImeBattle'
_lobby = _battle = _battle_owner = None
_library = None
_tick_id = None
_route_epoch = 0
_gameplay_owner = _focus_call = None
_battle_baseline = False  # None permits one retry before the first stock chat open.
_binding = None
_token = 0
_enabled = _listening = False
_status = None
_status_labels = {0: 'waiting for game window', 1: 'hook ready', 2: 'TSF attached',
                  3: 'IMM attached', 4: 'TSF association failed',
                  5: 'battle gameplay, IME released to game'}
_errors = set()
_candidate_display = None
_EMPTY_CANDIDATES = (-1, [], u'')


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
    epoch = _route_epoch
    try:
        view = _active()
        obj = view.flashObject if _enabled and view is not None else None
        if obj and _battle_owner is not None and (_gameplay_owner is _battle_owner or not _battle_owner.isFocused()):
            obj = None
        snapshot = obj.as_snapshot() if obj else None
        if epoch != _route_epoch:
            return  # Flash can synchronously change the route while sampling.
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
        if epoch != _route_epoch:
            return
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
    path = native_library(sys.executable, os.getcwdu())
    library = imp.load_dynamic('_wot_native_ime', path)
    print('[WotNativeIme] native module: %r' % path)
    return library


def _update_candidates(view, page):
    global _candidate_display
    generation = _binding[1] if _binding and _binding[0] is view else 0
    display = (view, generation, page) if generation and (page[1] or page[2]) else None
    if display == _candidate_display:
        return
    previous = _candidate_display
    _candidate_display = None
    try:
        if previous and (not display or previous[0] is not view):
            previous[0].flashObject.as_candidates(0, *_EMPTY_CANDIDATES)
        if display:
            view.flashObject.as_candidates(generation, *page)
            _candidate_display = display
    except Exception:
        _error('Could not update the candidate panel')


def _tick(epoch, closing=False):
    global _tick_id, _status, _binding, _gameplay_owner
    if epoch != _route_epoch:
        return  # A callback can already be dequeued when its route is cancelled.
    _tick_id = None
    view = _active()
    if _enabled and _battle_owner is not None:
        if closing or not _battle_owner.isFocused():
            if _gameplay_owner is not _battle_owner:
                _gameplay_owner = _battle_owner
                _binding = None
                try:
                    released = _library.ImeGameplay()
                except Exception:
                    released = False
                    _error('Could not release battle input bridge')
                _update_candidates(None, _EMPTY_CANDIDATES)
                if released and _status != 5:
                    _status = 5
                    print('[WotNativeIme] ' + _status_labels[5])
            return
        if _focus_call is _battle_owner and _gameplay_owner is _battle_owner:
            return  # The stock open call is still running.
    _gameplay_owner = None
    if not _enabled or view is None:
        if _library is not None:
            _library.ImeStop()
        _update_candidates(None, _EMPTY_CANDIDATES)
        return
    try:
        attached = _library.ImeStart(_refresh, _commit)
        if attached:
            _library.ImeTick()
        if epoch != _route_epoch:
            return
        status = _library.ImeStatus()
        page = _library.ImeCandidates() if attached and status in (2, 3) else _EMPTY_CANDIDATES
        _update_candidates(view, page)
        if status != _status:
            _status = status
            print('[WotNativeIme] ' + _status_labels[status])
    except Exception:
        _error('Native input host unavailable')
        attached = False
    if epoch == _route_epoch and _enabled:
        _tick_id = BigWorld.callback(0.016 if attached else 0.25, lambda: _tick(epoch))


def _cancel_route():
    global _route_epoch, _tick_id
    _route_epoch += 1
    if _tick_id is not None:
        BigWorld.cancelCallback(_tick_id)
        _tick_id = None


def _switch_route(closing=False):
    _cancel_route()
    epoch = _route_epoch
    _tick(epoch, closing)
    return epoch


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
            if self is _active() and _gameplay_owner is None:
                _switch_route()
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


def _gui_released(view):
    # The public hasGuiControlModeConsumers(*ids) only checks the specified IDs.
    return (len(view.app._BattleEntry__input._BattleGameInputMgr__consumers) == 0
            and not BigWorld.player().isForcedGuiControlMode())


def _capture_baseline(view, allow_retry):
    global _battle_baseline
    epoch = _route_epoch
    _battle_baseline = False
    try:
        ready = (_gameplay_owner is view and not view.isFocused() and _gui_released(view)
                 and _library.ImeStatus() == 5 and _library.ImeBattleBaseline())
    except Exception:
        ready = False
        _error('Could not sample battle input baseline')
    if _enabled and _battle_owner is view and epoch == _route_epoch:
        _battle_baseline = ready or (None if allow_retry else False)


def _queue_handoff(view, epoch, attempt=1):
    global _tick_id
    def after_stock_close():
        global _tick_id
        if not _enabled or epoch != _route_epoch or _battle_owner is not view:
            return
        _tick_id = None
        try:
            if _gameplay_owner is not view or view.isFocused():
                return
            result = _library.ImeBattleHandoff() if _gui_released(view) else 'gui-consumer'
            print('[WotNativeIme] battle handoff baseline=%d result=%s attempt=%d' %
                  (bool(_battle_baseline), result, attempt))
            if attempt == 1 and result == 'retry-chat' and epoch == _route_epoch:
                _queue_handoff(view, epoch, 2)
        except Exception:
            _error('Could not hand battle input back to game')
    # Unwind the stock focusLostS stack first. A confirmed same-chat rebound gets one recheck.
    _tick_id = BigWorld.callback(0 if attempt == 1 else 0.05, after_stock_close)


def _watch_focus(original, before_open=False):
    def changed(view, value):
        global _focus_call, _battle_baseline
        if not _enabled or view is not _battle_owner:
            return original(view, value)
        outer = _focus_call
        _focus_call = view
        completed = False
        close_epoch = None
        try:
            if value and _gameplay_owner is view and _tick_id is not None:
                _cancel_route()  # Invalidate the last close before stock can reenter.
            if value and _battle_baseline is None:
                if before_open and outer is not view:
                    _capture_baseline(view, False)
                else:
                    _battle_baseline = False
            if not value and _gameplay_owner is not view:
                close_epoch = _switch_route(closing=True)
            result = original(view, value)
            completed = True
            return result
        finally:
            _focus_call = outer
            if outer is not view and _enabled and _battle_owner is view and _gameplay_owner is view:
                if (completed or not value) and view.isFocused():
                    _switch_route()
                elif completed and _battle_baseline and close_epoch == _route_epoch:
                    _queue_handoff(view, close_epoch)
    return changed


_focus_hooks = [(name, getattr(BattleMessengerView, name), _watch_focus(getattr(BattleMessengerView, name), name == 'setFocused'))
                for name in ('setFocused', '_BattleMessengerView__setGuiMode')]


def _end_battle():
    global _battle_owner, _battle, _binding, _battle_baseline, _gameplay_owner
    _battle_owner = _battle = _binding = _gameplay_owner = None
    _battle_baseline = False
    try:
        _library.ImeBattleEnd()
    except Exception:
        _error('Could not clear battle input baseline')


def _on_populate(view):
    global _battle_owner, _battle, _binding
    result = _populate(view)
    if _enabled and _battle_owner is not view:
        if _battle_owner is not None:
            _end_battle()
        _battle_owner, _battle, _binding = view, None, None
        entry_epoch = _switch_route()
        if _battle_owner is view and entry_epoch == _route_epoch and _gameplay_owner is view:
            _capture_baseline(view, True)
        try:
            _load_view(view.app, BATTLE)
        except Exception:
            _error('Could not load battle input bridge')
    return result


def _on_dispose(view):
    if _battle_owner is view:
        _end_battle()
        _switch_route()
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
        for name, original, hook in _focus_hooks:
            setattr(BattleMessengerView, name, hook)
        _enabled = True
        print('[WotNativeIme] V1.0.8-beta mod loaded')
    except Exception:
        _error('Could not register native IME')


def fini():
    global _enabled, _listening, _binding
    _enabled = False
    _cancel_route()
    if _battle_owner is not None:
        _end_battle()
    _binding = None
    if _listening:
        g_eventBus.removeListener(events.GUICommonEvent.LOBBY_VIEW_LOADED, _on_lobby_loaded)
        _listening = False
    for name, original, hook in _focus_hooks + [('_populate', _populate, _on_populate), ('_dispose', _dispose, _on_dispose)]:
        current = getattr(BattleMessengerView, name)
        if getattr(current, 'im_func', current) is hook:
            setattr(BattleMessengerView, name, original)
    if _library is not None:
        _library.ImeStop()
    _update_candidates(None, _EMPTY_CANDIDATES)
