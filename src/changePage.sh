#!/usr/bin/env bash

# uncomment to debug
# set -x

seed_initial_page_from_base() {
    local page="$1"
    local base_root="$2"
    local page_root="$3"

    if [ "${page}" -ne 0 ]; then
        return
    fi

    local base_sets="${base_root}/Sets"
    local page_sets="${page_root}/Sets"

    if [ ! -d "${base_sets}" ]; then
        return
    fi

    mkdir -p "${page_sets}"

    for src_uuid in "${base_sets}"/*; do
        [ -d "${src_uuid}" ] || continue
        local uuid
        uuid=$(basename "${src_uuid}")
        local dst_uuid="${page_sets}/${uuid}"

        if [ ! -d "${dst_uuid}" ]; then
            echo "Seeding base set ${uuid} into page 0..."
            if cp -a "${src_uuid}" "${page_sets}/" 2>/dev/null; then
                :
            else
                mkdir -p "${dst_uuid}"
                cp -R "${src_uuid}"/. "${dst_uuid}/" 2>/dev/null
            fi
        fi
    done

    if command -v getfattr >/dev/null 2>&1 && command -v setfattr >/dev/null 2>&1; then
        for src_uuid in "${base_sets}"/*; do
            [ -d "${src_uuid}" ] || continue
            local uuid
            uuid=$(basename "${src_uuid}")
            local dst_uuid="${page_sets}/${uuid}"
            [ -d "${dst_uuid}" ] || continue

            local existing_index
            existing_index=$(getfattr --only-values -n user.song-index "${dst_uuid}" 2>/dev/null | tr -d '\n')
            if [[ "${existing_index}" =~ ^[0-9]+$ ]]; then
                continue
            fi

            local attr
            for attr in user.song-index user.song-color user.last-modified-time user.was-externally-modified user.local-cloud-state; do
                local value
                value=$(getfattr --only-values -n "${attr}" "${src_uuid}" 2>/dev/null | tr -d '\n')
                if [ -n "${value}" ]; then
                    setfattr -n "${attr}" -v "${value}" "${dst_uuid}" 2>/dev/null || true
                fi
            done
        done
    else
        echo "getfattr/setfattr missing; skipping metadata sync for page 0."
    fi
}

PAGE="$1"

if [ -z "${PAGE}" ]; then
    echo "No page number provided."
    exit 1
fi

if ! [[ "${PAGE}" =~ ^-?[0-9]+$ ]]; then
    echo "Invalid page number: ${PAGE}"
    exit 1
fi

if (( PAGE < 0 )); then
    echo "Page is less than zero, ignoring..."
    exit 0
fi

echo "Change Page to ${PAGE}"

killall MoveLauncher MoveMessageDisplay Move MoveOriginal control_surface_move 2>/dev/null || true

echo "Waiting for Move binaries to exit..."
sleep 0.5

USER_DATA=/data/UserData
BASE="${USER_DATA}/UserLibrary_base"
DIR="UserLibrary_${PAGE}"
PAGE_DIR="${USER_DATA}/${DIR}"

# Ensure base library exists. If the original UserLibrary is a real folder,
# move it to UserLibrary_base. Otherwise create an empty base skeleton.
if [ ! -d "${BASE}" ]; then
    if [ -d "${USER_DATA}/UserLibrary" ] && [ ! -L "${USER_DATA}/UserLibrary" ]; then
        echo "Initializing UserLibrary_base from existing UserLibrary..."
        mv "${USER_DATA}/UserLibrary" "${BASE}"
    else
        echo "Creating empty UserLibrary_base..."
        mkdir -p "${BASE}/Sets" \
                 "${BASE}/Samples" \
                 "${BASE}/Recordings" \
                 "${BASE}/Track Presets" \
                 "${BASE}/Audio Effects"
    fi
fi

if [ ! -d "${PAGE_DIR}" ]; then
    echo "Page ${PAGE} doesn't exist, creating..."
    mkdir -p "${PAGE_DIR}/Sets"

    ln -snf "${BASE}/Samples"        "${PAGE_DIR}/Samples"
    ln -snf "${BASE}/Recordings"      "${PAGE_DIR}/Recordings"
    ln -snf "${BASE}/Track Presets"   "${PAGE_DIR}/Track Presets"
    ln -snf "${BASE}/Audio Effects"   "${PAGE_DIR}/Audio Effects"
fi

seed_initial_page_from_base "${PAGE}" "${BASE}" "${PAGE_DIR}"

echo "${DIR}"
rm -f "${USER_DATA}/UserLibrary"
ln -s "${DIR}" "${USER_DATA}/UserLibrary"

if [ "$2" = "skipLaunch" ]; then
    echo "Skipping launch..."
else
    /opt/move/MoveLauncher
fi
