# Capturing diagnostics for the "hollow / phasey audio" bug

If you're hitting the bug where audio sounds hollow, phasey, mids-scooped, or
karaoke-like — and unplugging/replugging your headphones fixes it — please
run a diagnostic capture and send it back. This helps us track down what
Schwung is doing differently from stock Move.

You'll need:

- A Mac (or Linux) with `ssh` and `scp` (built into Terminal).
- Your Move on the same network, reachable at `move.local`.
- Schwung 0.9.9 or later installed.

## Steps

**1. Reproduce the bug.** Boot Move with headphones plugged in. Play
something with stereo content (a chord with chorus or detune is ideal).
If the audio sounds normal, reboot and try again — the bug is intermittent
per boot. Don't replug yet.

**2. Open Terminal on your Mac and run:**

```sh
ssh -t ableton@move.local /data/UserData/schwung/scripts/collect-diagnostics.sh
```

The `-t` flag streams progress live (without it, you'll see nothing for 60 seconds, then everything at once).
The script will print a 60-second countdown.

**3. While the countdown runs:** unplug your headphones, wait ~3 seconds,
plug them back in. Do this **twice** during the 60-second window. Try to do
it in the first 30 seconds.

**4. When the script finishes, copy the bundle to your Mac:**

```sh
scp ableton@move.local:/data/UserData/schwung/diagnostics-*.tgz ~/Desktop/
```

**5. Send the resulting `.tgz` file** (it'll be on your Desktop) to Charles
on Slack/Discord/email. The bundle is small (a few KB) and contains:

- MIDI traffic captured during your replug events
- Schwung's debug log (last 200 KB)
- System info: kernel version, schwung version, loaded modules, free disk

Nothing personal — no audio, no project files, no display content.

## What we're looking for

The capture pinpoints whether the bug is:

- **MIDI corruption between Schwung and the XMOS audio chip** (something
  Schwung does to the SPI mailbox).
- **A boot-init race** that leaves XMOS in a stuck routing state.
- **An XMOS firmware issue** that Schwung happens to expose.

A single capture from a confirmed-bad-state device tells us more than hours
of testing on units where we can't reproduce it.

## Troubleshooting

**`Permission denied` on SSH:**
The default Move SSH user is `ableton` with no password. If your Move is
configured differently, replace `ableton@move.local` with your username.

**`No such file: collect-diagnostics.sh`:**
The script ships with Schwung 0.9.9+. Update Schwung to the latest version
and try again.

**`move.local: nodename nor servname provided`:**
Your Move isn't on the same network as your Mac, or mDNS isn't resolving.
Find the Move's IP via your router's admin page and use that instead of
`move.local`.
