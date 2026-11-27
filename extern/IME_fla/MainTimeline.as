package IME_fla {
    import flash.display.MovieClip;

    // Compile-time signature of the client's res/gui/flash/ime.swf. Never embed this stub.
    public dynamic class MainTimeline extends MovieClip {
        public var list_mc:MovieClip;
        public var candListHeight:Number;
        public var isCandListOpen:Boolean;
        public function CreateList(count:Number, first:Number):* { return undefined; }
        public function SetCandidateListProps(color:Number, selectedColor:Number, background:Number,
                selectedBackground:Number, size:Number, indexBackground:Number, selectedIndexBackground:Number):* { return undefined; }
        public function DisplayCallback(selected:Number):* { return undefined; }
        public function RepositionWindow(x:Number, bottom:Number, readingHeight:Number,
                                         top:Number, width:Number, height:Number):Boolean {
            return false;
        }
    }
}
