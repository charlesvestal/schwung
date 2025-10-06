#!/usr/bin/env bash
echo ">>>>>>>>>>>>>>>>>>>>>>>>Change Page Relative by $0 $1 $2"

delta=$1

if ! [[ "${delta}" =~ ^-?[0-9]+$ ]]; then
    echo "Invalid delta '${delta}', expected integer."
    exit 1
fi
current_page=$(ls -la /data/UserData/UserLibrary 2>/dev/null | grep -o '[0-9]*$')
current_page=${current_page:-0}

echo "Current page: ${current_page}"

new_page=$((current_page + delta))

if (( new_page < 0 )); then
    echo "Already at first page; reloading page 0."
    new_page=0
fi

echo "Target page: ${new_page}"

/data/UserData/control_surface_move/changePage.sh ${new_page}
