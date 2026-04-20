#!/bin/env sh

set -eu

help() {
    echo "usage: $0 [-t git|archive|file] [-o output] <url>"
    exit 1
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || {
        echo "error: missing required command: $1" >&2
        exit 1
    }
}

basename_from_url() {
    url="$1"
    url="${url%%\?*}"
    url="${url%%\#*}"
    printf "%s\n" "${url##*/}"
}

repo_name_from_git() {
    name="$(basename_from_url "$1")"
    name="${name%.git}"
    printf "%s\n" "$name"
}

detect_type() {
    case "$1" in
        *.git|git@*|ssh://git@*)
            echo "git"
            ;;
        *.tar.gz|*.tgz|*.tar.bz2|*.tbz2|*.tar.xz|*.txz|*.tar|*.zip)
            echo "archive"
            ;;
        *)
            echo "file"
            ;;
    esac
}

archive_kind() {
    case "$1" in
        *.tar.gz|*.tgz) echo "tar.gz" ;;
        *.tar.bz2|*.tbz2) echo "tar.bz2" ;;
        *.tar.xz|*.txz) echo "tar.xz" ;;
        *.tar) echo "tar" ;;
        *.zip) echo "zip" ;;
        *) echo "" ;;
    esac
}

download_file() {
    url="$1"
    dest="$2"

    require_cmd curl
    mkdir -p "$(dirname "$dest")"
    curl -fL --retry 3 -o "$dest" "$url"
}

fetch_git() {
    url="$1"
    outdir="$2"

    require_cmd git
    git clone "$url" "$outdir" --recurse-submodules
}

fetch_archive() {
    url="$1"
    outdir="$2"
    kind="$(archive_kind "$url")"

    [ -n "$kind" ] || {
        echo "error: could not determine archive format from url: $url" >&2
        exit 1
    }

    require_cmd curl
    mkdir -p "$outdir"

    tmp="$(mktemp)"
    trap 'rm -f "$tmp"' EXIT INT TERM

    curl -fL --retry 3 -o "$tmp" "$url"

    case "$kind" in
        tar.gz)  require_cmd tar; tar -xzf "$tmp" -C "$outdir" ;;
        tar.bz2) require_cmd tar; tar -xjf "$tmp" -C "$outdir" ;;
        tar.xz)  require_cmd tar; tar -xJf "$tmp" -C "$outdir" ;;
        tar)     require_cmd tar; tar -xf "$tmp" -C "$outdir" ;;
        zip)     require_cmd unzip; unzip -q "$tmp" -d "$outdir" ;;
        *) echo "error: unsupported archive type: $kind" >&2; exit 1 ;;
    esac
}

fetch_file() {
    url="$1"
    outfile="$2"
    download_file "$url" "$outfile"
}

forced_type=""
output=""

while getopts "t:o:h" opt; do
    case "$opt" in
        t) forced_type="$OPTARG" ;;
        o) output="$OPTARG" ;;
        h|?) help ;;
    esac
done

shift $((OPTIND - 1))

[ $# -eq 1 ] || help
url="$1"

case "$forced_type" in
    "" ) type="$(detect_type "$url")" ;;
    git|archive|file) type="$forced_type" ;;
    * )
        echo "error: invalid type: $forced_type" >&2
        help
        ;;
esac

case "$type" in
    git)
        outdir="${output:-$(repo_name_from_git "$url")}"
        fetch_git "$url" "$outdir"
        ;;
    archive)
        outdir="${output:-.}"
        fetch_archive "$url" "$outdir"
        ;;
    file)
        filename="$(basename_from_url "$url")"
        outfile="${output:-$filename}"
        fetch_file "$url" "$outfile"
        ;;
esac

echo "done: $url"