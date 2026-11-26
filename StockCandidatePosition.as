package {
    import IME_fla.MainTimeline;
    import flash.display.MovieClip;
    import flash.text.TextFieldAutoSize;
    import flash.text.TextFormat;
    import flash.geom.Point;
    import flash.geom.Rectangle;
    import flash.system.ApplicationDomain;
    import flash.text.TextField;
    import flash.text.TextFieldType;

    // Scaleform supplies SendIMEMessage on the loaded movie instance.
    public dynamic class StockCandidatePosition extends MainTimeline {
        private var reportedCorrection:Boolean;
        private const rowFormat:TextFormat = CandidateSkin.format();

        override public function DisplayCallback(selected:Number):* {
            // SetList has already filled the rows and applied the selection. This callback owns layout.
            if (!list_mc) return;
            var width:Number = 100;
            var limit:Number = stage && stage.scaleX > 0 ? stage.stageWidth / stage.scaleX - 8 : Number.POSITIVE_INFINITY;
            var y:Number = 3;
            var selectedY:Number = 0;
            var selectedHeight:Number = 0;
            var rows:Array = list_mc.skinRows || (list_mc.skinRows = []);
            for (var i:int = 0; i < list_mc.numChildren; ++i) {
                var row:MovieClip = list_mc.getChildByName("Character" + i) as MovieClip;
                if (!row || !row.visible) continue;
                var number:TextField = row.ID as TextField;
                var value:TextField = row.Value as TextField;
                if (rows[i] !== row) {
                    rows[i] = row;
                    value.embedFonts = number.embedFonts = false;
                    value.autoSize = number.autoSize = TextFieldAutoSize.LEFT;
                    value.defaultTextFormat = number.defaultTextFormat = rowFormat;
                    number.x = 8;
                }
                // The stock selection handler reapplies its own text format before SetList calls us.
                number.setTextFormat(rowFormat);
                value.setTextFormat(rowFormat);
                number.alpha = value.text.length ? 1 : 0.25;
                if (value.wordWrap) value.wordWrap = false;
                value.x = number.x + number.width + 2;
                var available:Number = Math.max(24, limit - value.x - 8);
                if (value.width > available) {
                    value.multiline = true;
                    value.wordWrap = true;
                    value.width = available;
                }
                width = Math.max(width, Math.ceil(value.x + value.width + 8));
                var h:Number = CandidateSkin.rowHeight(Math.max(value.height, number.height));
                row.y = y;
                number.y = value.y = Math.floor((h - value.height) / 2);
                if (i == selected) { selectedY = y; selectedHeight = h; }
                // Keep the original row, click surface and controller; paint behind them.
                row.bkGround_mc.alpha = row.RowNumberBackground_mc.alpha = 0;
                if (row.bkGround_mc.height != h) row.bkGround_mc.height = h;
                y += h;
            }
            for each (row in rows) {
                if (row && row.visible && row.bkGround_mc.width != width) row.bkGround_mc.width = width;
            }
            candListHeight = y > 3 ? y + 3 : 0;
            if (list_mc.skinWidth !== width || list_mc.skinHeight !== candListHeight ||
                list_mc.skinY !== selectedY || list_mc.skinHighlight !== selectedHeight) {
                CandidateSkin.paint(list_mc.graphics, width, candListHeight, selectedY, selectedHeight);
                list_mc.skinWidth = width;
                list_mc.skinHeight = candListHeight;
                list_mc.skinY = selectedY;
                list_mc.skinHighlight = selectedHeight;
            }
            isCandListOpen = true;
        }

        override public function RepositionWindow(x:Number, bottom:Number, readingHeight:Number,
                                                  top:Number, width:Number, height:Number):Boolean {
            if (!stage || !list_mc || (stage.scaleX == 1 && stage.scaleY == 1))
                return super.RepositionWindow(x, bottom, readingHeight, top, width, height);

            var anchor:Point;
            var upper:Point;
            var viewport:Rectangle;
            try {
                var sx:Number = stage.scaleX;
                var sy:Number = stage.scaleY;
                if (sx > 0 && sy > 0 && isFinite(sx) && isFinite(sy)) {
                    var domain:ApplicationDomain = loaderInfo.applicationDomain;
                    var field:TextField = stage.focus as TextField;
                    if (!field && domain.hasDefinition("scaleform.gfx.FocusManager")) {
                        var manager:Object = domain.getDefinition("scaleform.gfx.FocusManager");
                        field = manager.getFocus(0) as TextField;
                    }
                    if (field && field.stage === stage && field.type == TextFieldType.INPUT) {
                        var bounds:Rectangle = field.getBounds(this);
                        var rawMid:Number = (bottom + top) / 2;
                        anchor = stageToList(x / sx, bottom / sy);
                        upper = stageToList(x / sx, top / sy);
                        // A long inline composition can extend past a single-line field before it scrolls.
                        // Accept its clipped x only when the y coordinates independently identify this line.
                        if ((!field.multiline || field.numLines == 1) && isFinite(anchor.x) &&
                            (rawMid < bounds.top || rawMid > bounds.bottom)) {
                            anchor.x = Math.max(bounds.left, Math.min(anchor.x, bounds.right));
                        }
                        if (!includesCaret(bounds, x, rawMid) && includesCaret(bounds, anchor.x, (anchor.y + upper.y) / 2)) {
                            // WoT's AbstractApplication.as_updateStage(w,h,s) stores round(w/s),
                            // round(h/s) in App and sets both Stage scales to s.
                            var appDomain:ApplicationDomain = field.root.loaderInfo.applicationDomain;
                            var app:Object = appDomain.hasDefinition("App") ? appDomain.getDefinition("App") : null;
                            var w:Number = app && app.instance && app.stage === stage ? app.appWidth : width / sx;
                            var h:Number = app && app.instance && app.stage === stage ? app.appHeight : height / sy;
                            var a:Point = stageToList(0, 0);
                            var b:Point = stageToList(w, h);
                            if (isFinite(a.x) && isFinite(a.y) && isFinite(b.x) && isFinite(b.y) && b.x > a.x && b.y > a.y)
                                viewport = new Rectangle(a.x, a.y, b.x - a.x, b.y - a.y);
                        }
                    }
                }
            } catch (error:Error) {
                // Some movies do not expose the focused field or its application domain.
            }
            if (!viewport) return super.RepositionWindow(x, bottom, readingHeight, top, width, height);
            var above:Boolean = super.RepositionWindow(anchor.x, anchor.y, readingHeight, upper.y, viewport.right, viewport.bottom);
            var panel:Rectangle = list_mc.getBounds(this);
            list_mc.x += Math.max(viewport.left, Math.min(panel.left, viewport.right - panel.width)) - panel.left;
            list_mc.y += Math.max(viewport.top, Math.min(panel.top, viewport.bottom - panel.height)) - panel.top;
            if (!reportedCorrection) {
                reportedCorrection = true;
                trace("[WotNativeIme] stock candidate position corrected");
            }
            return above;
        }

        private function includesCaret(bounds:Rectangle, x:Number, y:Number):Boolean {
            return bounds.width > 0 && bounds.height > 0 &&
                   x >= bounds.left && x <= bounds.right && y >= bounds.top && y <= bounds.bottom;
        }

        private function stageToList(x:Number, y:Number):Point {
            return globalToLocal(stage.localToGlobal(new Point(x, y)));
        }
    }
}
