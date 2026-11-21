from cStringIO import StringIO
import re
import sys
from xml.etree import ElementTree
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile

RESOURCE = 'gui/flash/ime.xml'
MOD_NAME = 'wot.native.ime.stock-filter_1.0.1.wotmod'
OUTPUT = 'dist/WotNativeIme_V17_StockFilter.zip'
UNSUPPORTED = 'GFxIME_NotSupported'
KEYBOARDS = {'GFxIME_Ch_Simp', 'GFxIME_Ch_Trad'}
# WoT CN 2.4.1.1 registration counts, including localized aliases.
EXPECTED = {'ChineseSimplified': (56, 54), 'ChineseTraditional': (35, 31),
            'Japanese': (13, 13), 'Korean': (11, 11)}
TAG = re.compile(r'(<Tag>)(GFxIME_\w+)(</Tag>)')


def patch_registration(original):
    root = ElementTree.fromstring(original)
    languages = root.find('language')
    if root.tag != 'data' or languages is None or sorted(child.tag for child in languages) != sorted(EXPECTED):
        raise ValueError('Unexpected Scaleform registration format')
    preserved_keyboards = []
    patched_count = 0
    for language in languages:
        tags = language.findall('Tag')
        names = language.findall('imeName')
        if len(tags) != len(names) or any(not tag.text or not tag.text.startswith('GFxIME_') for tag in tags):
            raise ValueError('Unexpected Scaleform entries for ' + language.tag)
        changed = 0
        for tag in tags:
            if tag.text in KEYBOARDS:
                preserved_keyboards.append(tag.text)
            elif tag.text != UNSUPPORTED:
                tag.text = UNSUPPORTED
                changed += 1
        if (len(tags), changed) != EXPECTED[language.tag]:
            raise ValueError('Unexpected Scaleform registration counts for ' + language.tag)
        patched_count += changed
    if sorted(preserved_keyboards) != sorted(KEYBOARDS):
        raise ValueError('Expected both stock US keyboard registrations')

    def replace(match):
        opening, tag, closing = match.groups()
        if tag in KEYBOARDS or tag == UNSUPPORTED:
            return match.group(0)
        return opening + UNSUPPORTED + closing

    # Preserve stock encoding, whitespace and every field other than renderer tags.
    patched = TAG.sub(replace, original)
    if (len(TAG.findall(original)) != sum(total for total, _ in EXPECTED.values()) or
            ElementTree.tostring(ElementTree.fromstring(patched)) != ElementTree.tostring(root)):
        raise ValueError('Scaleform registration rewrite does not match parsed entries')
    return patched, patched_count


def stock_filter(archive):
    with ZipFile(archive) as stock:
        if stock.getinfo(RESOURCE).file_size > 64 * 1024:
            raise ValueError('Unexpected Scaleform registration size')
        patched, count = patch_registration(stock.read(RESOURCE))
    metadata = ('<?xml version="1.0" encoding="utf-8"?>\n'
                '<root><id>wot.native.ime.stock-filter</id><version>1.0.1</version>\n'
                '<name>WoT Native IME Stock Filter</name></root>\n')
    payload = StringIO()
    with ZipFile(payload, 'w', ZIP_STORED) as mod:
        mod.writestr('meta.xml', metadata)
        mod.writestr('res/' + RESOURCE, patched)
    with ZipFile(payload) as mod:
        assert mod.testzip() is None and mod.namelist() == ['meta.xml', 'res/' + RESOURCE]
        assert mod.read('res/' + RESOURCE) == patched
    print('Scaleform renderer tags replaced: %d' % count)
    return payload.getvalue()


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: python pack_stock_filter.py PATH_TO_GUI_PART3_PKG')
    payload = stock_filter(sys.argv[1])
    with ZipFile(OUTPUT, 'w', ZIP_DEFLATED) as release:
        release.writestr('mods/2.4.1.1/' + MOD_NAME, payload)
    with ZipFile(OUTPUT) as release:
        assert release.testzip() is None and release.namelist() == ['mods/2.4.1.1/' + MOD_NAME]
    print(OUTPUT)
