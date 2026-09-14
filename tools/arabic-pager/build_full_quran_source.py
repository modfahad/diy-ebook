"""Assemble the fetched 114 surahs into one QuranSource JSON, plus a cleaned
translation file.

    python tools/arabic-pager/build_full_quran_source.py

Reads   tools/arabic-pager/data/all/{chapters,uthmani-N,en-N}.json
Writes  tools/arabic-pager/data/all/quran-source.json       (QuranSource)
        tools/arabic-pager/data/all/translation-verses.json (cleaned English)

ONE source covering surahs 1..114 in sequence, not 114 sources. The format
addresses surahs as SURAH_INDEX[id - 1], so a package can only hold contiguous
ids starting at 1 -- which 1..114 is, exactly. The alternative, one package per
surah, would also need 228 library index slots against a hard limit of 96.
See docs/quran-content.md section 3.

The translation cleaning is lifted verbatim from fetch_translation.py rather
than reinvented: it strips footnote markup, folds curly punctuation, and
NFKD-drops the combining marks in academic transliterations (Allah with a
macron, Muhammad with an under-dot) that the panel's ASCII-only Latin face
cannot draw. That last rule was a real bug once; it is not a nicety.
"""
import json
import os
import re
import sys
import unicodedata

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, 'data', 'all')
CHAPTERS = 114
CANONICAL_AYAHS = 6236


def clean(text):
    """Exactly fetch_translation.py's clean(). Kept identical on purpose."""
    text = re.sub(r'<sup[^>]*>.*?</sup>', '', text, flags=re.S)
    text = re.sub(r'<[^>]+>', '', text)
    for bad, good in (('‘', "'"), ('’', "'"), ('“', '"'),
                      ('”', '"'), ('–', '-'), ('—', '-'),
                      ('…', '...'), (' ', ' '),
                      # Transliteration half-rings: ayn (U+02BF) and
                      # hamza (U+02BE). These are MODIFIER LETTERS, not
                      # combining marks, so the NFKD pass below never
                      # touched them -- they survived the original
                      # clean() and would reach the panel's ASCII-only
                      # Latin face as 68 undrawable codepoints.
                      ('ʿ', "'"), ('ʾ', "'")):
        text = text.replace(bad, good)
    text = ''.join(ch for ch in unicodedata.normalize('NFKD', text)
                   if not unicodedata.combining(ch))
    return re.sub(r'\s+', ' ', text).strip()


def read(name):
    with open(os.path.join(DATA, name), encoding='utf-8') as handle:
        return json.load(handle)


