#!/usr/bin/env python3
import argparse,json,os,sys,time
p=argparse.ArgumentParser();p.add_argument('--path',required=True);p.add_argument('--bytes',type=int,default=67108864);a=p.parse_args()
if not a.path.startswith('/tmp/ndnsf-di-'):print('SCRATCH_PATH_POLICY_INVALID',file=sys.stderr);sys.exit(3)
os.makedirs(a.path,exist_ok=True);path=os.path.join(a.path,'fsync-probe.bin');start=time.monotonic()
with open(path,'wb') as f:
 remaining=a.bytes;chunk=b'\0'*1048576
 while remaining: size=min(remaining,len(chunk));f.write(chunk[:size]);remaining-=size
 f.flush();os.fsync(f.fileno())
os.unlink(path);print(json.dumps({'status':'PASS','bytes':a.bytes,'seconds':time.monotonic()-start},sort_keys=True))
