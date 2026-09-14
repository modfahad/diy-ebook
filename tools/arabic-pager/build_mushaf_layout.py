"""Map the Madinah mushaf's (page, line) onto THIS repo's word sequence.

    python tools/arabic-pager/build_mushaf_layout.py

Reads   data/all/mushaf-layout.json   (fetch_mushaf_layout.py, quran.com)
        data/all/quran-source.json    (build_full_quran_source.py)
Writes  data/all/word-lines.json      one (page, line) per word, in order

WHY AN ALIGNER AND NOT A JOIN. The two sides disagree about what a "word"
is, and about orthography, so their word lists cannot be zipped:

  * quran.com's per-page word units attach a waqf (pause) mark to the word
    before it, usually joined by a space -- "رَيْبَ ۛ" is ONE unit there and
    TWO tokens here. 4,583 such spaces; 4,578 of our tokens are pure marks.
  * Their edition carries tajweed marks ours does not: U+06ED x4,807 vs 99,
    U+06E2 x2,445 vs 510.
  * A handful of verses differ in combining-mark ORDER only (52:1, 54:1 --
    shadda before vs after the vowel), which is invisible but not equal.
  * 11:13 differs orthographically (ٱفْتَرَىٰهُ vs افْتَرَاهُ), and 5:52 has a
    spurious internal space splitting دَآئِرَةٌ into two of their tokens.

Naively zipping produced 82,011 vs 77,429 -- a 4,582-word drift that would
have misaligned the entire package downstream of Al-Baqarah. That is exactly
the phantom-word bug class docs/quran-content.md section 10 records.

So words are matched on their CONSONANTAL SKELETON: every combining mark
dropped, every alef form (آأإاٱ) and alef maqsura folded together. The match
is greedy and many-to-many, which absorbs both their spurious splits and
their space-joined units. Our pure-mark tokens carry no skeleton at all, so
they are not matched -- each one is given the line of the word it follows,
which is where the mushaf itself puts it.

THE POINT OF ALL THIS: our word sequence is not modified. The atlas, the
shaped WORD_INDEX and the ayah-marker glyph ids stay byte-identical; only a
(page, line) label is added alongside. An aligner that "fixed" our splits to
match theirs would have forced a re-shape and re-broken the alignment
build-full-quran-shaped.mjs exists to guard.

The script refuses to write anything unless all 6,236 verses and all 82,011
words align.
"""
import io
import json
import sys
import unicodedata

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = 'tools/arabic-pager/data/all'
LAYOUT_IN = '%s/mushaf-layout.json' % HERE
SOURCE_IN = '%s/quran-source.json' % HERE
OUT = '%s/word-lines.json' % HERE

# Alef forms and alef maqsura fold together: the two editions disagree about
# which one a given word uses (11:13), and the distinction never changes
# which word it is.
ALEF = set('آأإاٱى')


def skeleton(text):
    """Letters only, alef forms folded. Empty for a pure mark token."""
    return ''.join('ا' if c in ALEF else c
                   for c in text if unicodedata.category(c) == 'Lo')


def align_verse(mine, their_tokens, their_positions):
    """(page, line) per token of `mine`, or None if the verse cannot align."""
    out = [None] * len(mine)
    mine_words = [i for i, w in enumerate(mine) if skeleton(w)]
    their_words = [i for i, w in enumerate(their_tokens) if skeleton(w)]

    i = j = 0
    while i < len(mine_words):
        want = skeleton(mine[mine_words[i]])
        got = ''
        used = []
        while len(got) < len(want) and j < len(their_words):
            got += skeleton(their_tokens[their_words[j]])
            used.append(their_words[j])
            j += 1
        if not used:
            return None
        if got == want:
            out[mine_words[i]] = their_positions[used[0]]
            i += 1
            continue
        if not got.startswith(want):
            return None
        # One of theirs spans several of ours: find how many, and give them
        # all that unit's line -- they are on it.
        span = [mine[mine_words[i]]]
        k = i + 1
        while skeleton(''.join(span)) != got and k < len(mine_words):
            span.append(mine[mine_words[k]])
            k += 1
        if skeleton(''.join(span)) != got:
            return None
        for n in range(i, k):
            out[mine_words[n]] = their_positions[used[0]]
        i = k

    if any(out[i] is None for i in mine_words):
        return None

    # A pure-mark token belongs to the line of the word before it. A verse
    # that opens with one (the rub-el-hizb sign ۞ does this) takes the line
    # of the word after instead -- there is no word before.
    last = None
    for n in range(len(mine)):
        if out[n] is None:
            out[n] = last
        else:
            last = out[n]
    if out[0] is None:
        following = next((p for p in out if p is not None), None)
        for n in range(len(mine)):
            if out[n] is not None:
                break
            out[n] = following
    return None if any(p is None for p in out) else out


def main():
    layout = json.load(io.open(LAYOUT_IN, encoding='utf-8'))
    source = json.load(io.open(SOURCE_IN, encoding='utf-8'))
    ours = {(a['surah'], a['ayah']): a['words'] for a in source['ayahs']}

    verses_out = []
    failed = []
    assigned = 0
    for verse in layout['verses']:
        surah, ayah = (int(p) for p in verse['verse_key'].split(':'))
        mine = ours.get((surah, ayah))
        if mine is None:
            failed.append((surah, ayah, 'not in quran-source.json'))
            continue

        tokens, positions = [], []
        for word in verse['words']:
            # Their unit may carry an internal space (word + waqf mark);
            # both halves sit on the same line.
            for piece in word['text'].split():
                tokens.append(piece)
                positions.append((word['page'], word['line']))

        mapped = align_verse(mine, tokens, positions)
        if mapped is None:
            failed.append((surah, ayah, 'could not align %d words' % len(mine)))
            continue
        assigned += len(mapped)
        verses_out.append({
            'surah': surah,
            'ayah': ayah,
            'words': [{'page': p, 'line': l} for (p, l) in mapped],
            'end': verse.get('end'),
        })

    total = sum(len(w) for w in ours.values())
    print('verses   %d of %d aligned' % (len(verses_out), len(ours)))
    print('words    %d of %d assigned' % (assigned, total))
    if failed:
        print('FAILED verses (%d):' % len(failed))
        for surah, ayah, why in failed[:20]:
            print('  %d:%d  %s' % (surah, ayah, why))
        raise SystemExit(
            'refusing to write: an unaligned verse silently misplaces every '
            'word after it. Fix the aligner, do not relax the check.')
    if assigned != total:
        raise SystemExit('assigned %d of %d words' % (assigned, total))

    io.open(OUT, 'w', encoding='utf-8', newline='\n').write(
        json.dumps({
            'mushaf': layout['mushaf'],
            'pages': layout['pages'],
            'verses': verses_out,
        }, ensure_ascii=False))
    print('wrote    %s' % OUT)


if __name__ == '__main__':
    main()