def main():
    chapters = {c['id']: c for c in read('chapters.json')['chapters']}
    if len(chapters) != CHAPTERS:
        raise SystemExit('expected %d chapters, got %d' % (CHAPTERS, len(chapters)))

    # (surah, ayah) -> [{'page','line'}, ...], one per word, in order.
    word_lines = {
        (v['surah'], v['ayah']): v['words']
        for v in read('word-lines.json')['verses']
    }

    surahs = []
    ayahs = []
    translation_verses = []
    non_ascii = 0
    arabic_in_english = []

    for chapter_id in range(1, CHAPTERS + 1):
        meta = chapters[chapter_id]
        verses = read('uthmani-%d.json' % chapter_id)['verses']
        english = read('en-%d.json' % chapter_id)['translations']

        # The chapter metadata is an independent statement of how many ayahs
        # this surah has. Checking both fetches against it catches a truncated
        # response, which would otherwise become a silently short surah.
        if len(verses) != meta['verses_count']:
            raise SystemExit('surah %d: %d Arabic verses, metadata says %d'
                             % (chapter_id, len(verses), meta['verses_count']))
        if len(english) != meta['verses_count']:
            raise SystemExit('surah %d: %d translation verses, metadata says %d'
                             % (chapter_id, len(english), meta['verses_count']))

        surahs.append({
            'id': chapter_id,  # real chapter number: the whole point of one package
            'name': meta['name_arabic'],
            'revelationPlace':
                'meccan' if meta['revelation_place'] == 'makkah' else 'medinan',
            'hasBismillah': bool(meta['bismillah_pre']),
        })

        for i, verse in enumerate(verses):
            # .strip() and the empty filter below are not cosmetic. The API's
            # Uthmani text carries a LEADING SPACE on the first ayah of 110 of
            # the 114 surahs, so a naive text.split(' ') mints 110 phantom
            # empty words. That would put the text package's WORD_INDEX out of
            # step with the shaped atlas package's (which skips them), by
            # exactly 82121 vs 82011 -- silently misaligning word-level
            # highlighting and Hifz word-hiding for every ayah after the first
            # of each surah.
            text = verse['text_uthmani'].strip()
            words = [w for w in text.split(' ') if w]
            # REAL MADINAH PAGINATION as of 2026-09-02. This used to be a
            # placeholder -- one page per surah, one line per ayah -- because
            # "there is no canonical Madinah-mushaf line-break data in this
            # repository". There is now: fetch_mushaf_layout.py pulls
            # quran.com's per-word page_number/line_number and
            # build_mushaf_layout.py maps it onto this repo's own word
            # splits (see that script for why an aligner, not a join).
            # An ayah is filed under the page and line its FIRST word sits
            # on; its later words can be on another page entirely, which is
            # why the per-word positions are carried separately rather than
            # inferred from this.
            position = word_lines.get((chapter_id, i + 1))
            if position is None:
                raise SystemExit(
                    'no mushaf layout for %d:%d -- run '
                    'build_mushaf_layout.py first' % (chapter_id, i + 1))
            if len(position) != len(words):
                raise SystemExit(
                    '%d:%d has %d words but %d mushaf positions'
                    % (chapter_id, i + 1, len(words), len(position)))
            ayahs.append({
                'surah': chapter_id,
                'ayah': i + 1,
                'page': position[0]['page'],
                'line': position[0]['line'],
                'words': words,
                'text': text,
            })

        for i, item in enumerate(english):
            cleaned = clean(item['text'])
            if any(0x0600 <= ord(ch) <= 0x06FF for ch in cleaned):
                non_ascii += 1
                arabic_in_english.append('%d:%d' % (chapter_id, i + 1))
            translation_verses.append({
                'surah': chapter_id,
                'ayah': i + 1,
                'text': cleaned,
            })

    empty_words = sum(1 for a in ayahs for w in a['words'] if not w.strip())
    if empty_words:
        raise SystemExit('%d empty words survived the split' % empty_words)

    if len(ayahs) != CANONICAL_AYAHS:
        raise SystemExit('assembled %d ayahs, canonical count is %d'
                         % (len(ayahs), CANONICAL_AYAHS))

    source = {
        'schemaVersion': 1,
        'metadata': {
            'title': 'The Holy Quran',
            'script': 'uthmani',
            'language': 'ar',
            'source': 'quran.com API v4, /quran/verses/uthmani; '
                      'page/line from /verses/by_page (Madinah mushaf)',
        },
        'surahs': surahs,
        'ayahs': ayahs,
    }

    with open(os.path.join(DATA, 'quran-source.json'), 'w', encoding='utf-8') as handle:
        json.dump(source, handle, ensure_ascii=False)
    with open(os.path.join(DATA, 'translation-verses.json'), 'w', encoding='utf-8') as handle:
        json.dump(translation_verses, handle, ensure_ascii=False)

    print('surahs        %d' % len(surahs))
    print('ayahs         %d (canonical %d)' % (len(ayahs), CANONICAL_AYAHS))
    print('words         %d' % sum(len(a['words']) for a in ayahs))
    print('translations  %d' % len(translation_verses))
    pages = sorted({a['page'] for a in ayahs})
    print('mushaf pages  %d (%d..%d)' % (len(pages), pages[0], pages[-1]))
    gaps = [p for p in range(pages[0], pages[-1] + 1) if p not in set(pages)]
    print('page gaps     %s' % (gaps[:10] if gaps else 'none (contiguous)'))
    print('verses with Arabic script inside the English: %d' % non_ascii)
    if arabic_in_english:
        print('  ' + ', '.join(arabic_in_english[:12])
              + (' ...' if len(arabic_in_english) > 12 else ''))
    leftover = sorted({ch for v in translation_verses for ch in v['text']
                       if ord(ch) > 127 and not (0x0600 <= ord(ch) <= 0x06FF)})
    print('other non-ASCII codepoints remaining: %s'
          % (', '.join('U+%04X' % ord(c) for c in leftover) or 'none'))


if __name__ == '__main__':
    main()
