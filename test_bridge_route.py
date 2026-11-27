# -*- coding: utf-8 -*-
"""Exercise the Python 2.7 bridge outside the game with sampled Scaleform states."""
import ast


class View(object):
    def __init__(self):
        self.flashObject = self
        self.generation = 0
        self.pages = []

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
    events = None

    def ImeGameplay(self):
        if self.events is not None:
            self.events.append('release')
        if self.saved is not None:
            self.focus, self.saved = self.saved, None
        self.token = 'gameplay'
        return True

    def ImeStart(self, refresh, commit):
        if self.events is not None:
            self.events.append('attach')
        if self.saved is None:
            self.saved, self.focus = self.focus, 'ime'
        self.refresh = refresh
        return True

    def ImeTick(self):
        self.refresh()

    def ImeStatus(self):
        return 2 if self.token else 1

    def ImeCandidates(self):
        return (-1, [], u'')

    def ImeClear(self):
        self.token = 0


view, native = View(), Native()
state = dict(__builtins__=__builtins__, _battle_owner=None, _lobby=view, _library=native, _binding=None, _token=0,
             _enabled=True, _candidate_display=None, _EMPTY_CANDIDATES=(-1, [], u''))
# Load the production callbacks without importing the unavailable game modules.
with open('mod_wot_native_ime.py', 'rb') as source:
    module = ast.parse(source.read(), source.name)
module.body = [node for node in module.body if isinstance(node, ast.FunctionDef)]
exec compile(module, source.name, 'exec') in state
refresh, commit, candidates = (state[key] for key in ('_refresh', '_commit', '_update_candidates'))
edit = (u'a', 1, 1, False, u'ab', 2, 2, False)
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
class BattleChat(object):
    opened = False

    def isFocused(self):
        return self.opened


class Clock(object):
    pending = None

    def callback(self, delay, callback):
        assert self.pending is None
        self.pending = callback
        return 1

    def cancelCallback(self, handle):
        assert handle == 1 and self.pending
        self.pending = None


clock = Clock()
chat = BattleChat()
state.update(_battle_owner=chat, _battle=view, _gameplay_owner=None, _focus_call=None,
             BigWorld=clock, _status=None, _tick_id=None, _route_epoch=0,
             _status_labels={1: 'stock', 2: 'editing', 5: 'direct'})
def stock_mode(owner, value):
    owner.opened = value
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

change = state['_watch_focus'](stock_set_focused)
native.events = []
state['_switch_route']()  # Enter battle with chat closed, even with a stale Flash snapshot.
assert native.token == 'gameplay' and state['_binding'] is None and clock.pending is None
native.events[:] = []
assert change(chat, True)
assert native.events == ['enter', 'set-done', 'attach']
assert clock.pending and native.token != 'gameplay' and commit(native.token, *edit)
view.generation = 0
refresh()  # Open chat that is being handled by the stock input method.
assert native.token == 0
native.events[:] = []
assert change(chat, False)
assert native.focus == 'vehicle', (native.focus, native.events)
assert native.events == ['release', 'leave', 'set-done']
assert not change(chat, False) and native.events == ['release', 'leave', 'set-done']
assert native.token == 'gameplay' and clock.pending is None
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
assert change(chat, False) and native.focus == 'vehicle' and clock.pending is None
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

view.generation = 0
state['_battle_owner'] = None
state['_switch_route']()
assert native.token == 0 and clock.pending
print('Stock handoff, battle chat open/closed routing and candidate generations passed')
