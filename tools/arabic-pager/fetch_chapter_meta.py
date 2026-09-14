"""Fetch a chapter's identity (Arabic name, English meaning, revelation place,
whether it opens with a separate Bismillah line) from the quran.com API.

    python tools/arabic-pager/fetch_chapter_meta.py <chapter> <out.json>

Al-Fatihah is the one surah where bismillah_pre comes back false: its
Bismillah is ayah 1 itself, not a separate line before the surah, so this is
fetched rather than assumed true for every surah.
"""
import io
import json
import subprocess
import sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

CHAPTER = int(sys.argv[1]) if len(sys.argv) > 1 else 78
OUT = sys.argv[2] if len(sys.argv) > 2 else 'tools/arabic-pager/data/chapter-meta.json'

url = 'https://api.quran.com/api/v4/chapters/%d?language=en' % CHAPTER
raw = subprocess.run(
    ['curl', '-s', '--max-time', '45',
     '-H', 'User-Agent: quran-device-converter/0.1', url],
    capture_output=True, check=True).stdout.decode('utf-8')
chapter = json.loads(raw)['chapter']

out = {
    'source': url,
    'id': chapter['id'],
    'name_simple': chapter['name_simple'],
    'name_arabic': chapter['name_arabic'],
    'meaning': chapter['translated_name']['name'],
    'revelation_place': chapter['revelation_place'],
    'bismillah_pre': chapter['bismillah_pre'],
    'verses_count': chapter['verses_count'],
}
io.open(OUT, 'w', encoding='utf-8', newline='\n').write(
    json.dumps(out, ensure_ascii=False, indent=1))

print('wrote %s' % OUT)
print('%(name_simple)s (%(name_arabic)s) -- %(meaning)s' % out)
print('revelation_place=%(revelation_place)s bismillah_pre=%(bismillah_pre)s verses=%(verses_count)d'
      % out)
