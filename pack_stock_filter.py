from cStringIO import StringIO
import re
import sys
from xml.etree import ElementTree
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile

RESOURCE = 'gui/flash/ime.xml'
MOD = 'mods/2.4.1.1/wot.native.ime.stock-filter_1.0.0.wotmod'
OUTPUT = 'dist/WotNativeIme_V16_StockFilter.zip'
TAGS = {'GFxIME_SogouPinyin', 'GFxIME_Ch_Simp_QQPinyin'}
STOCK_ENTRY = re.compile(r'(<imeName>[^<]*</imeName>\s*<displayName>)([^<]*)(</displayName>\s*<Tag>)([^<]*)(</Tag>)')


def patch_registration(original):
    root = ElementTree.fromstring(original)
    simplified = root.find('language/ChineseSimplified')
    if root.tag != 'data' or simplified is None:
        raise ValueError('Unexpected Scaleform registration format')
    opening, closing = '<ChineseSimplified>', '</ChineseSimplified>'
    start = original.index(opening) + len(opening)
    end = original.index(closing, start)
    seen = set()

    def replace(entry):
        prefix, name, middle, tag, suffix = entry.groups()
        if tag not in TAGS or name == 'Sougou Wubi':
            return entry.group(0)
        seen.add(tag)
        return prefix + name + middle + 'GFxIME_NotSupported' + suffix

    patched = original[:start] + STOCK_ENTRY.sub(replace, original[start:end]) + original[end:]
    if seen != TAGS:
        raise ValueError('Expected both stock Sogou and QQ Pinyin registrations')
    ElementTree.fromstring(patched)
    return patched


def main(archive):
    with ZipFile(archive) as stock:
        if stock.getinfo(RESOURCE).file_size > 64 * 1024:
            raise ValueError('Unexpected Scaleform registration size')
        patched = patch_registration(stock.read(RESOURCE))
    metadata = ('<?xml version="1.0" encoding="utf-8"?>\n'
                '<root><id>wot.native.ime.stock-filter</id><version>1.0.0</version>\n'
                '<name>WoT Native IME Stock Filter Experiment</name></root>\n')
    payload = StringIO()
    with ZipFile(payload, 'w', ZIP_STORED) as mod:
        mod.writestr('meta.xml', metadata)
        mod.writestr('res/' + RESOURCE, patched)
    with ZipFile(OUTPUT, 'w', ZIP_DEFLATED) as release:
        release.writestr(MOD, payload.getvalue())
    with ZipFile(OUTPUT) as release:
        assert release.testzip() is None and release.namelist() == [MOD]
        with ZipFile(StringIO(release.read(MOD))) as mod:
            assert mod.testzip() is None and mod.namelist() == ['meta.xml', 'res/' + RESOURCE]
            assert mod.read('res/' + RESOURCE) == patched
    print(OUTPUT)


if __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit('usage: python pack_stock_filter.py PATH_TO_GUI_PART3_PKG')
    main(sys.argv[1])
