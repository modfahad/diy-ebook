"""Render any surah as An-Naba-style panel pages, by number.

    python tools/arabic-pager/render_surah.py 1
    python tools/arabic-pager/render_surah.py 78 --px 38 --out preview-naba

Wraps render_pages.py so the surah's name, Arabic name, meaning and ayah count
come from the fetched chapter metadata instead of being retyped per surah.
Requires fetch_all_surahs.py + build_full_quran_source.py +
make_render_translations.py to have run.

DEFAULT GLYPH SIZE IS 38, NOT render_pages.py's 44. The KFGQPC Uthmanic face is
taller than the Naskh one the 44 default was tuned for: at 44 only two
ayah+translation blocks fit a 792x272 page and the bottom third is blank, where
An-Naba's layout fits three. Measured, not guessed -- 44 gives 4 pages for
Al-Fatihah, 38 gives 3 with the page properly filled.
"""
import argparse
import json
import os
import subprocess
import sys

sys.stdout.reconfigure(encoding='utf-8', errors='replace')
HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, 'data', 'all')
FONT = os.path.join(HERE, '..', '..', 'fonts',
                    'KFGQPC Uthmanic Script HAFS Regular.otf')

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('surah', type=int)
parser.add_argument('--px', type=int, default=38)
parser.add_argument('--out', default=None, help='PNG directory')
parser.add_argument('--font', default=FONT)
args = parser.parse_args()

if not 1 <= args.surah <= 114:
    raise SystemExit('surah must be 1..114')

chapters = {c['id']: c for c in
            json.load(open(os.path.join(DATA, 'chapters.json'), encoding='utf-8'))['chapters']}
meta = chapters[args.surah]
out = args.out or os.path.join(HERE, 'preview-surah-%d' % args.surah)
os.makedirs(out, exist_ok=True)

cmd = [sys.executable, os.path.join(HERE, 'render_pages.py'),
       args.font,
       os.path.join(DATA, 'uthmani-%d.json' % args.surah),
       '--translation', os.path.join(DATA, 'tr-%d.json' % args.surah),
       '--png-dir', out,
       '--px', str(args.px),
       '--name', meta['name_simple'],
       '--arabic-name', meta['name_arabic'],
       '--meaning', meta['translated_name']['name'],
       '--symbol', 'kSurah%dPages' % args.surah]
print('surah %d  %s (%s)  %d ayat'
      % (meta['id'], meta['name_simple'], meta['translated_name']['name'],
         meta['verses_count']))
raise SystemExit(subprocess.run(cmd).returncode)
