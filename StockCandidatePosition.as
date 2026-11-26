package {
    import IME_fla.MainTimeline;
    import flash.geom.Point;
    import flash.geom.Rectangle;
    import flash.system.ApplicationDomain;
    import flash.text.TextField;
    import flash.text.TextFieldType;

    // Scaleform supplies SendIMEMessage on the loaded movie instance.
    public dynamic class StockCandidatePosition extends MainTimeline {
        private var reportedCorrection:Boolean;

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
                        // Require the original host anchor to miss the input and its inverse to hit it.
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
