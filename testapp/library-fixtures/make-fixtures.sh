#!/usr/bin/env bash
# Generate the Library Core host fixtures (testapp/library-fixtures/).
#
# Deterministic: run `bash testapp/library-fixtures/make-fixtures.sh` to
# (re)create every file. Contains 41 supported book files (names chosen to
# exercise latin, cyrillic, spaces, mixed-case extensions, deterministic
# sort and a list longer than one 14-row viewport) plus ignored extras:
# directories, hidden files, unsupported extensions and temporary files.
# One decoy ('draft.txt~') intentionally matches the repository's `*~`
# gitignore rule, so fixtures are always regenerated rather than committed.
set -euo pipefail
cd "$(dirname "$0")"

mkdir -p subdir empty

books=(
  "01 zone.txt"
  "02 sector.txt"
  "03 flat.txt"
  "04 plain.txt"
  "05 fifth.txt"
  "06 sixth.txt"
  "07 seventh.txt"
  "08 octave.txt"
  "09 ninth.txt"
  "10 ten.txt"
  "The Hobbit.epub"
  "the-lord-of-the-rings.fb2"
  "Война и мир.epub"
  "Преступление и наказание.txt"
  "Анна Каренина.txt"
  "Мастер и Маргарита.fb2"
  "test book.txt"
  "Test BOOK.epub"
  "test book 2.txt"
  "a small story.txt"
  "A Tall Tale.txt"
  "alpha.txt"
  "Alphabet.txt"
  "barbar.txt"
  "beta.txt"
  "charms.txt"
  "delta.txt"
  "echo.txt"
  "foxtrot.txt"
  "golf.txt"
  "hotel.txt"
  "india.txt"
  "juliet.txt"
  "kilo.txt"
  "lima.txt"
  "mike.txt"
  "november.txt"
  "oscar.txt"
  "papa.txt"
  "quebec.txt"
  "roman.txt"
)

# Remove stale generated files (never removes dirs or .gitkeep).
find . -type f ! -name .gitkeep ! -name make-fixtures.sh -delete

for b in "${books[@]}"; do
  printf 'fixture dummy content\n' > "$b"
done

# ignored extras (stale ones are wiped by the find above)
printf 'x\n' > notes.pdf
printf 'x\n' > cover.JPG
printf 'x\n' > overview.zip
printf 'x\n' > 'draft.txt~'
printf 'x\n' > .hidden.epub
printf 'x\n' > '#lock#.txt'
printf 'x\n' > note.TMP
printf 'x\n' > subdir/inside.txt
touch empty/.gitkeep

echo "fixtures generated: $(find . -type f ! -name .gitkeep ! -name make-fixtures.sh | wc -l) files"