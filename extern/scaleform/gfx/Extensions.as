package scaleform.gfx {
    import flash.text.TextField;

    // Build-time signature. Scaleform supplies this class at runtime.
    public class Extensions {
        public static function get isScaleform():Boolean { return false; }
        public static function setIMEEnabled(field:TextField, enabled:Boolean):void {}
    }
}
