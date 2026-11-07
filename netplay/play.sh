#!/bin/bash
# Play a rollback netplay session with ./np built by build-macos.sh:
#   netplay/play.sh host <game> [port] [players]   (default port 7000, 2 players)
#   netplay/play.sh join <game> <address>          (the address the host sent you)
# ROMs are looked for in $NP_ROMS, by default ~/mame-roms.
# Everybody plays with the player 1 controls, whatever seat they get.
set -e
cd "$(dirname "$0")/.."
mode=$1; game=$2
usage() { sed -n 2,6p "$0"; exit 1; }
[ -n "$game" ] || usage
case "$mode" in
  host) NET=(-netplay_host "${3:-7000}" -netplay_players "${4:-2}")
        EXCL=$(awk -F'\t' -v g="$game" '$1==g{print $2}' netplay/exclude.tsv)
        [ -n "$EXCL" ] && NET+=(-netplay_exclude "$EXCL") ;;
  join) [ -n "$3" ] || usage; NET=(-netplay_join "$3") ;;
  *) usage ;;
esac
[ -x ./np ] || { echo "./np is missing: build it with netplay/build-macos.sh"; exit 1; }
mkdir -p netplay/cfg netplay/nvram
exec ./np "$game" -rompath "${NP_ROMS:-$HOME/mame-roms}" -window -skip_gameinfo -nonvram_save \
  -cfg_directory netplay/cfg -nvram_directory netplay/nvram "${NET[@]}"
