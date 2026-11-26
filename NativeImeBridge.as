package {
    import flash.display.DisplayObject;
    import flash.display.DisplayObjectContainer;
    import flash.display.Loader;
    import flash.display.MovieClip;
    import flash.display.Stage;
    import flash.events.Event;
    import flash.events.FocusEvent;
    import flash.geom.Point;
    import flash.geom.Rectangle;
    import flash.system.ApplicationDomain;
    import flash.text.TextField;
    import flash.text.TextFieldType;
    import flash.text.TextLineMetrics;
    import net.wg.infrastructure.base.AbstractView;

    public class NativeImeBridge extends AbstractView {
        private static const STOCK:Object = {
            "net.wg.gui.messenger.ChannelComponent": "messageInput",
            "net.wg.gui.messenger.controls.ContactsTreeComponent": "searchInput",
            "net.wg.gui.messenger.forms.ChannelsSearchForm": "searchNameInput",
            "net.wg.gui.messenger.forms.ChannelsCreateForm": "channelNameInput",
            "net.wg.gui.messenger.views.SearchContactView": "searchInput",
            "net.wg.gui.messenger.forms.ContactsSearchForm": "textInput",
            "net.wg.gui.messenger.views.BaseManageContactView": "input"
        };
        private static const BATTLE:String = "net.wg.gui.battle.views.battleMessenger.BattleMessenger";
        private var domain:ApplicationDomain;
        private var app:Object;
        private var getFocus:Function;
        private var field:TextField;
        private var generation:uint;
        private var battle:Boolean;
        private var bridgeDetached:Boolean;
        private var candidatePanel:CandidatePanel;

        public function NativeImeBridge() {
            mouseEnabled = false;
            mouseChildren = false;
            tabEnabled = false;
            addEventListener(Event.ADDED_TO_STAGE, bridgeStageChanged);
            addEventListener(Event.REMOVED_FROM_STAGE, bridgeStageChanged);
        }

        public function as_start(inBattle:Boolean):void {
            battle = inBattle;
            domain = loaderInfo.applicationDomain;
            if (domain.hasDefinition("App")) app = domain.getDefinition("App");
            if (domain.hasDefinition("scaleform.gfx.FocusManager")) {
                var manager:Object = domain.getDefinition("scaleform.gfx.FocusManager");
                getFocus = manager.getFocus;
            }
        }

        public function as_stop():void {
            domain = null;
            app = null;
            getFocus = null;
            setField(null);
        }

        private function bridgeStageChanged(event:Event):void {
            if (event.target !== this) return;
            bridgeDetached = event.type == Event.REMOVED_FROM_STAGE;
            if (bridgeDetached) setField(null);
        }

        private function stockUnsupported(movie:Stage):Boolean {
            // The named child is the Loader. SetLastIME writes the tab on its content timeline.
            try {
                var ime:Object = app && app.instance && app.utils ? app.utils.IME : null;
                var container:DisplayObjectContainer = ime ? ime.getContainer() as DisplayObjectContainer : null;
                if (!container || container.stage !== movie) return false;
                var loader:Loader = container.getChildByName("LangBar") as Loader;
                var timeline:MovieClip = loader ? loader.content as MovieClip : null;
                var tab:MovieClip = timeline ? timeline.IMENameTab_mc as MovieClip : null;
                var label:TextField = tab ? tab.tf as TextField : null;
                return label != null && label.text === "Not Supported";
            } catch (error:Error) {
                // Loading or disposing the stock view can make its timeline inaccessible.
            }
            return false;
        }

        private function setField(next:TextField):void {
            if (next === field) return;
            hideCandidates();
            if (field) {
                field.removeEventListener(FocusEvent.FOCUS_OUT, lostField);
                field.removeEventListener(Event.REMOVED_FROM_STAGE, lostField);
            }
            field = next;
            if (field) {
                field.addEventListener(FocusEvent.FOCUS_OUT, lostField, false, 0, true);
                field.addEventListener(Event.REMOVED_FROM_STAGE, lostField, false, 0, true);
            }
            if (++generation == 0) ++generation;
        }

        private function lostField(event:Event):void {
            if (event.target === field) setField(null);
        }

        private function ownerHas(node:Object, cls:String, slot:String, target:DisplayObject):Boolean {
            if (!(slot in node) || !domain.hasDefinition(cls) || node[slot] !== target) return false;
            var type:Class = domain.getDefinition(cls) as Class;
            return node is type;
        }

        private function shouldTakeOver(target:TextField, movie:Stage):Boolean {
            if (!target || target.stage !== movie || target.type != TextFieldType.INPUT ||
                target.displayAsPassword || target.restrict != null || !target.parent) return false;
            var input:Object = target.parent;
            if (!battle && (!("textField" in input) || input.textField !== target ||
                !("editable" in input) || !input.editable || !input.enabled)) return false;
            if (!stockUnsupported(movie)) return false;
            var node:DisplayObject;
            for (node = target; node; node = node.parent) if (!node.visible || node.alpha <= 0) return false;
            for (node = target.parent; node; node = node.parent) {
                if (battle) {
                    if (ownerHas(node, BATTLE, "messageInputField", target)) return true;
                } else {
                    for (var name:String in STOCK) if (ownerHas(node, name, STOCK[name], target.parent)) return true;
                }
            }
            return false;
        }

        private function inputStage():Stage {
            return battle ? stage : app && app.instance ? app.stage as Stage : stage;
        }

        private function focused():Stage {
            var movie:Stage = inputStage();
            var target:TextField = !bridgeDetached && movie && domain ?
                (getFocus != null ? getFocus(0) : movie.focus) as TextField : null;
            setField(shouldTakeOver(target, movie) ? target : null);
            return movie;
        }

        private function caretLocal(index:int):Rectangle {
            var line:int = index < field.length ? field.getLineIndexOfChar(index) : field.numLines - 1;
            if (line < 0 || line >= field.numLines) return null;
            var charBox:Rectangle = index < field.length ? field.getCharBoundaries(index) : null;
            if (charBox && charBox.height > 0 && charBox.width > 0)
                return new Rectangle(charBox.left, charBox.top, 1, charBox.height);
            var lineStart:int = field.getLineOffset(line);
            if (index > lineStart) {
                charBox = field.getCharBoundaries(index - 1);
                if (charBox && charBox.height > 0 && charBox.width > 0)
                    return new Rectangle(charBox.right, charBox.top, 1, charBox.height);
            }
            var metrics:TextLineMetrics = field.getLineMetrics(line);
            var y:Number = 2;
            for (var i:int = field.scrollV - 1; i < line; ++i) y += field.getLineMetrics(i).height;
            return new Rectangle(metrics.x + 2 - field.scrollH, y, 1, metrics.height);
        }

        private function normalized(value:Number, total:Number):int {
            return Math.round(10000 * Math.max(0, Math.min(value, total)) / total);
        }

        private function movieViewport(movie:Stage):Rectangle {
            var lobby:Boolean = !battle && app && app.instance;
            var width:Number = lobby ? app.appWidth : movie.stageWidth;
            var height:Number = lobby ? app.appHeight : movie.stageHeight;
            return width > 0 && height > 0 && isFinite(width) && isFinite(height) ?
                new Rectangle(0, 0, width, height) : null;
        }

        private function visibleCaret(movie:Stage, viewport:Rectangle):Rectangle {
            if (viewport.isEmpty()) return null;
            var caret:Rectangle = caretLocal(field.caretIndex);
            if (!caret) return null;
            if (!field.multiline) {
                var bounds:Rectangle = field.getBounds(field);
                if (bounds.isEmpty()) return null;
                // Some Scaleform builds return content coordinates for a scrolled glyph.
                if (caret.x >= bounds.right && field.scrollH > 0) caret.x -= field.scrollH;
                caret.x = Math.max(bounds.left, Math.min(caret.x, bounds.right - caret.width));
            }
            var a:Point = movie.globalToLocal(field.localToGlobal(caret.topLeft));
            var b:Point = movie.globalToLocal(field.localToGlobal(caret.bottomRight));
            if (b.x <= viewport.left || a.x >= viewport.right ||
                b.y <= viewport.top || a.y >= viewport.bottom) return null;
            a.x = Math.max(a.x, viewport.left);
            a.y = Math.max(a.y, viewport.top);
            return new Rectangle(a.x, a.y, Math.min(b.x, viewport.right) - a.x,
                Math.min(b.y, viewport.bottom) - a.y);
        }

        public function as_snapshot():Array {
            var movie:Stage = focused();
            if (!field) return null;
            var screen:Rectangle = movieViewport(movie);
            var viewport:Rectangle = screen ? field.getBounds(movie).intersection(screen) : null;
            var caret:Rectangle = viewport ? visibleCaret(movie, viewport) : null;
            if (!caret) {
                hideCandidates();
                return null;
            }
            if (candidatePanel) {
                if (candidatePanel.parent === movie) candidatePanel.position(screen, caret);
                else hideCandidates();
            }
            var first:int = field.selectionBeginIndex;
            var last:int = field.selectionEndIndex;
            return [generation, field.text, first, last, first != last && field.caretIndex == first,
                normalized(caret.left, screen.width), normalized(caret.top, screen.height),
                normalized(caret.right, screen.width), normalized(caret.bottom, screen.height),
                normalized(viewport.left, screen.width), normalized(viewport.top, screen.height),
                normalized(viewport.right, screen.width), normalized(viewport.bottom, screen.height)];
        }

        public function as_candidates(expected:uint, selection:int, labels:Array, preedit:String):void {
            var movie:Stage = focused();
            if (!field || generation != expected || !labels || labels.length > 16 || !preedit && labels.length == 0 ||
                preedit && preedit.length > 8192) {
                hideCandidates();
                return;
            }
            var screen:Rectangle = movieViewport(movie);
            var caret:Rectangle = screen ? visibleCaret(movie, field.getBounds(movie).intersection(screen)) : null;
            if (!caret) {
                hideCandidates();
                return;
            }
            for each (var label:* in labels) {
                if (!(label is String) || label.length > 256) { hideCandidates(); return; }
            }
            if (!candidatePanel || candidatePanel.parent !== movie) {
                hideCandidates();
                candidatePanel = new CandidatePanel();
                movie.addChild(candidatePanel);
            }
            candidatePanel.showPage(labels, selection, preedit, screen, caret);
        }

        private function hideCandidates():void {
            if (candidatePanel && candidatePanel.parent) candidatePanel.parent.removeChild(candidatePanel);
            candidatePanel = null;
        }

        private function splitPair(value:String, index:int):Boolean {
            return index > 0 && index < value.length && value.charCodeAt(index - 1) >= 0xd800 &&
                value.charCodeAt(index - 1) <= 0xdbff && value.charCodeAt(index) >= 0xdc00 &&
                value.charCodeAt(index) <= 0xdfff;
        }

        public function as_commit(token:uint, before:String, first:int, last:int, leading:Boolean,
                                  after:String, newFirst:int, newLast:int, newLeading:Boolean):Boolean {
            focused();
            var target:TextField = field;
            if (!target || token != generation || before !== target.text || first != target.selectionBeginIndex ||
                last != target.selectionEndIndex || leading != (first != last && target.caretIndex == first) ||
                newFirst < 0 || newLast < newFirst || newLast > after.length ||
                splitPair(after, newFirst) || splitPair(after, newLast) ||
                (target.maxChars > 0 && after.length > target.maxChars)) return false;
            if (before !== after) {
                var start:int = 0;
                while (start < before.length && start < after.length && before.charCodeAt(start) == after.charCodeAt(start)) ++start;
                if (splitPair(before, start) || splitPair(after, start)) --start;
                var oldEnd:int = before.length;
                var newEnd:int = after.length;
                while (oldEnd > start && newEnd > start && before.charCodeAt(oldEnd - 1) == after.charCodeAt(newEnd - 1)) {
                    --oldEnd;
                    --newEnd;
                }
                if (splitPair(before, oldEnd) || splitPair(after, newEnd)) { ++oldEnd; ++newEnd; }
                var replacement:String = after.substring(start, newEnd);
                for (var i:int = 0; i < replacement.length; ++i) {
                    var code:int = replacement.charCodeAt(i);
                    if (code < 32 || (code >= 127 && code <= 159) || code == 0x2028 || code == 0x2029 ||
                        (code >= 0xd800 && code <= 0xdfff && !splitPair(replacement, i) && !splitPair(replacement, i + 1))) return false;
                }
                target.replaceText(start, oldEnd, replacement);
                if (target.text !== after) {
                    target.text = before;
                    target.setSelection(leading ? last : first, leading ? first : last);
                    return false;
                }
                target.dispatchEvent(new Event(Event.CHANGE, true));
                focused();
            }
            if (field === target && generation == token && target.text === after)
                target.setSelection(newLeading ? newLast : newFirst, newLeading ? newFirst : newLast);
            return true;
        }
    }
}
