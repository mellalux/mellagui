#!/bin/sh
# Downloads a collection of free fonts from Google Fonts (github.com/google/fonts) into the user's font folder, where the Paint text tool - and every
# other program that reads ~/.local/share/fonts - finds them. The fonts are free to use and share: SIL Open Font License (ofl/), Apache 2.0 (apache/) or
# the Ubuntu Font Licence (ufl/); the licence text of each family is saved beside its files.
#
#   tools/get-google-fonts.sh                      the curated collection below (about 90 families)
#   tools/get-google-fonts.sh lobster pacifico     only these families (the folder name in the repository: lower case, no blanks)
#   tools/get-google-fonts.sh --list               print the collection and stop
#   tools/get-google-fonts.sh --dest DIR ...       another folder (default: $XDG_DATA_HOME/fonts/google, i.e. ~/.local/share/fonts/google)
#   tools/get-google-fonts.sh --remove             delete what this script put there
#
# Needs curl. Only the regular, bold, italic and bold italic files are taken (and the variable font file of a family that has only that - the text tool
# draws its default weight and makes bold itself); a family that is there already is left alone, so it is safe to run again. One request fetches the
# repository's file list (about 7 MB); then every file is downloaded by itself. Not looked at by MellaGUI's tests: they never touch the network.
set -eu

DEST="${XDG_DATA_HOME:-${HOME:-.}/.local/share}/fonts/google"
LIST=0
REMOVE=0
FAMILIES=""

# sans serif
SANS="lato opensans roboto notosans montserrat raleway poppins rubik nunito quicksand comfortaa worksans dmsans manrope inter outfit josefinsans firasans
ibmplexsans sourcesans3 ptsans ubuntu oswald teko fredoka baloo2"
# serif
SERIF="playfairdisplay merriweather lora ebgaramond cormorantgaramond spectral librebaskerville ptserif notoserif cinzel robotoslab"
# monospace
MONO="robotomono sourcecodepro firacode jetbrainsmono ibmplexmono inconsolata ubuntumono vt323"
# display
DISPLAY_F="anton bebasneue bangers creepster righteous bungee orbitron abrilfatface alfaslabone archivoblack fugazone monoton staatliches pressstart2p
silkscreen unifrakturcook luckiestguy chewy specialelite"
# handwriting and script
HAND="pacifico lobster caveat dancingscript sacramento greatvibes kaushanscript indieflower shadowsintolight amaticsc architectsdaughter gloriahallelujah
patrickhand courgette handlee kalam permanentmarker satisfy rocksalt homemadeapple yellowtail"
CURATED="$SANS $SERIF $MONO $DISPLAY_F $HAND"

while [ $# -gt 0 ]; do
    case "$1" in
    --list) LIST=1 ;;
    --remove) REMOVE=1 ;;
    --dest) shift; DEST="${1:?--dest needs a folder}" ;;
    -h|--help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    -*) echo "unknown option: $1" >&2; exit 2 ;;
    *) FAMILIES="$FAMILIES $1" ;;
    esac
    shift
done
[ -n "$FAMILIES" ] || FAMILIES="$CURATED"

if [ "$LIST" = 1 ]; then
    for f in $CURATED; do echo "$f"; done
    exit 0
fi
if [ "$REMOVE" = 1 ]; then
    if [ -d "$DEST" ]; then
        rm -rf "$DEST"
        echo "removed $DEST"
    else
        echo "nothing to remove ($DEST)"
    fi
    exit 0
fi
command -v curl >/dev/null 2>&1 || { echo "this needs curl" >&2; exit 1; }

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
echo "reading the list of files of google/fonts ..."
curl -fsS --retry 2 -o "$TMP/tree.json" "https://api.github.com/repos/google/fonts/git/trees/main?recursive=1" || {
    echo "could not read the file list from GitHub (no network, or its rate limit)" >&2
    exit 1
}
grep -o '"path": "\(ofl\|apache\|ufl\)/[^"]*"' "$TMP/tree.json" | sed 's/"path": "//;s/"$//' > "$TMP/paths.txt"

RAW="https://raw.githubusercontent.com/google/fonts/main"
mkdir -p "$DEST"
n_fam=0
n_files=0
n_skip=0
missing=""
for fam in $FAMILIES; do
    dir=""
    case "$fam" in
    */*) grep -q "^$fam/" "$TMP/paths.txt" && dir="$fam" ;;
    *) for lic in ofl apache ufl; do
           if grep -q "^$lic/$fam/" "$TMP/paths.txt"; then dir="$lic/$fam"; break; fi
       done ;;
    esac
    if [ -z "$dir" ]; then
        missing="$missing $fam"
        continue
    fi
    name="${dir#*/}"
    # the top level files of the family folder only: Regular / Bold / Italic / BoldItalic, or a variable font ([wght] in the name)
    grep "^$dir/[^/]*\.ttf$" "$TMP/paths.txt" | sed "s#^$dir/##" | grep -E '(-(Regular|Bold|Italic|BoldItalic)|\[[^]]*\]|-(Italic|Bold)\[[^]]*\])\.ttf$|^[A-Za-z0-9_]*\.ttf$' > "$TMP/files.txt" || true
    # (a family with one plain file, such as Lobster-Regular.ttf or ShadowsIntoLight.ttf, is taken as it is; Light, Black ... are left out)
    grep -vE '(-(Thin|ExtraLight|Light|Medium|SemiBold|ExtraBold|Black|Condensed|Expanded)[A-Za-z]*)\.ttf$' "$TMP/files.txt" > "$TMP/files2.txt" || true
    if [ ! -s "$TMP/files2.txt" ]; then
        missing="$missing $fam"
        continue
    fi
    mkdir -p "$DEST/$name"
    got=0
    while IFS= read -r file; do
        out="$DEST/$name/$file"
        if [ -s "$out" ]; then
            n_skip=$((n_skip + 1))
            continue
        fi
        # the brackets and the comma of a variable font's name must be written as %XX in a URL
        enc="$(printf '%s' "$file" | sed 's/\[/%5B/g; s/\]/%5D/g; s/,/%2C/g')"
        if curl -fsS --retry 2 --globoff -o "$out.part" "$RAW/$dir/$enc"; then
            mv "$out.part" "$out"
            got=$((got + 1))
            n_files=$((n_files + 1))
        else
            rm -f "$out.part"
            echo "  could not get $dir/$file" >&2
        fi
    done < "$TMP/files2.txt"
    for lic in OFL.txt LICENSE.txt UFL.txt; do                           # the licence text, once per family
        if grep -q "^$dir/$lic$" "$TMP/paths.txt" && [ ! -s "$DEST/$name/$lic" ]; then
            curl -fsS --retry 2 -o "$DEST/$name/$lic" "$RAW/$dir/$lic" 2>/dev/null || rm -f "$DEST/$name/$lic"
        fi
    done
    n_fam=$((n_fam + 1))
    echo "$name: $got new file(s)"
done
echo
echo "done: $n_files file(s) downloaded, $n_skip were there already, in $DEST"
[ -z "$missing" ] || echo "not found (the folder name may differ in the repository):$missing"
echo "Paint's text tool lists them next time it is started (Text tool > Font)."
