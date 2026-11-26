"""Run against a CN #997 donor and the separately built candidate resource."""
from __future__ import print_function
import io
import sys

from patch_stock_ime import NEW_ROOT, OLD_ROOT, read_movie


def verify(stock, patched):
    stage, before = read_movie(stock)
    new_stage, after = read_movie(patched)
    assert new_stage == stage
    index, = [i for i, tag in enumerate(before) if tag[0] == 76]
    assert before[:index] == after[:index]  # Includes the complete original DoABC.
    assert before[index + 1:] == after[index + 2:]
    kind, abc, _ = after[index]
    assert kind == 82 and b'StockCandidatePosition' in abc and b'RepositionWindow' in abc
    assert b'[WotNativeIme] stock candidate position corrected' in abc
    assert after[index + 1][:2] == (76, before[index][1].replace(OLD_ROOT, NEW_ROOT))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('Usage: test_stock_ime.py STOCK_IME_SWF PATCHED_IME_SWF')
    with io.open(sys.argv[1], 'rb') as stock, io.open(sys.argv[2], 'rb') as patched:
        verify(stock.read(), patched.read())
    print('Verified the original movie, every original DoABC and all non-root symbols')
