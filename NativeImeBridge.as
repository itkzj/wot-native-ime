package {
    import flash.display.DisplayObject;
    import flash.display.Sprite;
    import flash.display.Stage;
    import flash.events.Event;
    import flash.events.FocusEvent;
    import flash.events.MouseEvent;
    import flash.geom.Point;
    import flash.geom.Rectangle;
    import flash.system.ApplicationDomain;
    import flash.text.TextField;
    import flash.text.TextFieldAutoSize;
    import flash.text.TextFieldType;
    import flash.text.TextFormat;
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
        private var candidatePanel:Sprite;
        private var candidateStage:Stage;
        private var candidateLabels:Array;
        private var candidateSerial:uint;
        private var candidateSelection:int = -1;
        private var candidateLimit:Number;
        private var candidateWidth:Number;
        private var candidateHeight:Number;
        private var pressedRow:int = -1;
        private var pendingChoice:Array;

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
            if (!domain.hasDefinition(cls) || !(slot in node) || node[slot] !== target) return false;
            var type:Class = domain.getDefinition(cls) as Class;
            return node is type;
        }

        private function eligible(target:TextField, movie:Stage):Boolean {
            if (!target || target.stage !== movie || target.type != TextFieldType.INPUT ||
                target.displayAsPassword || target.restrict != null || !target.parent) return false;
            var node:DisplayObject;
            for (node = target; node; node = node.parent) if (!node.visible || node.alpha <= 0) return false;
            var input:Object = target.parent;
            if (!battle && (!("textField" in input) || input.textField !== target ||
                !("editable" in input) || !input.editable || !input.enabled)) return false;
            for (node = target.parent; node; node = node.parent) {
                if (battle) {
                    if (ownerHas(node, BATTLE, "messageInputField", target)) return true;
                } else {
                    for (var name:String in STOCK) if (ownerHas(node, name, STOCK[name], target.parent)) return true;
                }
            }
            return false;
        }

        private function focused():Stage {
            var movie:Stage = battle ? stage : app && app.instance ? app.stage as Stage : stage;
            var target:TextField = !bridgeDetached && movie && domain ?
                (getFocus != null ? getFocus(0) : movie.focus) as TextField : null;
            setField(movie && domain && eligible(target, movie) ? target : null);
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
            var caret:Rectangle = caretLocal(field.caretIndex);
            if (!caret || viewport.isEmpty()) return null;
            var a:Point = movie.globalToLocal(field.localToGlobal(caret.topLeft));
            var b:Point = movie.globalToLocal(field.localToGlobal(caret.bottomRight));
            if (a.x < viewport.left - 1 || a.x > viewport.right + 1 ||
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
            if (candidatePanel) positionCandidates(movie, screen, caret);
            var first:int = field.selectionBeginIndex;
            var last:int = field.selectionEndIndex;
            return [generation, field.text, first, last, first != last && field.caretIndex == first,
                normalized(caret.left, screen.width), normalized(caret.top, screen.height),
                normalized(caret.right, screen.width), normalized(caret.bottom, screen.height),
                normalized(viewport.left, screen.width), normalized(viewport.top, screen.height),
                normalized(viewport.right, screen.width), normalized(viewport.bottom, screen.height)];
        }

        public function as_candidates(serial:uint, selection:int, clickable:Boolean, labels:Array):void {
            var movie:Stage = focused();
            if (!field || serial == 0 || !labels || labels.length == 0 || labels.length > 16) {
                hideCandidates();
                return;
            }
            var screen:Rectangle = movieViewport(movie);
            var caret:Rectangle = screen ? visibleCaret(movie, field.getBounds(movie).intersection(screen)) : null;
            if (!caret) {
                hideCandidates();
                return;
            }
            if (serial != candidateSerial || !candidatePanel || candidatePanel.parent !== movie) {
                hideCandidates();
                for each (var label:* in labels) {
                    if (!(label is String) || label.length > 256) return;
                }
                candidateLabels = labels.concat();
                candidateSerial = serial;
                candidatePanel = new Sprite();
                candidatePanel.mouseChildren = false;
                candidatePanel.tabEnabled = false;
                candidatePanel.tabChildren = false;
                candidatePanel.addEventListener(MouseEvent.MOUSE_DOWN, candidateDown);
                candidatePanel.addEventListener(MouseEvent.CLICK, candidateClick);
                // The service view can sit under chat or be clipped. Use the focused movie's Stage.
                movie.addChild(candidatePanel);
                candidateStage = movie;
                movie.addEventListener(FocusEvent.MOUSE_FOCUS_CHANGE, preserveFieldFocus, true, 100, true);
                candidateLimit = -1;
            }
            if (!clickable) {
                pendingChoice = null;
                pressedRow = -1;
            }
            candidatePanel.mouseEnabled = clickable;
            var row:int = selection >= 0 && selection < candidateLabels.length ? selection : -1;
            if (row != candidateSelection) {
                candidateSelection = row;
                if (candidatePanel.numChildren) paintCandidates();
            }
            positionCandidates(movie, screen, caret);
        }

        public function as_takeChoice():Array {
            if (!pendingChoice) return null;
            var movie:Stage = focused();
            if (!candidatePanel || candidatePanel.parent !== movie) {
                hideCandidates();
                return null;
            }
            var choice:Array = pendingChoice;
            pendingChoice = null;
            return choice;
        }

        private function hideCandidates():void {
            if (candidateStage) candidateStage.removeEventListener(FocusEvent.MOUSE_FOCUS_CHANGE, preserveFieldFocus, true);
            var panel:Sprite = candidatePanel;
            candidatePanel = null;
            candidateStage = null;
            candidateLabels = null;
            candidateSerial = 0;
            candidateSelection = -1;
            pressedRow = -1;
            pendingChoice = null;
            if (panel && panel.parent) panel.parent.removeChild(panel);
        }

        private function positionCandidates(movie:Stage, screen:Rectangle, caret:Rectangle):void {
            if (candidatePanel.parent !== movie) {
                hideCandidates();
                return;
            }
            var limit:Number = Math.max(24, screen.width - 8);
            if (candidateLimit != limit) layoutCandidates(limit);
            var scale:Number = Math.min(1, screen.width / candidateWidth, screen.height / candidateHeight);
            candidatePanel.scaleX = candidatePanel.scaleY = scale;
            var width:Number = candidateWidth * scale;
            var height:Number = candidateHeight * scale;
            var y:Number = caret.bottom + 4;
            if (y + height > screen.height && caret.top - 4 >= height) y = caret.top - 4 - height;
            candidatePanel.x = Math.max(0, Math.min(caret.left, screen.width - width));
            candidatePanel.y = Math.max(0, Math.min(y, screen.height - height));
            if (movie.getChildIndex(candidatePanel) != movie.numChildren - 1)
                movie.setChildIndex(candidatePanel, movie.numChildren - 1);
        }

        private function layoutCandidates(limit:Number):void {
            candidateLimit = limit;
            pressedRow = -1;
            while (candidatePanel.numChildren) candidatePanel.removeChildAt(candidatePanel.numChildren - 1);
            var format:TextFormat = new TextFormat("Microsoft YaHei", 16, 0xF5F1E5);
            candidateWidth = Math.min(100, limit);
            candidateHeight = 4;
            for (var index:int = 0; index < candidateLabels.length; ++index) {
                var text:TextField = new TextField();
                text.embedFonts = false;
                text.selectable = false;
                text.multiline = true;
                text.autoSize = TextFieldAutoSize.LEFT;
                text.defaultTextFormat = format;
                text.text = candidateLabels[index];
                if (index < 9) text.text = (index + 1) + ". " + text.text;
                text.x = 8;
                text.y = candidateHeight;
                if (text.width > limit - 16) {
                    text.wordWrap = true;
                    text.width = limit - 16;
                }
                candidateWidth = Math.max(candidateWidth, Math.ceil(text.width) + 16);
                candidateHeight += Math.max(26, Math.ceil(text.height));
                candidatePanel.addChild(text);
            }
            candidateHeight += 4;
            paintCandidates();
        }

        private function paintCandidates():void {
            candidatePanel.graphics.clear();
            candidatePanel.graphics.beginFill(0x1B2619);
            candidatePanel.graphics.drawRect(0, 0, candidateWidth, candidateHeight);
            candidatePanel.graphics.endFill();
            if (candidateSelection < 0) return;
            var text:TextField = candidatePanel.getChildAt(candidateSelection) as TextField;
            var height:Number = Math.max(26, Math.ceil(text.height));
            candidatePanel.graphics.beginFill(0x384B2C);
            candidatePanel.graphics.drawRect(0, text.y, candidateWidth, height);
            candidatePanel.graphics.endFill();
            candidatePanel.graphics.beginFill(0xF7D267);
            candidatePanel.graphics.drawRect(0, text.y, 4, height);
            candidatePanel.graphics.endFill();
        }

        private function preserveFieldFocus(event:FocusEvent):void {
            if (event.target === field && candidatePanel && candidatePanel.mouseEnabled &&
                (event.relatedObject === candidatePanel || (!event.relatedObject &&
                    candidatePanel.getBounds(candidatePanel).contains(candidatePanel.mouseX, candidatePanel.mouseY)))) {
                event.preventDefault();
            }
        }

        private function candidateRow(y:Number):int {
            for (var i:int = 0; i < candidatePanel.numChildren; ++i) {
                var text:TextField = candidatePanel.getChildAt(i) as TextField;
                if (y >= text.y && y < text.y + Math.max(26, Math.ceil(text.height))) return i;
            }
            return -1;
        }

        private function candidateDown(event:MouseEvent):void {
            event.stopPropagation();
            pressedRow = candidateRow(event.localY);
        }

        private function candidateClick(event:MouseEvent):void {
            event.stopPropagation();
            var row:int = candidateRow(event.localY);
            if (!pendingChoice && row >= 0 && row == pressedRow) pendingChoice = [candidateSerial, row];
            pressedRow = -1;
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
            }
            if (target.text === after) target.setSelection(newLeading ? newLast : newFirst, newLeading ? newFirst : newLast);
            return true;
        }
    }
}
