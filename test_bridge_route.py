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
            '_active', '_refresh', '_commit', '_update_candidates', '_text'):
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


chat = BattleChat()
state.update(_battle_owner=chat, _battle=view)
refresh()  # Includes a stale Flash snapshot on the first frame after chat closes.
assert native.token == 'gameplay' and state['_binding'] is None
chat.opened = True
refresh()
assert native.token and native.token != 'gameplay' and commit(native.token, *edit)
view.generation = 0
refresh()  # Open chat that is being handled by the stock input method.
assert native.token == 0
chat.opened = False
refresh()
assert native.token == 'gameplay'
state['_battle_owner'] = None
refresh()
assert native.token == 0
print('Stock handoff, battle chat open/closed routing and candidate generations passed')
