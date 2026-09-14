"""Fetch all 114 surahs -- Uthmani text, translation, and chapter metadata.

    python tools/arabic-pager/fetch_all_surahs.py [--translation 20]

Writes one file per chapter under data/all/, plus a single chapters.json:

    data/all/uthmani-<n>.json     verse text, Uthmani script
    data/all/en-<n>.json          translation
    data/all/chapters.json        all 114 chapters' names/metadata, one call

**Resumable on purpose.** A file that already exists and parses is skipped, so
an interrupted run costs nothing and re-running is cheap. 228 requests against
a public API is not something to repeat carelessly.

Uthmani rather than the imlaei script fetch_verses.py uses: the device now has
the KFGQPC Uthmanic Script HAFS font, whose whole point is rendering exactly
this text -- including U+06D6, which the previous Naskh font lacked and which
was being dropped from An-Naba.

No Quranic text is written from memory anywhere in this repository; the API is
the only source, and provenance travels in each file.
"""
import json
import os
import subprocess
import sys
import time

sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(HERE, 'data', 'all')
TRANSLATION_ID = 20  # Saheeh International, same as the An-Naba pass
CHAPTERS = 114
PAUSE_SECONDS = 0.25


def get(url):
    raw = subprocess.run(
        ['curl', '-s', '--max-time', '45',
         '-H', 'User-Agent: quran-device-converter/0.1', url],
        capture_output=True, check=True).stdout.decode('utf-8')
    return json.loads(raw)


def already_good(path, key):
    """True when the file exists and holds a non-empty `key`."""
    if not os.path.exists(path):
        return False
    try:
        with open(path, encoding='utf-8') as handle:
            data = json.load(handle)
        return bool(data.get(key))
    except (json.JSONDecodeError, OSError):
        return False  # truncated by an interrupted run; refetch


def write(path, payload):
    with open(path, 'w', encoding='utf-8') as handle:
        json.dump(payload, handle, ensure_ascii=False, indent=1)


def main():
    os.makedirs(OUT_DIR, exist_ok=True)

    chapters_path = os.path.join(OUT_DIR, 'chapters.json')
    if not already_good(chapters_path, 'chapters'):
        print('fetching chapter metadata (1 call for all 114)')
        write(chapters_path, get('https://api.quran.com/api/v4/chapters?language=en'))
    else:
        print('chapters.json already present')

    fetched = skipped = 0
    for chapter in range(1, CHAPTERS + 1):
        arabic_path = os.path.join(OUT_DIR, 'uthmani-%d.json' % chapter)
        if already_good(arabic_path, 'verses'):
            skipped += 1
        else:
            write(arabic_path, get(
                'https://api.quran.com/api/v4/quran/verses/uthmani?chapter_number=%d'
                % chapter))
            fetched += 1
            time.sleep(PAUSE_SECONDS)

        english_path = os.path.join(OUT_DIR, 'en-%d.json' % chapter)
        if already_good(english_path, 'translations'):
            skipped += 1
        else:
            write(english_path, get(
                'https://api.quran.com/api/v4/quran/translations/%d?chapter_number=%d'
                % (TRANSLATION_ID, chapter)))
            fetched += 1
            time.sleep(PAUSE_SECONDS)

        if chapter % 10 == 0 or chapter == CHAPTERS:
            print('  chapter %3d/114  fetched=%d skipped=%d'
                  % (chapter, fetched, skipped), flush=True)

    print('done: %d fetched, %d already present' % (fetched, skipped))


if __name__ == '__main__':
    main()
