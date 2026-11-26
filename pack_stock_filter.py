import re
from xml.etree import ElementTree
from zipfile import ZipFile

RESOURCE = 'gui/flash/ime.xml'
CHINESE = ('ChineseSimplified', 'ChineseTraditional')
KEYBOARDS = ('GFxIME_Ch_Simp', 'GFxIME_Ch_Trad')
UNSUPPORTED = 'GFxIME_NotSupported'
TAG = re.compile(r'<Tag>(GFxIME_\w+)</Tag>')


def stock_registration(archive):
    with ZipFile(archive) as stock:
        if stock.getinfo(RESOURCE).file_size > 64 * 1024:
            raise ValueError('Unexpected Scaleform registration size')
        original = stock.read(RESOURCE)
    root = ElementTree.fromstring(original)
    patched = original
    for language in CHINESE:
        sections = root.findall('./language/' + language)
        if len(sections) != 1:
            raise ValueError('Expected one registration section for ' + language)
        tags = sections[0].findall('Tag')
        if not tags or len(tags) != len(sections[0].findall('imeName')):
            raise ValueError('Missing IME name or tag in ' + language)
        for tag in tags:
            if tag.text not in KEYBOARDS:
                tag.text = UNSUPPORTED
        start = patched.index('<%s>' % language)
        end = patched.index('</%s>' % language, start)
        section, count = TAG.subn(lambda m: m.group(0) if m.group(1) in KEYBOARDS else
                                 '<Tag>' + UNSUPPORTED + '</Tag>', patched[start:end])
        if count != len(tags):
            raise ValueError('Unexpected registration tags in ' + language)
        patched = patched[:start] + section + patched[end:]
    if ElementTree.tostring(ElementTree.fromstring(patched)) != ElementTree.tostring(root):
        raise ValueError('Rewritten XML differs from the intended Chinese IME registrations')
    return patched
