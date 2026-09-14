"""Fetch an English translation of a surah from the quran.com API.

Written to a file rather than pasted anywhere: translations are copyrighted
works, and this exists so a specific person can put one on their own device.
The provenance travels in the file.
"""
import io
import json
import re
import subprocess
import sys
import unicodedata

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

CHAPTER = int(sys.argv[1]) if len(sys.argv) > 1 else 78
TRANSLATION = int(sys.argv[2]) if len(sys.argv) > 2 else 20
OUT = sys.argv[3] if len(sys.argv) > 3 else 'tools/arabic-pager/data/an-naba-en.json'

url = ('https://api.quran.com/api/v4/quran/translations/%d?chapter_number=%d'
       % (TRANSLATION, CHAPTER))
raw = subprocess.run(
    ['curl', '-s', '--max-time', '45',
     '-H', 'User-Agent: quran-device-converter/0.1', url],
    capture_output=True, check=True).stdout.decode('utf-8')
data = json.loads(raw)

meta = subprocess.run(
    ['curl', '-s', '--max-time', '45',
     '-H', 'User-Agent: quran-device-converter/0.1',
     'https://api.quran.com/api/v4/resources/translations'],
    capture_output=True, check=True).stdout.decode('utf-8')
info = next(t for t in json.loads(meta)['translations'] if t['id'] == TRANSLATION)


def clean(text):
    # Footnote markers are <sup foot_note="...">n</sup>; the note itself is not
    # in this payload, so a dangling superscript number would just confuse.
    text = re.sub(r'<sup[^>]*>.*?</sup>', '', text, flags=re.S)
    text = re.sub(r'<[^>]+>', '', text)
    # Curly quotes and dashes: the panel font is ASCII-only for Latin.
    for bad, good in (('‘', "'"), ('’', "'"), ('“', '"'),
                      ('”', '"'), ('–', '-'), ('—', '-'),
                      ('…', '...'), (' ', ' ')):
        text = text.replace(bad, good)
    # Academic transliteration marks (Allah -> Allah with macron, Muhammad ->
    # Muhammad with an under-dot) decompose to a base letter plus a combining
    # mark under NFKD; dropping the combining marks recovers the plain ASCII
    # spelling instead of leaving a codepoint the ASCII-only Latin face
    # cannot draw.
    text = ''.join(ch for ch in unicodedata.normalize('NFKD', text)
                   if not unicodedata.combining(ch))
    return re.sub(r'\s+', ' ', text).strip()


verses = [{'verse_key': '%d:%d' % (CHAPTER, i + 1), 'text': clean(t['text'])}
          for i, t in enumerate(data['translations'])]

out = {
    'source': url,
    'translation_id': TRANSLATION,
    'translation_name': info['name'],
    'author': info.get('author_name'),
    'note': 'Copyrighted translation, fetched for personal device use. '
            'Attribution travels with the package; check the licence before '
            'redistributing.',
    'verses': verses,
}
io.open(OUT, 'w', encoding='utf-8', newline='\n').write(
    json.dumps(out, ensure_ascii=False, indent=1))

nonascii = sorted({c for v in verses for c in v['text'] if ord(c) > 126})
print('wrote %s' % OUT)
print('translation  %s (%s)' % (info['name'], info.get('author_name')))
print('verses       %d' % len(verses))
print('avg length   %d chars' % (sum(len(v['text']) for v in verses) // len(verses)))
print('longest      %d chars' % max(len(v['text']) for v in verses))
print('non-ascii    %s' % (repr(''.join(nonascii)) if nonascii else 'none'))
print()
print('excerpt (first two, for your review):')
for v in verses[:2]:
    print('  %s  %s' % (v['verse_key'], v['text']))
