# -*- coding: utf-8 -*-
"""Exercise the Python 2.7 bridge outside the game with sampled Scaleform states."""
import ast


class View(object):
    def __init__(self):
        self.flashObject = self
        self.generation = 0
        self.pages = []

    def _populate(self):
        pass

    def as_start(self, battle):
        pass

    def as_snapshot(self):
        if self.generation:
            return [self.generation, u'a', 1, 1, False, 1, 1, 1, 2, 0, 0, 10, 10]

    def as_commit(self, generation, *edit):
        return generation == self.generation

    def as_candidates(self, generation, *page):
        self.pages.append((generation, page))


class Native(object):
    token = 0

    def ImeSnapshot(self, token, *args):
        self.token = token

    focus = 'stock-chat'
    saved = None
    def __init__(self):
        self.events = []

    def ImeGameplay(self):
        self.events.append('release')
        if self.saved is not None:
            self.focus, self.saved = self.saved, None
        self.token = 'gameplay'
        return True

    baseline = False
    chat_document = False

    def ImeBattleBaseline(self):
        self.events.append('baseline')
        self.baseline = not self.chat_document
        return self.baseline

    def ImeBattleHandoff(self):
        self.events.append('handoff')
        if not self.baseline:
            return 'skipped'
        self.chat_document = False
        return 'restored-chat'

    def ImeBattleEnd(self):
        self.baseline = False
        self.events.append('end')

    def ImeStart(self, refresh, commit):
        self.events.append('attach')
        if self.saved is None:
            self.saved, self.focus = self.focus, 'ime'
        self.refresh = refresh
        return True

    def ImeTick(self):
        self.refresh()

    def ImeStatus(self):
        if self.token == 'gameplay':
            return 5
        return 2 if self.token else 1

    def ImeCandidates(self):
        return (-1, [], u'')

    def ImeClear(self):
        self.token = 0


class BattleChat(object):
    opened = False

    def __init__(self):
        self.app = self._BattleEntry__input = self
        self.consumers = self._BattleGameInputMgr__consumers = set()

    def isFocused(self):
        return self.opened


class Clock(object):
    pending = None
    forced_gui = False

    def player(self):
        return self

    def isForcedGuiControlMode(self):
        return self.forced_gui

    def callback(self, delay, callback):
        assert self.pending is None
        self.pending, self.delay = callback, delay
        return 1

    def run(self):
        callback, self.pending = self.pending, None
        callback()

    def cancelCallback(self, handle):
        assert handle == 1 and self.pending
        self.pending = None


def new_fixture():
    view, native, clock = View(), Native(), Clock()
    state = dict(__builtins__=__builtins__, View=View, BigWorld=clock, _library=native, _enabled=True,
                 _lobby=view, _battle=None, _battle_owner=None, _gameplay_owner=None, _focus_call=None,
                 _tick_id=None, _route_epoch=0, _battle_baseline=False, _binding=None, _token=0,
                 _candidate_display=None, _EMPTY_CANDIDATES=(-1, [], u''), _status=None,
                 _status_labels={1: 'stock', 2: 'editing', 5: 'direct'}, BATTLE='battle',
                 _populate=lambda owner: None, _dispose=lambda owner: None)
    # Load production callbacks and views without importing the unavailable game modules.
    with open('mod_wot_native_ime.py', 'rb') as source:
        module = ast.parse(source.read(), source.name)
    module.body = [node for node in module.body if isinstance(node, (ast.FunctionDef, ast.ClassDef))]
    exec compile(module, source.name, 'exec') in state
    state['_load_view'] = lambda *args: None
    return state, native, view, clock


edit = (u'a', 1, 1, False, u'ab', 2, 2, False)


def test_snapshots():
    state, native, view, clock = new_fixture()
    refresh, commit, candidates = (state[key] for key in ('_refresh', '_commit', '_update_candidates'))
    page = (0, [u'b'], u'b')

    refresh()
    assert native.token == 0
    view.generation = 1
    refresh()
    first = native.token
    assert first and commit(first, *edit)
    candidates(view, page)
    view.generation = 0  # The stock route took over between a sample and a native result.
    assert not commit(first, *edit)
    refresh()
    assert native.token == 0 and not commit(first, *edit)
    candidates(view, page)
    assert view.pages[-1] == (0, state['_EMPTY_CANDIDATES'])
    view.generation = 2
    refresh()
    assert native.token != first and not commit(first, *edit) and commit(native.token, *edit)
    candidates(view, page)
    view.generation = 3  # Identical candidates on the next field must be redrawn.
    refresh()
    candidates(view, page)
    candidates(view, page)
    assert view.pages == [(1, page), (0, state['_EMPTY_CANDIDATES']), (2, page), (3, page)]


