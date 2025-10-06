cat << 'EOM'
 __  __                   ____            _             _   ____              __              
|  \/  | _____   _____   / ___|___  _ __ | |_ _ __ ___ | | / ___| _   _ _ __ / _| __ _  ___ ___ 
| |\/| |/ _ \ \ / / _ \ | |   / _ \| '_ \| __| '__/ _ \| | \___ \| | | | '__| |_ / _` |/ __/ _ \
| |  | | (_) \ V /  __/ | |__| (_) | | | | |_| | | (_) | |  ___) | |_| | |  |  _| (_| | (_|  __/
|_|  |_|\___/ \_/ \___|  \____\___/|_| |_|\__|_|  \___/|_| |____/ \__,_|_|  |_|  \__,_|\___\___|

EOM

# uncomment to debug
# set -x

function yes_or_no {
    while true; do
        read -p "$* [y/N]: " yn < /dev/tty

        case $yn in
            [Yy]*) return 0  ;;
            [Nn]*|"") return 1 ;;
        esac
    done
}

install_pages=true
url=https://github.com/bobbydigitales/control_surface_move/raw/main/

if [ "$1" = "local" ]; then
  url="file:/Users/rob/OrbStack/debian/home/rob/Projects/control_surface_move/"
fi

filename=control_surface_move.tar.gz
hostname=move.local
username=ableton
ssh_ableton="ssh -o LogLevel=QUIET -n $username@$hostname"

ssh_root="ssh -o LogLevel=QUIET -n root@$hostname"

echo "Downloading build...$url$filename"
curl -LO "$url$filename"
echo "Build MD5: `md5sum $filename`"

echo "Connecting via ssh to $ssh_ableton..."
if ! $ssh_ableton -o ConnectTimeout=1 ls &> /dev/null
then
    echo
    echo "Error: Could not connect to move.local using SSH."
    echo "Check that your Move is connected to the same network as this device"
    echo "and that you have added your keys at http://move.local/development/ssh"
    exit
fi

# $ssh_ableton rm -fr ./control_surface_move
scp  -o ConnectTimeout=1 $filename ableton@move.local:.
$ssh_ableton "tar -xvf ./$filename"

$ssh_ableton "killall MoveLauncher Move MoveOriginal"

$ssh_root cp -aL /data/UserData/control_surface_move/control_surface_move_shim.so /usr/lib/
$ssh_root chmod u+s /usr/lib/control_surface_move_shim.so
if $ssh_root "test ! -f /opt/move/MoveOriginal"; then
  $ssh_root mv /opt/move/Move /opt/move/MoveOriginal
  $ssh_ableton cp /opt/move/MoveOriginal ~/
fi

$ssh_root chmod +x /data/UserData/control_surface_move/Move.sh
$ssh_root cp /data/UserData/control_surface_move/Move.sh /opt/move/Move

$ssh_root md5sum /opt/move/Move
$ssh_root md5sum /opt/move/MoveOriginal
$ssh_root md5sum /usr/lib/control_surface_move_shim.so

if yes_or_no "Install 'Pages of Sets'?"; then

  if $ssh_ableton "test ! -L ~/UserLibrary"; then
    echo "Installing pages of sets..."
    $ssh_ableton "mv ~/UserLibrary ~/UserLibrary_base"
    
    $ssh_ableton "/data/UserData/control_surface_move/changePage.sh 0 skipLaunch"
    
    $ssh_ableton "cp -a ~/UserLibrary_base/Sets/* ~/UserLibrary_0/Sets/"

    
  else
    echo "Pages of Sets already installed!"
  fi

  echo
  echo "Pages of Sets usage:"
  echo " - To change pages, hold Shift and press the left (<) or right (>) arrow on the Move."
  echo " - Changing pages restarts the current Live session."
  echo " - Sets are per page; Samples/Recordings/Track Presets/Audio Effects are shared."
  echo " - The web UI at http://move.local shows Sets for the current page."
else
    echo "Not installing Pages of Sets"
fi

echo "Restating Move binary with shim installed..."


$ssh_ableton "nohup /opt/move/MoveLauncher 2>/dev/null 1>/dev/null &" &

echo "Done!"
