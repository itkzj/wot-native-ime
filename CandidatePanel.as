package {
    import flash.display.Sprite;
    import flash.geom.Rectangle;
    import flash.text.TextField;
    import flash.text.TextFieldAutoSize;

    internal final class CandidatePanel extends Sprite {
        private var labels:Array;
        private var preedit:String;
        private var selectedIndex:int;
        private var layoutLimit:Number;
        private var panelWidth:Number, panelHeight:Number;
        private var highlightY:Number, highlightHeight:Number, separatorY:Number;

        public function CandidatePanel() {
            mouseEnabled = mouseChildren = false;
            tabEnabled = tabChildren = false;
        }

        public function showPage(values:Array, selection:int, composition:String, screen:Rectangle, caret:Rectangle):void {
            labels = values.concat();
            preedit = composition;
            selectedIndex = selection >= 0 && selection < labels.length ? selection : -1;
            layout(Math.max(24, screen.width - 8));
            position(screen, caret);
        }

        public function position(screen:Rectangle, caret:Rectangle):void {
            var limit:Number = Math.max(24, screen.width - 8);
            if (layoutLimit != limit) layout(limit);
            var scale:Number = Math.min(1, screen.width / panelWidth, screen.height / panelHeight);
            if (scaleX != scale) scaleX = scale;
            if (scaleY != scale) scaleY = scale;
            var w:Number = panelWidth * scale, h:Number = panelHeight * scale;
            var panelY:Number = caret.bottom + 4;
            if (panelY + h > screen.height && caret.top - 4 >= h) panelY = caret.top - 4 - h;
            var nextX:Number = Math.max(0, Math.min(caret.left, screen.width - w));
            var nextY:Number = Math.max(0, Math.min(panelY, screen.height - h));
            if (x != nextX) x = nextX;
            if (y != nextY) y = nextY;
            if (parent && parent.getChildAt(parent.numChildren - 1) !== this)
                parent.setChildIndex(this, parent.numChildren - 1);
        }

        private function layout(limit:Number):void {
            var resized:Boolean = layoutLimit != limit;
            var oldWidth:Number = panelWidth, oldHeight:Number = panelHeight;
            layoutLimit = limit;
            panelWidth = Math.min(100, limit);
            panelHeight = 3;
            var slot:int = 0;
            for (var index:int = preedit ? -1 : 0; index < labels.length; ++index, ++slot) {
                var text:TextField;
                if (slot < numChildren) text = getChildAt(slot) as TextField;
                else {
                    text = new TextField();
                    text.embedFonts = false;
                    text.selectable = false;
                    text.multiline = true;
                    text.autoSize = TextFieldAutoSize.LEFT;
                    text.x = 8;
                    addChild(text);
                }
                var role:String = index < 0 ? "preedit" : "candidate";
                var reformat:Boolean = text.name != role;
                if (reformat) {
                    text.name = role;
                    text.defaultTextFormat = CandidateSkin.format(index < 0);
                }
                var caption:String = index < 0 ? preedit : (index < 9 ? (index + 1) + ". " : "") + labels[index];
                var changed:Boolean = reformat || text.text != caption;
                if (changed || resized) {
                    if (text.wordWrap) text.wordWrap = false;
                    if (changed) text.text = caption;
                    if (text.width > limit - 16) {
                        text.wordWrap = true;
                        text.width = limit - 16;
                    }
                }
                if (text.y != panelHeight) text.y = panelHeight;
                panelWidth = Math.max(panelWidth, Math.ceil(text.width) + 16);
                panelHeight += CandidateSkin.rowHeight(text.height);
            }
            while (numChildren > slot) removeChildAt(numChildren - 1);
            panelHeight += 3;
            paint(oldWidth, oldHeight);
        }

        private function paint(oldWidth:Number, oldHeight:Number):void {
            var selected:TextField = selectedIndex < 0 ? null : getChildAt(selectedIndex + (preedit ? 1 : 0)) as TextField;
            var top:Number = selected ? selected.y : 0;
            var h:Number = selected ? CandidateSkin.rowHeight(selected.height) : 0;
            var divider:Number = 0;
            if (preedit && labels.length) {
                var header:TextField = getChildAt(0) as TextField;
                divider = header.y + CandidateSkin.rowHeight(header.height) - 1;
            }
            if (oldWidth == panelWidth && oldHeight == panelHeight &&
                highlightY == top && highlightHeight == h && separatorY == divider) return;
            highlightY = top;
            highlightHeight = h;
            separatorY = divider;
            CandidateSkin.paint(graphics, panelWidth, panelHeight, top, h);
            if (divider) {
                graphics.beginFill(0x66614B);
                graphics.drawRect(6, divider, panelWidth - 12, 1);
                graphics.endFill();
            }
        }
    }
}