def test_routes():
    state, native, view, clock = new_fixture()
    refresh, commit = state['_refresh'], state['_commit']
    view.generation = 3
    chat = BattleChat()

    def stock_mode(owner, value):
        owner.opened = value
        if value:
            native.chat_document = True  # The stock close can leave this TSF focus behind.
            owner.consumers.add('chat')
        else:
            owner.consumers.discard('chat')
        native.events.append('enter' if value else 'leave')
        native.focus = 'stock-chat' if value else 'vehicle'

    mode = state['_watch_focus'](stock_mode)

    def stock_set_focused(owner, value):
        if value == owner.opened:
            return False
        owner.opened = value
        mode(owner, value=value)  # Synchronous Scaleform focusReceived / focusLost.
        native.events.append('set-done')
        return True

    change = state['_watch_focus'](stock_set_focused, before_open=True)
    state['_on_populate'](chat)
    assert native.events == ['release', 'baseline'], native.events
    state['_battle'] = view
    assert native.token == 'gameplay' and state['_binding'] is None and clock.pending is None
    native.events[:] = []
    assert change(chat, True)
    assert native.events == ['enter', 'set-done', 'attach']
    assert clock.pending and native.token != 'gameplay' and commit(native.token, *edit)
    view.generation = 0
    refresh()  # Open chat that is being handled by the stock input method.
    assert native.token == 0
    native.events[:] = []
    stale_input_tick = clock.pending
    assert change(chat, False)
    handoff = clock.pending
    stale_input_tick()  # Already dequeued when the stock close started.
    assert state['_tick_id'] is not None and clock.pending is handoff
    for alias in ('NativeImeView', 'BattleImeView'):
        overlay = state[alias]()
        overlay.app = chat.app
        overlay._populate()  # Asynchronous view loading must not steal the handoff.
        assert clock.pending is handoff
    state.update(_lobby=view, _battle=view)
    assert native.focus == 'vehicle', (native.focus, native.events)
    assert native.events == ['release', 'leave', 'set-done']
    assert not change(chat, False) and native.events == ['release', 'leave', 'set-done']
    assert native.chat_document  # Matches the post-close TSF residue despite vehicle focus.
    clock.run()
    assert not native.chat_document and native.events == ['release', 'leave', 'set-done', 'handoff']
    assert native.token == 'gameplay' and clock.pending is None
    assert change(chat, True)
    view.generation = 4
    def close_during_sample():
        snapshot = View.as_snapshot(view)
        assert change(chat, False)
        return snapshot
    view.as_snapshot = close_during_sample
    clock.run()
    del view.as_snapshot
    assert state['_binding'] is None and native.token == 'gameplay'
    clock.run()
    assert clock.pending is None
    view.generation = 0
    change(chat, True)
    assert native.token == 0 and clock.pending
    # A release callback can sample while the stock chat still reports open.
    view.generation = 4
    refresh()
    last_token = native.token
    release = native.ImeGameplay
    errors = []
    state['_error'] = errors.append

    samples = []
    def reentrant_release():
        refresh()
        samples.append((native.token, commit(last_token, *edit)))
        release()
        raise RuntimeError('native release error')

    native.ImeGameplay = reentrant_release
    assert change(chat, False) and native.focus == 'vehicle'
    clock.run()
    assert clock.pending is None
    assert samples == [(0, 0)]
    assert errors == ['Could not release battle input bridge']
    native.ImeGameplay = release

    # Reconcile to the actual stock flag if the stock method throws mid-transition.
    stock_error = ValueError('stock focus error')
    for changed_flag in (False, True):
        assert change(chat, True) or chat.isFocused()
        def interrupted_close(owner, value):
            if changed_flag:
                stock_mode(owner, value)
            raise stock_error
        try:
            state['_watch_focus'](interrupted_close)(chat, False)
            assert False, 'lost stock exception'
        except ValueError as error:
            assert error is stock_error
        assert bool(clock.pending) == chat.isFocused() == (not changed_flag)
        assert native.focus == ('vehicle' if changed_flag else 'ime')

    def interrupted_open(owner, value):
        owner.opened = value  # Stock setFocused writes the flag before calling Flash.
        raise stock_error

    try:
        state['_watch_focus'](interrupted_open)(chat, True)
        assert False, 'lost stock exception'
    except ValueError as error:
        assert error is stock_error
    assert native.focus == 'vehicle' and clock.pending is None
    mode(chat, True)  # A later stock focusReceived can finish entering chat.
    assert native.focus == 'ime' and clock.pending

    # Another GUI consumer can outlive chat. A late consumer can also arrive before the next frame.
    for consumer in ('map', 'chat', None):
        if consumer == 'map':
            chat.consumers.add(consumer)  # Retains its control throughout the stock close.
        assert change(chat, False)
        if consumer == 'chat':
            chat.consumers.add(consumer)  # Late GUI ownership at the time of the callback.
        clock.forced_gui = consumer is None
        native.events[:] = []
        clock.run()
        assert native.events == [] and clock.pending is None
        chat.consumers.clear()
        clock.forced_gui = False
        assert change(chat, True)

    # A dequeued callback cannot touch a reopened chat, or cancel the newer timer.
    assert change(chat, False)
    stale = clock.pending
    def reopen(owner, value):
        stale()  # Already dequeued when the new stock open starts.
        assert native.events[-1] == 'set-done' and native.chat_document
        return stock_set_focused(owner, value)
    assert state['_watch_focus'](reopen, before_open=True)(chat, True)
    new_tick = clock.pending
    native.events[:] = []
    stale()
    assert native.events == [] and clock.pending is new_tick
    assert change(chat, False)
    stale = clock.pending
    view.generation = 0
    state['_on_dispose'](chat)
    lobby_tick = clock.pending
    assert native.token == 0 and lobby_tick
    assert native.events[-2:] == ['end', 'attach']
    stale()
    assert clock.pending is lobby_tick and 'handoff' not in native.events

    # At entry the input manager may not yet exist. Retry once, before stock takes chat focus.
    chat.app = None
    native.chat_document = False  # Simulate the new battle's clean driving focus.
    state['_on_populate'](chat)
    assert state['_battle_baseline'] is None
    chat.app = chat
    state['_battle'] = view
    native.events[:] = []
    assert change(chat, True)
    assert native.events == ['baseline', 'enter', 'set-done', 'attach']
    assert change(chat, False)
    clock.run()
    assert change(chat, True)
    assert native.events.count('baseline') == 1

    # A failed retry is terminal for this battle. fini cancels the last pending handoff.
    state['_on_dispose'](chat)
    chat.opened = False
    chat.consumers.clear()
    native.events[:] = []
    def unavailable():
        native.events.append('no-baseline')
        return False
    native.ImeBattleBaseline = unavailable
    state['_on_populate'](chat)
    state['_battle'] = view
    for i in range(2):
        assert change(chat, True) and change(chat, False)
        assert state['_battle_baseline'] is False and clock.pending is None
    assert native.events.count('no-baseline') == 2
    # Leave a real handoff queued in a fresh battle for the shutdown case.
    state['_on_dispose'](chat)
    native.chat_document = False
    del native.ImeBattleBaseline
    state['_on_populate'](chat)
    state['_battle'] = view
    assert change(chat, True) and change(chat, False)
    stale = clock.pending
    native.ImeStop = lambda: native.events.append('stop')
    state.update(_listening=False, _focus_hooks=[], BattleMessengerView=type('Hooks', (), {
        '_populate': lambda self: None, '_dispose': lambda self: None}))
    state['fini']()
    assert native.events[-2:] == ['end', 'stop']
    native.events[:] = []
    stale()
    assert clock.pending is None and native.events == []


