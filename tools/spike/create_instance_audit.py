#!/usr/bin/env python3
"""Audit what every plugin's create_instance reaches.

Run from schwung-parent/ (the directory holding all the module repos):

    python3 schwung/tools/spike/create_instance_audit.py

For each repo, resolve `.create_instance = <fn>` to the real function (it is
almost never literally named create_instance, which is why a naive grep
under-reports), brace-match its body, transitively expand every locally defined
function it calls in the same translation unit, and report the host_api_v1_t
members reached plus any thread/IO calls.

Findings and what they mean for residual 2.6 are in
docs/plans/2026-08-21-create-instance-thread-spike.md. Re-run before relying on
that document: the fleet is allowed to change under us.
"""
import re,os,glob
roots=[d for d in glob.glob('schwung-*')+glob.glob('move-anything-*') if os.path.isdir(d+'/.git') and 'backup' not in d]
HOSTCALL=re.compile(r'->\s*(log|midi_send_internal|midi_send_external|midi_inject_to_move|mod_emit_value|mod_clear_source|get_clock_status|get_bpm|get_beat_position|slot_recv_channel|mapped_memory|sample_rate|frames_per_block|audio_in_offset|audio_out_offset)\b')
RISK=re.compile(r'\b(pthread_create|dlopen|fopen|open\s*\(|system\s*\(|popen|curl_easy_perform|mmap|shm_open)\b')
def fnbody(src,name):
    for m in re.finditer(r'(?<![\w.])'+re.escape(name)+r'\s*\(',src):
        i=m.end()-1;d=0;j=i
        while j<len(src):
            if src[j]=='(':d+=1
            elif src[j]==')':
                d-=1
                if d==0:break
            j+=1
        k=j+1
        while k<len(src) and src[k] in ' \t\r\n':k+=1
        if k<len(src) and src[k]=='{':
            d=0;e=k
            while e<len(src):
                if src[e]=='{':d+=1
                elif src[e]=='}':
                    d-=1
                    if d==0:break
                e+=1
            return src[k:e+1]
    return None
CALL=re.compile(r'(?<![\w.>])([a-zA-Z_]\w{2,})\s*\(')
KW={'if','for','while','switch','return','sizeof','snprintf','memset','memcpy','calloc','malloc','free','strcmp','strncmp','strcpy','strncpy','atoi','atof','strdup','printf','fprintf','sprintf','strlen','realloc','static_cast','reinterpret_cast','const_cast','dynamic_cast','new','delete','catch','strstr','strchr','sscanf'}
for r in sorted(roots):
    files=[f for f in glob.glob(r+'/src/**/*',recursive=True) if f.endswith(('.c','.cpp','.cc'))]
    for f in files:
        try:s=open(f,errors='ignore').read()
        except:continue
        m=re.search(r'\.create_instance\s*=\s*([A-Za-z_]\w*)',s)
        if not m:continue
        fn=m.group(1)
        seen=set();stack=[fn];host=set();risk=set()
        while stack:
            n=stack.pop()
            if n in seen or len(seen)>60:continue
            seen.add(n)
            b=fnbody(s,n)
            if b is None:continue
            host|=set(HOSTCALL.findall(b))
            risk|=set(x.strip('( ') for x in RISK.findall(b))
            for c in CALL.findall(b):
                if c not in KW and c not in seen: stack.append(c)
        print(f"{r:26s} {os.path.basename(f):24s} entry={fn:22s} host={sorted(host)} risk={sorted(risk)}")
