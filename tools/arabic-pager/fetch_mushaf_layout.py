"""Fetch the Madinah mushaf's own page/line assignment for every word.

    python tools/arabic-pager/fetch_mushaf_layout.py [out.json]

This is the data docs/pending.md called unsourced -- "the canonical Madinah
line-break data, which nothing in this repo has yet" -- and it closes that
gap. quran.com's /verses/by_page endpoint returns, per word, the physical
`page_number` (1..604) and `line_number` (1..15) it occupies in the Madinah
mushaf. That is exactly what LAYOUT_DATA needs and what the device's
fill-and-wrap renderer has been approximating.

WHY BY PAGE AND NOT BY VERSE: a verse routinely spans a page boundary, so
/verses/by_page returns a partial word list for such a verse on each of the
two pages it touches. Words are therefore accumulated across the whole 604-
page sweep and only assembled per verse at the end -- assembling per page
would silently truncate every straddling verse. (Probing pages 2, 3 and 255
in isolation showed exactly that: 0 of page 255's 6 verses were complete.)

char_type_name == "end" entries are the mushaf's own end-of-ayah markers.
They are NOT words and are kept separately: folding them into the word list
would shift every subsequent word by one and misalign the whole package --
the phantom-word bug class docs/quran-content.md section 10 describes. Their
own (page, line) is recorded because a marker occupies a real position on a
real line, which a renderer placing markers needs.

No Quranic text is written from memory here; the API is the only source.
"""
import io
import json
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

OUT = (sys.argv[1] if len(sys.argv) > 1
       else 'tools/arabic-pager/data/all/mushaf-layout.json')

PAGES = 604
BASE = ('https://api.quran.com/api/v4/verses/by_page/%d?words=true'
        '&word_fields=line_number,text_uthmani&per_page=50')


def fetch_page(page):
    """One page, with retries. A transient failure mid-sweep would otherwise
    lose 600 pages of work, and the endpoint is occasionally slow."""
    url = BASE % page
    last = None
    for attempt in range(4):
        try:
            result = subprocess.run(
                ['curl', '-s', '--max-time', '45',
                 '-H', 'User-Agent: quran-device-converter/0.1', url],
                capture_output=True, check=True)
            return json.loads(result.stdout.decode('utf-8'))['verses']
        except Exception as error:  # noqa: BLE001 -- retry anything
            last = error
            time.sleep(1.5 * (attempt + 1))
    raise SystemExit('page %d failed after 4 attempts: %s' % (page, last))


def main():
    verses = {}   # verse_key -> {'words': [...], 'end': {...} or None}
    order = []

    for page in range(1, PAGES + 1):
        for verse in fetch_page(page):
            key = verse['verse_key']
            if key not in verses:
                verses[key] = {'words': [], 'end': None}
                order.append(key)
            entry = verses[key]
            for word in verse['words']:
                # THE PAGE COMES FROM THE REQUEST, NOT FROM THE WORD.
                # quran.com returns a per-word `page_number` and it is
                # sometimes wrong: /verses/by_page/121 correctly groups verse
                # 5:77 onto page 121, but stamps every one of its words
                # `page_number: 120` -- a page whose lines 1-3 are already
                # occupied by 5:71 and 5:72. Trusting that field put 5:77 back
                # at the top of page 120 and made the line sequence run
                # backwards, which the check at the end of this script now
                # catches. The endpoint's own grouping is authoritative: this
                # word came back from the request for page `page`.
                position = {
                    'page': page,
                    'line': int(word['line_number']),
                }
                if word['char_type_name'] == 'word':
                    entry['words'].append(dict(position, text=word['text_uthmani']))
                elif word['char_type_name'] == 'end':
                    # Recorded once. A marker cannot straddle a page, so a
                    # second sighting would mean the sweep saw a page twice.
                    if entry['end'] is None:
                        entry['end'] = position
        if page % 50 == 0:
            print('  ...%d/%d pages' % (page, PAGES))
        time.sleep(0.12)  # be a polite client, not a scraper

    # Verses come back in page order, which is Quranic order -- assert it
    # rather than assume, since everything downstream indexes positionally.
    def key_of(k):
        surah, ayah = (int(p) for p in k.split(':'))
        return (surah, ayah)

    if order != sorted(order, key=key_of):
        raise SystemExit('verses did not arrive in Quranic order')

    # The corpus reads front to back: (page, line) must never go backwards
    # across the whole sweep. This is the check that caught the per-word
    # page_number bug above, and it is the invariant LAYOUT_DATA's line
    # records depend on (docs/qpk-format.md rule 17) -- a backwards step here
    # becomes an out-of-order line record there, and a renderer that binary-
    # searches them would silently draw the wrong line.
    previous = None
    for k in order:
        for word in verses[k]['words']:
            current = (word['page'], word['line'])
            if previous is not None and current < previous:
                raise SystemExit(
                    'page/line went backwards at %s: %s after %s. The mushaf '
                    'does not read backwards, so this is bad data, not a bad '
                    'assumption.' % (k, current, previous))
            previous = current

    total_words = sum(len(v['words']) for v in verses.values())
    missing_end = [k for k in order if verses[k]['end'] is None]
    pages_seen = sorted({w['page'] for v in verses.values() for w in v['words']})
    lines_seen = sorted({w['line'] for v in verses.values() for w in v['words']})

    out = {
        'source': BASE % 0,
        'mushaf': 'Madinah (quran.com /verses/by_page, per-word line_number)',
        'pages': PAGES,
        'verses': [
            {'verse_key': k, 'words': verses[k]['words'], 'end': verses[k]['end']}
            for k in order
        ],
    }
    io.open(OUT, 'w', encoding='utf-8', newline='\n').write(
        json.dumps(out, ensure_ascii=False))

    print('wrote          %s' % OUT)
    print('verses         %d' % len(order))
    print('words          %d' % total_words)
    print('pages seen     %d (%d..%d)' % (len(pages_seen), pages_seen[0], pages_seen[-1]))
    print('lines seen     %d (%d..%d)' % (len(lines_seen), lines_seen[0], lines_seen[-1]))
    print('verses with no end marker: %d %s'
          % (len(missing_end), missing_end[:5]))


if __name__ == '__main__':
    main()
