"""Generate firmware/include/app/surah_names.h from chapters.json.

The surah picker (docs/pending.md's "biggest usability gap") needs to label
114 rows with something a person can jump to by name -- "Surah 18" does not
tell you it is Al-Kahf. QPK's own SURAH_INDEX has a name, but it is Arabic
text in TEXT_DATA (see ui::QuranScreen::loadSurah's comment), which
gfx::font5x7 -- the device's only font for chrome like this -- cannot draw.

data/all/chapters.json (fetched by fetch_chapter_meta.py) already carries a
plain-ASCII Latin name per chapter (`name_simple`, e.g. "Al-Kahf"), so this
compiles that into a small table instead of hand-typing 114 names or adding a
field to the QPK format -- the latter would mean regenerating the already-
staged shaped package, which cannot be re-verified while the card is in the
device (docs/pending.md).

    python tools/arabic-pager/gen_surah_names.py
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
CHAPTERS_PATH = os.path.join(HERE, 'data', 'all', 'chapters.json')
OUTPUT_PATH = os.path.join(HERE, '..', '..', 'firmware', 'include', 'app',
                            'surah_names.h')


def main():
    with open(CHAPTERS_PATH, encoding='utf-8') as handle:
        chapters = json.load(handle)['chapters']

    chapters.sort(key=lambda c: c['id'])
    if [c['id'] for c in chapters] != list(range(1, len(chapters) + 1)):
        raise SystemExit('chapters.json ids are not a dense 1..N sequence')

    names = [c['name_simple'] for c in chapters]
    for name in names:
        if not name.isascii():
            raise SystemExit('non-ASCII surah name %r -- font5x7 cannot '
                              'draw it' % name)

    lines = ',\n'.join('  "%s"' % name for name in names)
    header = '''// GENERATED FILE -- DO NOT EDIT.
//
// Latin surah names, compiled from %s
// by tools/arabic-pager/gen_surah_names.py, for the surah picker's row
// labels. Index 0 is surah 1 (kSurahNames[surah_id - 1], same addressing as
// QPK's own SURAH_INDEX). These are NOT the mushaf's Arabic names -- see
// this script's header for why a Latin table exists at all.

#pragma once

namespace app {

constexpr int kSurahNameCount = %d;

constexpr const char* kSurahNames[kSurahNameCount] = {
%s
};

}  // namespace app
''' % (os.path.relpath(CHAPTERS_PATH, os.path.join(HERE, '..', '..')).replace(os.sep, '/'),
       len(names), lines)

    os.makedirs(os.path.dirname(OUTPUT_PATH), exist_ok=True)
    with open(OUTPUT_PATH, 'w', encoding='utf-8', newline='\n') as handle:
        handle.write(header)
    print('wrote %s (%d names)' % (OUTPUT_PATH, len(names)))


if __name__ == '__main__':
    main()
