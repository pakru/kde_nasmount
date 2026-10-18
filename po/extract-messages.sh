#!/bin/bash
# Regenerate the translation template from the sources and merge it into every
# language. Maintainer tool: the build never runs it.
# SPDX-License-Identifier: GPL-3.0-or-later
#
#   po/extract-messages.sh                      rewrite po/kcm_nasmount.pot, then
#                                               msgmerge every po/<lang>/kcm_nasmount.po
#   po/extract-messages.sh --template-only OUT  write the template to OUT and touch
#                                               nothing else (tests/translations_gate.sh);
#                                               a relative OUT is relative to the caller's
#                                               directory
#
# QML is extracted as JavaScript, never as C++: the C++ scanner stops at the
# first literal of `i18n("a " + "b")` and records "a " as the whole message,
# silently. The JavaScript scanner joins the literals.
#
# The QML scan is joined into the C++ one (xgettext --join-existing), not
# merged afterwards with msgcat. A message that occurs in both with the same
# context becomes one entry that keeps both source references and any
# translator comment from either side, while the same text under a different
# context stays two entries, as it is two meanings. `msgcat --use-first` keeps
# only the first file's copy of such an entry, silently dropping the other's
# reference and `// i18n:` comment; plain msgcat keeps the comment but prefixes
# it with a marker line naming the input file.
#
# Every message that names a %1..%9 placeholder ends up flagged kde-format, the
# format `msgfmt --check` then verifies a translation against. C++ gets the flag
# from --kde, but the JavaScript scanner flags nothing, so without a pass of our
# own a Russian msgstr that dropped the %1 of a QML message would compile,
# install and show users a sentence missing its subject, while the same slip in
# a C++ message is rejected. See the flagging pass below.
#
# Source lists are sorted (byte order, so the result does not depend on the
# caller's locale) and entries keep first-occurrence order, so the template is
# reproducible.
#
# Only the long-standing xgettext/msgcat/msgmerge options are used, because the
# Debian/Ubuntu and Fedora build containers run the template gate with
# whichever gettext their release ships.
set -euo pipefail

invoked_from=$PWD
repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

domain=kcm_nasmount
template="po/$domain.pot"
template_only=""
usage() {
    echo "usage: $0 [--template-only OUTPUT]" >&2
    exit 2
}
case $# in
    0) ;;
    2) # An empty OUT is refused, not treated as "no OUT": that would silently
       # turn the read-only mode into the one that rewrites the template and
       # merges every catalog.
       { [ "$1" = "--template-only" ] && [ -n "$2" ]; } || usage
       # The script changes into the repository root, so a relative OUT would
       # otherwise land somewhere the caller did not mean.
       case $2 in
           /*) template_only=$2 ;;
           *) template_only="$invoked_from/$2" ;;
       esac ;;
    *) usage ;;
esac

work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT

# The KI18n call families the code may use. Every spelling is listed even where
# none is used today, so adding one later cannot produce a silently missing
# message.
keywords=(
    --keyword=i18n:1 --keyword=i18nc:1c,2 --keyword=i18np:1,2 --keyword=i18ncp:1c,2,3
    --keyword=ki18n:1 --keyword=ki18nc:1c,2 --keyword=ki18np:1,2 --keyword=ki18ncp:1c,2,3
    --keyword=kli18n:1 --keyword=kli18nc:1c,2 --keyword=kli18np:1,2 --keyword=kli18ncp:1c,2,3
    --keyword=xi18n:1 --keyword=xi18nc:1c,2 --keyword=xi18np:1,2 --keyword=xi18ncp:1c,2,3
)
# --add-comments=i18n collects the `// i18n: ...` notes above a call into the
# entry as translator comments. --force-po writes a file even when a scan finds
# nothing, so a failure shows up as a diff rather than a missing file.
# --no-wrap keeps every msgid on one line per source line, which the flagging
# pass below relies on; the final msgcat wraps the template for reading.
common=(
    --from-code=UTF-8 --add-comments=i18n --force-po --no-wrap
    --package-name=nasmount --package-version="$(tr -d '[:space:]' < VERSION)"
    --msgid-bugs-address=https://github.com/pakru/kde_nasmount/issues
)

# Only the front ends translate. core/root/helper/boot/packageguard never do
# (the helper runs as root with no knowledge of the caller's language), and the
# cleanup tool's output is deliberately English.
mapfile -t cpp < <(find src/session src/kcm src/dialog \( -name '*.cpp' -o -name '*.h' \) -print | LC_ALL=C sort)
mapfile -t qml < <(find src/kcm/ui src/dialog/qml -name '*.qml' -print | LC_ALL=C sort)

xgettext "${common[@]}" "${keywords[@]}" -C --kde -o "$work/joined.pot" "${cpp[@]}"
xgettext "${common[@]}" "${keywords[@]}" -L JavaScript --join-existing -o "$work/joined.pot" "${qml[@]}"
# Flag every message whose msgid or msgid_plural names a %1..%9 placeholder
# kde-format, unless it already is (the C++ ones). Only msgid lines count: a
# translator comment or a msgctxt may mention %1 without the text taking one,
# and flagging it would make msgfmt demand a placeholder the sentence never
# had. The pass reads paragraphs (one entry each) and, for an entry with no flag
# line yet, adds one in front of its msgctxt or msgid, where the format wants it.
awk '
BEGIN { RS = ""; FS = "\n"; OFS = "\n" }
{
    flag = 0; first = 0; inid = 0; placeholder = 0
    for (i = 1; i <= NF; i++) {
        if ($i ~ /^#, /) {
            flag = i
        } else if (first == 0 && $i ~ /^msg(ctxt|id)/) {
            first = i
        }
        if ($i ~ /^msgid(_plural)? /) {
            inid = 1
        } else if ($i ~ /^msg(ctxt|str)/) {
            inid = 0
        }
        if (inid && $i ~ /%[1-9]/) {
            placeholder = 1
        }
    }
    if (placeholder && !(flag && $flag ~ /(^|[ ,])kde-format($|[ ,])/)) {
        if (flag) {
            $flag = $flag ", kde-format"
        } else {
            $first = "#, kde-format\n" $first
        }
    }
    print $0 "\n"
}' "$work/joined.pot" > "$work/flagged.pot"
# A header whose messages are all ASCII keeps `charset=CHARSET`; a template must
# declare UTF-8 for msgmerge and msgfmt to accept the translations built on it.
msgcat --to-code=UTF-8 -o "$work/all.pot" "$work/flagged.pot"

if [ -n "$template_only" ]; then
    cp -- "$work/all.pot" "$template_only"
    exit 0
fi
mv -- "$work/all.pot" "$template"
for po in po/*/"$domain".po; do
    [ -e "$po" ] || continue
    msgmerge --quiet --update --backup=none --previous "$po" "$template"
done