def test_handoff_retry():
    state, native, view, clock = new_fixture()
    chat = BattleChat()
    state['_on_populate'](chat)
    state['_battle'] = view
    focus = state['_watch_focus'](lambda owner, value: setattr(owner, 'opened', value), before_open=True)
    def close_with(*outcomes):
        results = list(outcomes)
        native.ImeBattleHandoff = lambda: results.pop(0)
        focus(chat, True)
        focus(chat, False)
        assert clock.pending and clock.delay == 0
        clock.run()
        return results

    for last in ('restored-chat', 'chat-reclaimed', 'retry-chat'):
        results = close_with('retry-chat', last)
        assert clock.pending and clock.delay == 0.05 and results == [last]
        clock.run()
        assert not results and clock.pending is None
    for terminal in ('already-clear', 'replaced-foreign', 'replaced-context'):
        assert not close_with(terminal) and clock.pending is None

    for interrupt in ('map', 'forced', 'reopen', 'dispose'):
        results = close_with('retry-chat', 'restored-chat')
        stale = clock.pending
        if interrupt == 'map':
            chat.consumers.add('map')
            clock.run()
            chat.consumers.clear()
        elif interrupt == 'forced':
            clock.forced_gui = True
            clock.run()
            clock.forced_gui = False
        else:
            if interrupt == 'reopen':
                focus(chat, True)
            else:
                state['_on_dispose'](chat)
            active_tick = clock.pending
            assert active_tick and active_tick is not stale
            stale()
            assert clock.pending is active_tick
        assert results == ['restored-chat']
        if interrupt in ('map', 'forced'):
            assert clock.pending is None


if __name__ == '__main__':
    test_snapshots()
    test_routes()
    test_handoff_retry()
    print('Battle baseline, deferred handoff, stale events and candidate generations passed')
elif '__name__' not in globals():
    # The original local client-bytecode smoke loads this script with execfile(path, {}).
    state, native, view, clock = new_fixture()
