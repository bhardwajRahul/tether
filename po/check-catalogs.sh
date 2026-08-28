#!/bin/sh
# Every catalog in LINGUAS must compile clean and be fully translated. A fuzzy or
# untranslated entry ships English text to a translated desktop, so it fails here
# rather than in CI.
cd "$(dirname "$0")/.."

annotate() {
    if [ -n "$GITHUB_ACTIONS" ]; then
        echo "::error file=po/$1.po::$2"
    else
        echo "po/$1.po: $2" >&2
    fi
}

fail=0
while read -r lang; do
    [ -n "$lang" ] || continue
    out=$(msgfmt --check --check-format --statistics -o /dev/null "po/$lang.po" 2>&1) || fail=1
    case "$out" in
        *untranslated*|*fuzzy*)
            annotate "$lang" "$out"
            fail=1
            ;;
        *) echo "$lang: $out" ;;
    esac
done < po/LINGUAS
exit $fail
