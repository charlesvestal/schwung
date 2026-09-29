# one-time: resolve class member offsets + Song pointer, store in offs.json
import json
from model import *
from rtti import instances
CLASSES = ['live.Song','live.Transport','live.Parameter','live.TrackList','live.Track','live.Clips','live.PlayingState',
           'live.ClipSlot','live.SessionClip','live.Clip','live.ClipRegion','live.Loop','live.TimeSignature','live.Label','live.ViewData']
out = {'offs': {}}
for c in CLASSES:
    for n, t, o, _ in all_members(cls_by_name(c)):
        out['offs'][c + '.' + n] = o
song = [a for vp, l in instances('N7ableton10flip_model5FSongE').items() for a in l]
out['song'] = min(song)
out['base'] = BASE
out['pid'] = PID
json.dump(out, open('offs.json', 'w'), indent=1)
print(hex(out['song']), len(out['offs']))
