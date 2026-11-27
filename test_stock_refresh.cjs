// Exercise the production callback after the stock SetList has filled and formatted each visible row.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const src = fs.readFileSync(__dirname + '/StockCandidatePosition.as', 'utf8');
function method(name, params) {
    const start = src.indexOf('function ' + name + '(');
    if (start < 0) return null;
    const body = src.slice(src.indexOf('{',start)+1, src.indexOf('\n        }',start))
        .replace(/(var \w+|catch \(\w+):\w+/g,'$1').replace(/ as \w+/g,'')
        .replace(/for each \(([^)]+) in ([^)]+)\)/g,'for ($1 of $2)').replace(/super\./g,'stock.');
    return new Function('env', ...params, 'with(env){' + body + '}');
}
const render = method('DisplayCallback',['selected']);
const position = method('RepositionWindow',['x','bottom','readingHeight','top','width','height']);
const props = method('SetCandidateListProps',['color','selectedColor','background','selectedBackground','size','indexBackground','selectedIndexBackground']);
const create = method('CreateList',['count','first']);
function replay(scene) {
    let formats = 0, wraps = 0, paints = 0, made = 0;
    const skin = {font:'Microsoft YaHei',color:0xdcd8cb,size:14};
    const label = text => ({text, wrap:false, get width(){return this.wrap ? this.limit : this.text.length*10+4;},
        set width(n){this.limit=n;},get height(){return this.wrap?40:18;},get wordWrap(){return this.wrap;},
        set wordWrap(b){if(this.wrap!==b)wraps++;this.wrap=b;},setTextFormat(f){formats++;this.applied=f;}});
    let normal, active, fontSize;
    const env = {rowFormat:skin, TextFieldAutoSize:{LEFT:'left'},stage:{stageWidth:320,scaleX:1},
        candListHeight:0,isCandListOpen:false,CandidateSkin:{rowHeight:n=>Math.max(n,22),paint:()=>paints++},
        stock:{SetCandidateListProps(c,s,b,sb,z){normal=c;active=s;fontSize=z;},CreateList(n){
            normal=0;active=0xffffff;fontSize=26;
            const rows=Array.from({length:n},(_,i)=>({ID:label(String(i+1)),Value:label('候选'), visible:true,
                bkGround_mc:{},RowNumberBackground_mc:{}}));
            made+=n;env.list_mc={rows,graphics:{},numChildren:n,getChildByName:k=>rows[Number(k.slice(9))]};
        }}};
    const domain = {hasDefinition:()=>false};
    const fieldY = scene==='battle' ? 718 : 318;
    const field = {type:'input',multiline:false,root:{loaderInfo:{applicationDomain:domain}},
        getBounds:()=>({left:50,right:220,top:fieldY,bottom:fieldY+18,width:170,height:18})};
    field.parent = scene==='battle' ? {messageInputField:field} : {};
    env.stage = field.stage = {focus:field,stageWidth:640,stageHeight:2000,scaleX:2,scaleY:2};
    let placements=0;
    Object.assign(env,{loaderInfo:{applicationDomain:domain},reportedCorrection:true,TextFieldType:{INPUT:'input'},
        stageToList:(x,y)=>({x,y}), includesCaret:(b,x,y)=>x>=b.left&&x<=b.right&&y>=b.top&&y<=b.bottom,
        Rectangle:class {constructor(x,y,w,h){Object.assign(this,{left:x,top:y,right:x+w,bottom:y+h});}}});
    env.stock.RepositionWindow=(x,y)=>{ placements++; env.list_mc.x=x; env.list_mc.y=y+5; return false; };
    env.SetCandidateListProps=(...args)=>(props ? props(env,...args) : env.stock.SetCandidateListProps(...args));
    (create ? create(env,5,1) : env.stock.CreateList(5,1));
    env.list_mc.getBounds=()=>({left:env.list_mc.x,top:env.list_mc.y,width:env.list_mc.skinWidth,height:env.candListHeight});
    for(let i=0;i<240;i++){
        if(i===100)env.SetCandidateListProps(1,2,3,4,26,5,6);
        const rows=env.list_mc.rows, selected=i%5;
        rows[0].Value.text=String.fromCharCode(0x4e00+i); rows[4].Value.text='长'.repeat(50);
        for(let j=0;j<rows.length;j++){
            const r=rows[j], f={...(r.Value.defaultTextFormat||{}),color:j===selected?active:normal,size:fontSize};
            r.Value.applied=r.ID.applied=f;
        }
        render(env,selected);
        position(env,192,scene==='battle'?504:(fieldY+18)*2,0,scene==='battle'?486:fieldY*2,640,2000);
        assert.equal(env.list_mc.y,fieldY+23);
        assert.ok(rows[4].Value.wordWrap);
        for(const r of rows)for(const f of [r.ID,r.Value])assert.deepEqual(f.applied,skin);
    }
    console.log(scene, {formats,wraps,paints,made,placements});
    assert.equal(placements,240);
    assert.equal(made,5);
    assert.equal(paints,240); // Selection moves each time; all highlight changes are drawn.
    const measured = {formats,wraps};
    const wide = env.list_mc.rows[4].Value;
    env.stage.stageWidth=1600;
    render(env,0);
    assert.equal(wide.wordWrap,false);
    env.stage.stageWidth=640;
    render(env,0);
    assert.equal(wide.wordWrap,true);
    wide.text='短';
    render(env,0);
    assert.equal(wide.wordWrap,false);
    return measured;
}
const battle=replay('battle'), lobby=replay('lobby');
assert.deepEqual(battle,lobby);
assert.equal(battle.formats,10,'only initialize each of the five pairs of labels');
assert.equal(battle.wraps,1,'a stable long row must not toggle wrapping on each refresh');
