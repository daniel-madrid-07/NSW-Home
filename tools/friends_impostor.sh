#!/bin/bash
# What someone who has a user's friend code can do, over the real network.
# Version 2 of friends: the stranger reads the card (nickname and colour
# only), finds no status, cannot open the requests in the inbox, writes a
# card of its own over the user's and fills one inbox place; the user's
# friend must keep seeing the user, the user must notice and write the card
# back, and a newcomer looking the code up must never be shown the impostor.
# The run itself is tools/friends_net.py's "stranger" scenario:
#
#   bash tools/friends_impostor.sh <work folder>
set -u
here="$(cd "$(dirname "$0")/.." && pwd)"
py=python; command -v python >/dev/null || py=python3
"$py" "$here/tools/friends_net.py" "$1" stranger "${@:2}"
