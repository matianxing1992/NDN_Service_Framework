#!/usr/bin/env python3
"""Bounded nvidia-smi sampler with UUID-preserving records."""
from __future__ import annotations
import argparse,csv,json,subprocess,time
from pathlib import Path
FIELDS=['uuid','name','utilization.gpu','memory.used','memory.total']
def parse(text):
 rows=[]
 for row in csv.reader(text.splitlines(),skipinitialspace=True):
  if len(row)!=5:continue
  rows.append({'uuid':row[0].strip(),'model':row[1].strip(),'utilizationPercent':int(row[2]),'memoryUsedMiB':int(row[3]),'memoryTotalMiB':int(row[4])})
 return rows
def main():
 p=argparse.ArgumentParser();p.add_argument('--seconds',type=int,required=True);p.add_argument('--interval',type=float,default=1);p.add_argument('--output',required=True);a=p.parse_args();deadline=time.monotonic()+a.seconds;rows=[]
 while time.monotonic()<deadline:
  r=subprocess.run(['nvidia-smi','--query-gpu='+','.join(FIELDS),'--format=csv,noheader,nounits'],text=True,capture_output=True,check=True);rows.append({'monotonicSeconds':time.monotonic(),'gpus':parse(r.stdout)});time.sleep(a.interval)
 Path(a.output).write_text(json.dumps({'schemaVersion':'1.0','samples':rows},indent=2,sort_keys=True)+'\n')
if __name__=='__main__':main()
