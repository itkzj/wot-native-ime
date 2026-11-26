package {
    import flash.display.Graphics;
    import flash.display.GradientType;
    import flash.geom.Matrix;
    import flash.text.TextFormat;

    public final class CandidateSkin {
        public static function format(preedit:Boolean = false):TextFormat {
            return new TextFormat("Microsoft YaHei", 14, preedit ? 0xF1E7C8 : 0xDCD8CB, preedit);
        }

        public static function rowHeight(textHeight:Number):Number {
            return Math.max(22, Math.ceil(textHeight));
        }

        public static function paint(g:Graphics, width:Number, height:Number,
                                     selectedY:Number, selectedHeight:Number):void {
            g.clear();
            if (width <= 0 || height <= 0) return;
            var fill:Matrix = new Matrix();
            fill.createGradientBox(width, height, Math.PI / 2);
            g.beginGradientFill(GradientType.LINEAR, [0x414032, 0x23231D, 0x1B1B17], [1, 1, 1], [0, 86, 255], fill);
            g.drawRect(0, 0, width, height);
            g.endFill();
            if (selectedHeight > 0) {
                g.beginFill(0x54513E);
                g.drawRect(1, selectedY, width - 2, selectedHeight);
                g.endFill();
                g.beginFill(0xA69A69);
                g.drawRect(1, selectedY, 2, selectedHeight);
                g.endFill();
            }
            g.lineStyle(1, 0x6E6952);
            g.drawRect(0.5, 0.5, width - 1, height - 1);
            g.lineStyle();
        }
    }
}
