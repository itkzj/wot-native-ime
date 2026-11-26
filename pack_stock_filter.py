# -*- coding: utf-8 -*-
import re
from xml.etree import ElementTree
from zipfile import ZipFile

RESOURCE = 'gui/flash/ime.xml'
# Match the registered IME name as well as its renderer: some Wubi entries
# reuse a Pinyin renderer and even the Pinyin display name in the stock XML.
PINYIN = {
    'GFxIME_SogouPinyin': {'Sogou Pinyin', 'Sogou Pinyin IME', 'SOGOUPY.IME', '搜狗拼音输入法', '搜狗输入法'},
    'GFxIME_Ch_Simp_QQPinyin': {'QQPINYIN.IME', '中文 - QQ拼音输入法'},
}
ENTRY = re.compile(r'(<imeName>([^<]+)</imeName>\s*<displayName>[^<]*</displayName>\s*<Tag>)([^<]+)(</Tag>)')


def stock_registration(archive):
    with ZipFile(archive) as stock:
        if stock.getinfo(RESOURCE).file_size > 64 * 1024:
            raise ValueError('Unexpected Scaleform registration size')
        original = stock.read(RESOURCE)
    root = ElementTree.fromstring(original)
    sections = root.findall('./language/ChineseSimplified')
    if len(sections) != 1:
        raise ValueError('Expected one Simplified Chinese registration section')
    # Build the expected XML tree independently from the byte-preserving patch.
    name = None
    changed = 0
    for entry in sections[0]:
        if entry.tag == 'imeName':
            name = entry.text.encode('utf-8')
        elif entry.tag == 'Tag':
            if name in PINYIN.get(entry.text, ()):
                entry.text = 'GFxIME_NotSupported'
                changed += 1
            name = None
    if changed != 8:
        raise ValueError('Expected eight known Sogou/QQ Pinyin aliases')

    def replace(match):
        prefix, ime, tag, suffix = match.groups()
        if ime in PINYIN.get(tag, ()):
            return prefix + 'GFxIME_NotSupported' + suffix
        return match.group(0)

    start = original.index('<ChineseSimplified>')
    end = original.index('</ChineseSimplified>', start)
    patched = original[:start] + ENTRY.sub(replace, original[start:end]) + original[end:]
    if ElementTree.tostring(ElementTree.fromstring(patched)) != ElementTree.tostring(root):
        raise ValueError('Unexpected change outside the supported Pinyin registrations')
    return patched
