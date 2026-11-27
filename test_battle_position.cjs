// Replay the stock positioning callback with a battle chat field and a displaced host rectangle.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const source = fs.readFileSync(__dirname + '/StockCandidatePosition.as', 'utf8');
function method(name, args) {
    const start = source.indexOf('function ' + name + '(');
    assert.notEqual(start, -1);
    const body = source.slice(source.indexOf('{', start) + 1, source.indexOf('\n        }', start))
        .replace(/(var \w+|catch \(\w+):\w+/g, '$1').replace(/ as TextField/g, '')
        .replace(/super\.RepositionWindow/g, 'stock');
    return new Function('env', ...args, 'with(env){' + body + '}');
}
const place = method('RepositionWindow', ['x','bottom','readingHeight','top','width','height']);
function replay(scale, battle, lineTop = 718, hostBottom = 504) {
    const field = {type:'input', numLines:1, multiline:false,
        getBounds:() => ({left:48, right:237, top:lineTop, bottom:lineTop+18, width:189, height:18})};
    const stage = field.stage = {focus:field, scaleX:scale, scaleY:scale, stageHeight:1000*scale, stageWidth:1600*scale};
    field.parent = battle ? {messageInputField:field} : {};
    const domain = {hasDefinition:()=>false};
    field.root = {loaderInfo:{applicationDomain:domain}};
    const list_mc = {getBounds:()=>({left:list_mc.x, top:list_mc.y, width:334, height:125})};
    const env = {stage, list_mc, loaderInfo:{applicationDomain:domain}, TextFieldType:{INPUT:'input'},
        reportedCorrection:true, candListHeight:125, stageToList:(x,y)=>({x,y}),
        includesCaret:(b,x,y)=>x>=b.left && x<=b.right && y>=b.top && y<=b.bottom,
        stock(x,bottom,reading,top,w,h) {
            list_mc.x=x;
            const above = !(125 < h-bottom-reading || h-bottom > top);
            list_mc.y = above ? top-125-5 : bottom+reading+5;
            return above;
        }};
    const above = place(env,192,hostBottom,0,hostBottom-18,1600*scale,1000*scale);
    return [list_mc.x,list_mc.y,above];
}
for (const scale of [1,2]) {
    assert.deepEqual(replay(scale,true), [192,741,false], `battle chat at ${scale}x`);
    assert.deepEqual(replay(scale,true,970), [192,840,true], `near bottom at ${scale}x`);
    assert.deepEqual(replay(scale,false), [192,509,false], 'other input retains its own anchor');
}
assert.deepEqual(replay(2,true,718,1454), [96,741,false], 'a scaled host caret is converted once');
console.log('Battle stock candidates follow the focused row at 1x and 2x, including the lower edge');
