"""Fetch a chapter's Arabic verses (imlaei script) from the quran.com API.

    python tools/arabic-pager/fetch_verses.py <chapter> <out.json>

Mirrors data/an-naba-imlaei.json's shape exactly -- render_pages.py reads
verse['text_uthmani'] regardless of which script endpoint produced the file,
so that field name is kept even though the imlaei endpoint is what's fetched.

No Quranic text is written from memory anywhere in this repository: this is
the desktop's only source for verse text.
"""
import io
import json
import subprocess
import sys

# Windows consoles default to cp1252, which cannot print Arabic; the file
# itself is always written as UTF-8 regardless of this.
sys.stdout.reconfigure(encoding='utf-8', errors='replace')

CHAPTER = int(sys.argv[1]) if len(sys.argv) > 1 else 78
OUT = sys.argv[2] if len(sys.argv) > 2 else 'tools/arabic-pager/data/chapter-imlaei.json'

url = ('https://api.quran.com/api/v4/quran/verses/imlaei?chapter_number=%d'
       % CHAPTER)
raw = subprocess.run(
    ['curl', '-s', '--max-time', '45',
     '-H', 'User-Agent: quran-device-converter/0.1', url],
    capture_output=True, check=True).stdout.decode('utf-8')
data = json.loads(raw)

verses = [{'verse_key': v['verse_key'], 'text_uthmani': v['text_imlaei']}
          for v in data['verses']]

out = {
    'source': url,
    'fetched_for': 'chapter %d' % CHAPTER,
    'script': 'imlaei (standard orthography, fully vowelled)',
    'verses': verses,
}
io.open(OUT, 'w', encoding='utf-8', newline='\n').write(
    json.dumps(out, ensure_ascii=False, indent=1))

print('wrote %s' % OUT)
print('chapter      %d' % CHAPTER)
print('verses       %d' % len(verses))
print('excerpt (first, for your review):')
if verses:
    print('  %s  %s' % (verses[0]['verse_key'], verses[0]['text_uthmani']))
