#!/bin/bash
#
# Gate: the translation catalogs, their template and the metadata translations
# stay consistent with each other and with the sources.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Nothing else in the build notices these mistakes, and each one reaches users
# silently: KI18n falls back to English for any message a catalog lacks, and
# ki18n_install() installs whatever it finds under po/. Five checks, each
# reported on its own so one run shows every problem:
#
#  1. po/ layout. A .po file directly under po/ would be compiled into a
#     language-less directory by ki18n_install(). A language directory must be
#     named like a locale, because the uninstall allowlist for an installed
#     catalog must accept exactly that alphabet or uninstall would refuse the
#     installed .mo, and hold exactly kcm_nasmount.po: ki18n_install() would
#     install any other .po under the same directory.
#  2. Every catalog passes `msgfmt --check`. ki18n_install() runs msgfmt
#     without --check, so a translation that drops or mangles a %1 would
#     otherwise be compiled, installed and shown to users. msgfmt checks a
#     placeholder only for a message flagged kde-format, so the template is
#     checked first: a message naming a %N placeholder without the flag would
#     make this whole check pass vacuously for it.
#  3. The committed template is current: a fresh extraction equals it once
#     source references, line wrapping, ordering and the header are ignored.
#     A string added or edited without running `make pot` fails here, so the
#     template translators work from cannot fall behind the code. Source
#     references are ignored so an edit that only shifts line numbers does not
#     demand a regeneration.
#  4. Backstop for QML text that was never wrapped in a translation call.
#     This is a heuristic, not proof: it cannot see a lowercase literal, nor a
#     second literal on a line that already contains a translation call, so a
#     pass does not mean every string is translated. Where it does fire on a
#     capitalized literal that is data rather than text, change the code so
#     the value is not spelled that way; do not loosen the pattern.
#  5. Every language ships its metadata. The desktop entries, the KCM's
#     plugin metadata and the polkit action descriptions hold their
#     translations inline, outside the catalog, so nothing else ties them to a
#     language directory; this makes "add a language" a rule instead of a
#     checklist. A key that exists untranslated needs its translation
#     everywhere it occurs (the service menu has two Name= lines, and the one
#     Dolphin shows in its context menu is the second), and each polkit action
#     needs exactly one Name and one Description, since the Name becomes the
#     text of the authentication prompt.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

domain=kcm_nasmount
template="po/$domain.pot"
# The alphabet of an installed catalog's directory: ru, pt_BR, sr@latin,
# ca@valencia, zh_CN. The uninstall allowlist for an installed catalog must stay
# identical to it.
lang_pattern='^[a-z]{2,3}(_[A-Z]{2})?(@[a-z]+)?$'

tmp=$(mktemp -d)
trap 'rm -rf -- "$tmp"' EXIT

failed=0
fail() {
    echo "ERROR: $*" >&2
    failed=1
}

shopt -s nullglob

