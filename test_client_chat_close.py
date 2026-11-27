"""Replay CN 2.4.2.0 battle focus methods with simulated Flash and TSF endpoints."""
import marshal
import sys
import types
from zipfile import ZipFile

from test_bridge_route import new_fixture


class Chat(object):
    pass


def ignore(*args):
    pass


def child_code(parent, name):
    return next(c for c in parent.co_consts if isinstance(c, types.CodeType) and c.co_name == name)


def client_class(archive, module, name):
    path = 'scripts/client/' + module + '.pyc'
    return child_code(marshal.loads(archive.read(path)[8:]), name)


with ZipFile(sys.argv[1]) as archive:
    messenger = client_class(archive, 'messenger/gui/Scaleform/view/battle/messenger_view', 'BattleMessengerView')
    input_mgr = client_class(archive, 'gui/Scaleform/managers/battle_input', 'BattleGameInputMgr')
    entry = client_class(archive, 'gui/Scaleform/battle_entry', 'BattleEntry')

state, native, view, clock = new_fixture()
chat = Chat()
chat.protoBwChat2 = chat.voipController = chat
chat.setMicrophoneMute = ignore
world = dict(__builtins__=__builtins__, LOG_DEBUG=ignore, protoBwChat2=chat, isPlayerAvatar=lambda: True, avatar_getter=chat)


def bind(obj, cls, name, wrapper=None):
    fn = types.FunctionType(child_code(cls, name), world)
    if wrapper:
        fn = wrapper(fn)
    attr = '_' + cls.co_name + name if name.startswith('__') else name
    setattr(obj, attr, types.MethodType(fn, obj, type(obj)))


watch = state['_watch_focus']
bind(chat, messenger, 'setFocused', lambda fn: watch(fn, before_open=True))
bind(chat, messenger, '__setGuiMode', watch)
for name in ('isFocused', 'focusLost', 'focusReceived'):
    bind(chat, messenger, name)
chat.as_setFocusS = chat.focusReceived
chat.as_unSetFocusS = chat.focusLost
chat.app = chat.sessionProvider = chat.shared = chat
chat.calloutCtrl = None
chat._BattleMessengerView__findReceiverIndexByModifiers = chat.as_changeReceiverS = ignore
chat._BattleMessengerView__receiverIndex = 0
chat._BattleMessengerView__isFocused = False
game_input = chat._BattleEntry__input = Chat()
game_input._BattleGameInputMgr__consumers = set()
bind(game_input, input_mgr, 'hasGuiControlModeConsumers')
bind(game_input, input_mgr, 'leaveGuiControlMode')
bind(chat, entry, 'hasGuiControlModeConsumers')
chat.leaveGuiControlMode = game_input.leaveGuiControlMode
chat.isForcedGuiControlMode = lambda: bool(game_input._BattleGameInputMgr__consumers)


def enter(consumer):
    game_input._BattleGameInputMgr__consumers.add(consumer)
    native.focus = 'stock-chat'


def leave(value):
    assert value is False and native.focus == 'stock-chat', native.focus
    # The stock GUI released control; the same chat document is still focused.
    chat.vehicle_control = True


def handoff():
    assert not game_input._BattleGameInputMgr__consumers and chat.vehicle_control
    assert native.focus == 'stock-chat', native.focus
    native.focus = 'vehicle'
    return 'restored-chat'


chat.enterGuiControlMode = enter
chat.setForcedGuiControlMode = leave
clock.player = lambda: chat
native.focus = 'vehicle'
native.events = []
native.ImeBattleBaseline = lambda: native.focus == 'vehicle'
native.ImeBattleHandoff = handoff
state['_on_populate'](chat)
state['_battle'] = view
for i in range(3):
    chat.vehicle_control = False
    assert chat.setFocused(True) and chat.isFocused() and native.focus == 'ime'
    assert chat.setFocused(False) and not chat.isFocused()
    clock.run()
    assert native.focus == 'vehicle' and not game_input._BattleGameInputMgr__consumers
    assert clock.pending is None
# A second GUI consumer may claim input between the stock close and our callback.
assert chat.setFocused(True) and chat.setFocused(False)
game_input._BattleGameInputMgr__consumers.add('map')
chat.isForcedGuiControlMode = lambda: False
assert chat.hasGuiControlModeConsumers('map') and not chat.hasGuiControlModeConsumers()
clock.run()
assert native.focus == 'stock-chat' and clock.pending is None
print('CN client focus bytecode passed three chat cycles and a late GUI owner with simulated Flash and native endpoints')
