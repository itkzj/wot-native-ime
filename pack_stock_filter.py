import re
from xml.etree import ElementTree
from zipfile import ZipFile

RESOURCE = 'gui/flash/ime.xml'
UNSUPPORTED = 'GFxIME_NotSupported'
KEYBOARDS = {'GFxIME_Ch_Simp', 'GFxIME_Ch_Trad'}
# WoT CN 2.4.1.1: total registrations and registrations with a stock renderer.
EXPECTED = {'ChineseSimplified': (56, 54), 'ChineseTraditional': (35, 31),
            'Japanese': (13, 13), 'Korean': (11, 11)}
TAG = re.compile(r'<Tag>(GFxIME_\w+)</Tag>')


def stock_registration(archive):
    with ZipFile(archive) as stock:
        if stock.getinfo(RESOURCE).file_size > 64 * 1024:
            raise ValueError('Unexpected Scaleform registration size')
        original = stock.read(RESOURCE)
    root = ElementTree.fromstring(original)
    languages = root.find('language')
    if root.tag != 'data' or languages is None or sorted(child.tag for child in languages) != sorted(EXPECTED):
        raise ValueError('Unexpected Scaleform registration format')
    keyboards = []
    for language in languages:
        tags = language.findall('Tag')
        if len(tags) != len(language.findall('imeName')):
            raise ValueError('Missing Scaleform name or tag in ' + language.tag)
        changed = 0
        for tag in tags:
            if tag.text in KEYBOARDS:
                keyboards.append(tag.text)
            elif tag.text != UNSUPPORTED:
                tag.text = UNSUPPORTED
                changed += 1
        if (len(tags), changed) != EXPECTED[language.tag]:
            raise ValueError('Unexpected registration counts for ' + language.tag)
    if sorted(keyboards) != sorted(KEYBOARDS):
        raise ValueError('Expected both stock US keyboard registrations')

    # Keep the client's original encoding, comments, whitespace, names and GUIDs.
    patched, count = TAG.subn(lambda m: m.group(0) if m.group(1) in KEYBOARDS else '<Tag>' + UNSUPPORTED + '</Tag>', original)
    if (count != sum(total for total, _ in EXPECTED.values()) or
            ElementTree.tostring(ElementTree.fromstring(patched)) != ElementTree.tostring(root)):
        raise ValueError('Rewritten tags do not match parsed registrations')
    return patched