# --- 1. po/ layout -----------------------------------------------------------
languages=()
for stray in po/*.po; do
    fail "$stray sits directly under po/; ki18n_install() would compile it into a language-less directory. Move it to po/<lang>/$domain.po."
done
for dir in po/*/; do
    lang=${dir#po/}
    lang=${lang%/}
    if ! printf '%s\n' "$lang" | LC_ALL=C grep -Eq "$lang_pattern"; then
        fail "po/$lang is not a language directory name (expected ll, ll_CC or ll@variant): the uninstall allowlist for an installed catalog admits no other shape."
        continue
    fi
    languages+=("$lang")
    if [ ! -f "po/$lang/$domain.po" ]; then
        fail "po/$lang has no $domain.po."
    fi
    while IFS= read -r extra; do
        fail "$extra is not po/$lang/$domain.po; ki18n_install() would install it too."
    done < <(find "po/$lang" -name '*.po' ! -path "po/$lang/$domain.po" -print | LC_ALL=C sort)
done

# --- 2. catalogs compile cleanly ---------------------------------------------
# First the template: a message whose msgid names %1..%9 but is not flagged
# kde-format is one msgfmt --check cannot verify a translation of, and the
# extractor is the only thing that sets the flag (xgettext sets none for QML).
# Reading msgid lines only, as the extractor does: a translator comment or a
# msgctxt may mention %1 without the text taking one.
if [ -f "$template" ]; then
    unflagged=$(msgcat --no-wrap "$template" | awk '
        BEGIN { RS = ""; FS = "\n" }
        {
            flagged = 0; inid = 0; placeholder = 0; ref = ""; id = ""
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^#: / && ref == "") ref = substr($i, 4)
                if ($i ~ /^#, / && $i ~ /(^|[ ,])kde-format($|[ ,])/) flagged = 1
                if ($i ~ /^msgid(_plural)? /) {
                    inid = 1
                    if (id == "") id = $i
                } else if ($i ~ /^msg(ctxt|str)/) {
                    inid = 0
                }
                if (inid && $i ~ /%[1-9]/) placeholder = 1
            }
            if (placeholder && !flagged) print ref ": " id
        }')
    if [ -n "$unflagged" ]; then
        fail "template messages name a %N placeholder but are not flagged kde-format, so msgfmt --check cannot verify their translations (fix po/extract-messages.sh):"
        printf '%s\n' "$unflagged" >&2
    fi
fi
for catalog in po/*/"$domain".po; do
    if ! output=$(msgfmt --check -o /dev/null "$catalog" 2>&1); then
        fail "msgfmt --check rejects $catalog:"
        echo "$output" >&2
    fi
done

# --- 3. the template is current ----------------------------------------------
# msgcat re-sorts and unwraps both sides with the same tool, so neither the
# order entries were extracted in nor the line length the writer chose can
# differ. The header entry is dropped: it carries the version and the
# extraction date, which change without the messages changing.
normalize() {
    msgcat --no-location --no-wrap --sort-output "$1" | sed '1,/^$/d'
}

if [ ! -f "$template" ]; then
    fail "$template is missing: run make pot."
elif ! bash po/extract-messages.sh --template-only "$tmp/fresh.pot"; then
    fail "po/extract-messages.sh failed (it needs xgettext and msgcat from gettext)."
elif ! normalize "$template" > "$tmp/committed.txt" || ! normalize "$tmp/fresh.pot" > "$tmp/fresh.txt"; then
    fail "msgcat could not read the template."
elif ! diff -u --label "$template (committed)" --label "fresh extraction" \
        "$tmp/committed.txt" "$tmp/fresh.txt" >&2; then
    fail "$template is out of date with the sources: run make pot and commit the result."
fi

# --- 4. backstop for unwrapped QML text --------------------------------------
# A capitalized string literal on a line that is not a comment or an import and
# carries no translation call. The first grep fails with 2 on a real error (a
# missing file), which the guard below lets through to errexit; 1 only means no
# match.
qml_files=(src/kcm/ui/*.qml src/dialog/qml/*.qml)
if [ "${#qml_files[@]}" -eq 0 ]; then
    fail "no QML files found to check."
else
    capitalized=$(grep -nE '"[A-Z][A-Za-z]' "${qml_files[@]}" || [ $? -eq 1 ])
    untranslated=$(printf '%s\n' "$capitalized" \
        | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(//|\*|/\*|import )' \
        | grep -v 'i18n' || [ $? -eq 1 ])
    if [ -n "$untranslated" ]; then
        fail "QML string literals not wrapped in a translation call:"
        printf '%s\n' "$untranslated" >&2
    fi
fi

# --- 5. every language ships its metadata ------------------------------------
desktop_files=(servicemenu/nasmount.desktop.in packaging/nasmount-manager.desktop)
plugin_json=src/kcm/kcm_nasmount.json
actions=io.github.pakru.nasmount.actions
for lang in ${languages[@]+"${languages[@]}"}; do
    # Every occurrence of a key needs its twin: counting both sides catches a
    # language that translated the entry's Name but not the action's.
    for file in "${desktop_files[@]}"; do
        for key in Name GenericName Comment Keywords; do
            untranslated=$(grep -c "^$key=" "$file" || true)
            translated=$(grep -c "^$key\\[$lang\\]=" "$file" || true)
            if [ "$translated" -ne "$untranslated" ]; then
                fail "$file has $translated $key[$lang]= lines for $untranslated $key= lines."
            fi
        done
    done
    # The plugin metadata is flat JSON with no section to count in, so presence
    # is all that can be asked: a key that exists needs its translated twin.
    for key in Name Description X-KDE-Keywords; do
        if grep -Fq "\"$key\"" "$plugin_json" && ! grep -Fq "\"$key[$lang]\"" "$plugin_json"; then
            fail "$plugin_json has \"$key\" but no \"$key[$lang]\" entry."
        fi
    done
    # [Domain] holds the product name, which is never translated; only the
    # action sections count.
    problems=$(awk -v lang="$lang" '
        function flush() {
            if (section ~ /^\[io\.github\./) {
                if (names != 1) print section " has " names " Name[" lang "]= lines, expected 1"
                if (descriptions != 1) print section " has " descriptions " Description[" lang "]= lines, expected 1"
            }
        }
        /^\[/ { flush(); section = $0; names = 0; descriptions = 0; next }
        $0 ~ "^Name\\[" lang "\\]=" { names++ }
        $0 ~ "^Description\\[" lang "\\]=" { descriptions++ }
        END { flush() }' "$actions")
    if [ -n "$problems" ]; then
        fail "$actions lacks the $lang action translations:"
        printf '%s\n' "$problems" >&2
    fi
done

if [ "$failed" -ne 0 ]; then
    exit 1
fi
