#!/bin/sh
# Regenerate po/tether.pot from the sources in POTFILES.in and merge it into
# every catalog listed in LINGUAS. Run from the repository root, or via `make pot`.
set -e

cd "$(dirname "$0")/.."

# The release version, not `git describe`: a commit count and hash would make the
# pot stale on every commit, and committing the regenerated pot would change the
# hash again.
VERSION=$(sed -n 's/^project(tether VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$VERSION" ] || VERSION=unknown

# The metadata has to be repeated on the --join-existing call: it rewrites the
# header from its own defaults rather than keeping the one already in the file.
META="--package-name=tether --package-version=$VERSION --msgid-bugs-address=zack@bartel.com"

NEW=$(mktemp)
OLD_STRIPPED=$(mktemp)
NEW_STRIPPED=$(mktemp)
trap 'rm -f "$NEW" "$OLD_STRIPPED" "$NEW_STRIPPED"' EXIT

# shellcheck disable=SC2086
# --add-location=file: line numbers shift on every unrelated edit, and different
# gettext versions number the Desktop backend differently, so keeping them would
# churn all the catalogs and make the pot depend on which machine ran this.
xgettext --files-from=po/POTFILES.in --directory=. \
         --output="$NEW" --from-code=UTF-8 --c++ \
         --keyword=_ --keyword=N_ --keyword=P_:1,2 \
         --add-comments=TRANSLATORS --add-location=file $META

# Desktop entry keys join the same catalog, so msgfmt --desktop can localize them.
# shellcheck disable=SC2086
xgettext -j -L Desktop --output="$NEW" --add-location=file \
         --keyword=Name --keyword=GenericName --keyword=Comment $META \
         src/gtk/tether-gtk.desktop.in

# POT-Creation-Date alone changes on every run and would dirty the pot and every
# catalog for nothing, so a run that changed only the timestamp is dropped. The
# catalog loop below still runs: adding a language to LINGUAS must create its
# catalog even when no source string changed.
pot_changed=1
if [ -f po/tether.pot ]; then
    grep -v '^"POT-Creation-Date:' po/tether.pot > "$OLD_STRIPPED"
    grep -v '^"POT-Creation-Date:' "$NEW" > "$NEW_STRIPPED"
    if cmp -s "$OLD_STRIPPED" "$NEW_STRIPPED"; then
        pot_changed=0
    fi
fi
[ "$pot_changed" -eq 0 ] || cp "$NEW" po/tether.pot

while read -r lang; do
    [ -n "$lang" ] || continue
    if [ -f "po/$lang.po" ]; then
        msgmerge --quiet --update --backup=none "po/$lang.po" po/tether.pot
    else
        msginit --no-translator --locale="$lang" --input=po/tether.pot --output="po/$lang.po"
    fi
done < po/LINGUAS

if [ "$pot_changed" -eq 0 ]; then
    echo "po/tether.pot is up to date; $(wc -l < po/LINGUAS) catalogs checked"
else
    echo "po/tether.pot updated; $(grep -c '^msgid' po/tether.pot) entries"
fi
