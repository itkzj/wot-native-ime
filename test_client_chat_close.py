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
    keyboard = client_class(archive, 'messenger/gui/Scaleform/battle_entry', 'BattleEntry')
    keys = types.ModuleType('Keys')
    exec marshal.loads(archive.read('scripts/client/Keys.pyc')[8:]) in keys.__dict__

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


# Apply the same wrappers registered by the mod, including the real Enter entry point.
methods = Chat()
for name in ('setFocused', '__setGuiMode', 'handleEnterPressed', 'sendMessageToChannel'):
    fn = types.FunctionType(child_code(messenger, name), world)
    setattr(methods, '_BattleMessengerView' + name if name.startswith('__') else name, fn)
for name, original, hook in state['_chat_hooks'](methods):
    setattr(chat, name, types.MethodType(hook, chat, Chat))
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
# Model the case where the document read back after clearing is the same chat.
# Focus identity is checked in the native test. Here we exercise the stock close
# path and the frame callbacks that must finish this handoff before polling stops.
results = iter(('retry-chat', 'restored-chat'))
def reselected_chat():
    result = next(results)
    if result == 'restored-chat':
        assert handoff() == result
    return result

native.ImeBattleHandoff = reselected_chat
assert chat.setFocused(True) and chat.setFocused(False)
clock.run()
assert native.focus == 'stock-chat' and clock.pending is not None, 'chat focus left without a scheduled handoff'
clock.run()
assert native.focus == 'vehicle' and clock.pending is None
native.ImeBattleHandoff = handoff

# A second GUI consumer may claim input between the stock close and our callback.
assert chat.setFocused(True) and chat.setFocused(False)
game_input._BattleGameInputMgr__consumers.add('map')
chat.isForcedGuiControlMode = lambda: False
assert chat.hasGuiControlModeConsumers('map') and not chat.hasGuiControlModeConsumers()
clock.run()
assert native.focus == 'stock-chat' and clock.pending is None
# Battle loading can precede a usable driving baseline. Keyboard Enter calls
# as_enterPressedS directly; the Flash endpoint calls back into focusReceived.
state['_on_dispose'](chat)
game_input._BattleGameInputMgr__consumers.clear()
chat._BattleMessengerView__isEnabled = True
chat.isModalViewShown = lambda: False
world['_CONSUMERS_LOCK_ENTER'] = ('map',)
chat._BattleMessengerView__receivers = [(123,)]
chat._BattleMessengerView__canSendMessage = lambda cid: True
chat._BattleMessengerView__getController = lambda cid: chat
messages = []
chat.sendMessage = messages.append

def flash_enter(receiver):
    if not chat.isFocused():
        chat.focusReceived()
    elif chat.sendMessageToChannel(receiver, u'\u4f60'):
        # Flash disables the field and returns stage focus before calling focusLostS.
        assert native.focus == 'stock-chat', 'IME still owns the field when Flash disables it after Enter'
        chat.focusLost()

chat.as_enterPressedS = flash_enter
controls = Chat()
controls._BattleEntry__view = lambda: chat
for name in ('handleKey', 'isFocused', '__handleEnterPressed', '__setFocused'):
    bind(controls, keyboard, name)
world.update(Keys=keys, BigWorld=clock, CTRL_MODE_NAME=controls)
controls.KILL_CAM = 'killCam'
clock.isKeyDown = lambda key: False
chat.inputHandler = None

def press(key):
    event = Chat()
    event.key = key
    event.isKeyDown = lambda: True
    event.isAltDown = lambda: False
    return controls.handleKey(event)

native.focus = 'loading'
state['_on_populate'](chat)
state['_battle'] = view
assert not state['_battle_baseline']
native.focus = 'vehicle'
assert press(keys.KEY_RETURN) and chat.isFocused()
chat._BattleMessengerView__canSendMessage = lambda cid: False
assert press(keys.KEY_RETURN) and chat.isFocused()
assert not messages and native.focus == 'ime'
chat._BattleMessengerView__canSendMessage = lambda cid: True
assert press(keys.KEY_RETURN) and not chat.isFocused()
assert messages == [u'\u4f60'] and native.focus == 'stock-chat'
assert clock.pending is not None, 'Enter sent the message but left chat focus without a handoff'
clock.run()
assert native.focus == 'vehicle' and clock.pending is None
# Cancel through setFocused(False) without calling the channel send endpoint.
assert press(keys.KEY_RETURN) and press(keys.KEY_ESCAPE)
clock.run()
assert messages == [u'\u4f60'] and native.focus == 'vehicle' and clock.pending is None
# If Flash keeps the field open after an accepted send, the next frame resumes editing.
assert press(keys.KEY_RETURN) and chat.sendMessageToChannel(0, u'abc')
clock.run()
assert chat.isFocused() and native.focus == 'ime' and clock.pending is not None
print('CN client bytecode passed accepted and refused Enter sends, cancel, retained chat and focus handoff with simulated endpoints')
