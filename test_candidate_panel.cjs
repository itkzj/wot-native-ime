// Replay the production AS3 methods with measured display-list operations, without a game client.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, 'CandidatePanel.as'), 'utf8');
let fields = 0, writes = 0;
const rows = [], paints = [];
class TextField {
    constructor() { fields++; }
    get text() { return this.caption; }
    set text(value) { this.caption = value; writes++; }
    get width() { return this.wordWrap ? this.wrapWidth : this.text.length * 8 + 4; }
    set width(value) { this.wrapWidth = value; }
    get height() { return this.wordWrap ? 36 : 18; }
}
const panel = {
    TextField, TextFieldAutoSize: {LEFT: 'left'},
    CandidateSkin: {format: () => ({}), rowHeight: h => Math.max(22, h), paint: (_, ...args) => paints.push(args)},
    graphics: {beginFill() {}, drawRect() {}, endFill() {}},
    labels: null, preedit: '', selectedIndex: -1, layoutLimit: NaN, panelWidth: NaN, panelHeight: NaN,
    separatorY: NaN, highlightHeight: NaN, highlightY: NaN, x: 0, y: 0, scaleX: 1, scaleY: 1,
    get numChildren() { return rows.length; }, getChildAt: i => rows[i],
    addChild: row => rows.push(row), removeChildAt: i => rows.splice(i, 1),
    parent: {
        numChildren: 2, top: null, raises: 0,
        getChildIndex(child) { return child === this.top ? 1 : 0; },
        getChildAt(index) { assert.equal(index, 1); return this.top; },
        setChildIndex(child, index) { assert.equal(index, 1); this.top = child; this.raises++; },
    },
};
let transformWrites = 0;
for (const key of ['x', 'y', 'scaleX', 'scaleY']) {
    let value = panel[key];
    Object.defineProperty(panel, key, {
        get() { return value; }, set(next) { value = next; transformWrites++; },
    });
}
for (const method of ['layout', 'paint', 'position', 'showPage']) {
    const signature = `function ${method}(`;
    const start = source.indexOf(signature);
    assert.notEqual(start, -1, `Missing production method ${method}`);
    const args = source.slice(start + signature.length, source.indexOf(')', start)).replace(/:\w+/g, '');
    const body = source.slice(source.indexOf('{', start) + 1, source.indexOf('\n        }', start))
        .replace(/(\w+):(Number|Boolean|String|int|TextField)\b/g, '$1').replace(/ as TextField/g, '');
    panel[method] = new Function('panel', args, 'with (panel) {' + body + '}').bind(panel, panel);
}
const labels = Array.from({length: 9}, (_, i) => '词' + i);
const screen = {width: 800, height: 600}, caret = {left: 755, top: 400, bottom: 420};
for (let i = 0; i < 240; ++i) {
    panel.showPage(labels, 0, 'py' + i, screen, caret);
    assert.equal(rows[0].text, 'py' + i);
    assert.equal(rows[9].text, '9. 词8');
}
assert.equal(fields, 10);
assert.equal(writes, 249);
assert.deepEqual(paints, [[100, 226, 25, 22]]);
assert.equal(panel.x, 700);
assert.equal(panel.y, 170);
assert.equal(transformWrites, 2, 'stable geometry only writes the initial x and y');
assert.equal(panel.parent.raises, 1);
panel.parent.top = {};
panel.position(screen, caret);
assert.equal(panel.parent.top, panel);
assert.equal(panel.parent.raises, 2);
panel.showPage(['甲', '乙'], 1, '', screen, caret);
assert.deepEqual(rows.map(row => row.text), ['1. 甲', '2. 乙']);
assert.deepEqual(paints.at(-1), [100, 50, 25, 22]);
panel.showPage([], -1, 'a'.repeat(40), screen, caret);
panel.position({width: 100, height: 80}, caret);
assert.equal(rows.length, 1);
assert.equal(rows[0].wordWrap, true);
assert.deepEqual(paints.at(-1), [92, 42, 0, 0]);
panel.position(screen, caret);
assert.equal(rows[0].wordWrap, false);
panel.position({width: 40, height: 10}, {left: 0, top: 0, bottom: 0});
assert.ok(panel.scaleX < 1 && panel.scaleX === panel.scaleY);
panel.position(screen, {left: 10, top: 40, bottom: 60});
assert.deepEqual([panel.x, panel.y, panel.scaleX, panel.scaleY], [10, 64, 1, 1]);
console.log('Candidate panel replay passed: 240 updates, 10 fields, 249 text writes, 1 paint, 2 transform writes at fixed geometry');
