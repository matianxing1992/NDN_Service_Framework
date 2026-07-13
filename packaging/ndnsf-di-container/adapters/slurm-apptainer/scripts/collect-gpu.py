#!/usr/bin/env python3
import argparse,csv,json,sys
parser=argparse.ArgumentParser()
for name in ('host','container','requested-gres','slurm-job-gpus','cuda-visible-devices'):
    parser.add_argument('--'+name,required=True)
args=parser.parse_args()
def read(path):
    result=[]
    for row in csv.reader(open(path,encoding='utf-8')):
        if not row:
            continue
        row=[item.strip() for item in row]
        result.append({'index':int(row[0]),'uuid':row[1],'model':row[2],'memoryMiB':int(row[3]),'driverVersion':row[4]})
    return result
host,container=read(args.host),read(args.container)
if not host or not container or {x['uuid'] for x in host}!={x['uuid'] for x in container}:
    print('GPU_UUID_MAPPING_MISMATCH',file=sys.stderr); raise SystemExit(3)
print(json.dumps({'status':'PASS','requestedGres':args.requested_gres,'slurmJobGpus':args.slurm_job_gpus,'cudaVisibleDevices':args.cuda_visible_devices,'host':host,'container':container},sort_keys=True))
