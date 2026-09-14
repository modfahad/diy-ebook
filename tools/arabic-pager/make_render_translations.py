"""Re-shape the cleaned translation into what render_pages.py's --translation
expects: one file per surah, {"verses":[{"verse_key":"1:1","text":"..."}]}.

The raw API payload (en-<n>.json) has no verse_key and is uncleaned; the
cleaned corpus (translation-verses.json) has surah/ayah but not the key. This
bridges the two so the An-Naba page renderer can interleave translations for
any surah without refetching.
"""
import json
import os
import sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, 'data', 'all')

verses = json.load(open(os.path.join(DATA, 'translation-verses.json'), encoding='utf-8'))
by_surah = {}
for v in verses:
    by_surah.setdefault(v['surah'], []).append(
        {'verse_key': '%d:%d' % (v['surah'], v['ayah']), 'text': v['text']})

for surah, items in sorted(by_surah.items()):
    payload = {'translation_name': 'Saheeh International',
               'author': 'Saheeh International',
               'source': 'quran.com API v4, translation 20',
               'verses': items}
    with open(os.path.join(DATA, 'tr-%d.json' % surah), 'w', encoding='utf-8') as h:
        json.dump(payload, h, ensure_ascii=False)

print('wrote %d per-surah translation files' % len(by_surah))
