# -*- coding: utf-8 -*-
"""Exercise the Python 2.7 bridge outside the game with sampled Scaleform states."""
import types


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

    def ImeGameplay(self):
        self.token = 'gameplay'
        return True

    def ImeStart(self, refresh, commit):
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
    module = compile(source.read(), source.name, 'exec')
for code in module.co_consts:
    if isinstance(code, types.CodeType) and code.co_name in (
            '_active', '_refresh', '_commit', '_update_candidates', '_text', '_watch_focus', '_switch_route', '_tick'):
        state[code.co_name] = types.FunctionType(code, state)
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
state.update(_battle_owner=chat, _battle=view, BigWorld=clock, _status=None, _tick_id=None, _route_epoch=0,
             _status_labels={1: 'stock', 2: 'editing', 5: 'direct'})
change = state['_watch_focus'](lambda owner, value: setattr(owner, 'opened', value))
state['_switch_route']()  # Enter battle with chat closed, even with a stale Flash snapshot.
assert native.token == 'gameplay' and state['_binding'] is None and clock.pending is None
change(chat, True)
assert clock.pending and native.token != 'gameplay' and commit(native.token, *edit)
view.generation = 0
refresh()  # Open chat that is being handled by the stock input method.
assert native.token == 0
change(chat, False)
assert native.token == 'gameplay' and clock.pending is None
change(chat, True)
assert native.token == 0 and clock.pending
state['_battle_owner'] = None
state['_switch_route']()
assert native.token == 0 and clock.pending
print('Stock handoff, battle chat open/closed routing and candidate generations passed')
