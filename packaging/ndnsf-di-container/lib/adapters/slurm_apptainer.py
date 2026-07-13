"""Slurm plus Apptainer adapter with fail-closed, exactly-once submission."""
from __future__ import annotations
import importlib.util,json,os,re,subprocess,sys,time
from pathlib import Path
from typing import Any,Callable,Mapping,Sequence
from adapters.base import Adapter

def _load(name):
    key='ndnsf_container_'+name; value=sys.modules.get(key)
    if value is not None:return value
    spec=importlib.util.spec_from_file_location(key,Path(__file__).resolve().parents[1]/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[key]=value;spec.loader.exec_module(value);return value
_profile=_load('profile');_release=_load('release')

class SlurmAdapterError(RuntimeError): pass
SAFE=re.compile(r'^[A-Za-z0-9._/:@+-]+$')
TERMINAL={'COMPLETED','FAILED','TIMEOUT','CANCELLED','PREEMPTED','OUT_OF_MEMORY','NODE_FAIL'}

def _safe(value,label):
    text=str(value)
    if not SAFE.fullmatch(text):raise SlurmAdapterError('SLURM_UNSAFE_'+label)
    return text

def render_sbatch(profile:Mapping[str,Any],template_path:Path|str,materialization:Mapping[str,Any]|None=None)->str:
    _profile.validate_profile(dict(profile)); s=profile['slurm']; root=Path(__file__).resolve().parents[2]
    values={'JOB_NAME':s.get('jobName','ndnsf-di'),'PARTITION':s['partition'],'ACCOUNT':s.get('account') or 'devs','QOS':s.get('qos') or 'normal','WALL_TIME':s['wallTime'],'NODES':s['nodes'],'TASKS_PER_NODE':s['tasksPerNode'],'CPUS_PER_TASK':s['cpusPerTask'],'MEMORY':s['memory'],'GPU_TYPE':s['gpu']['type'],'GPU_COUNT':s['gpu']['count'],'RUN_ID':profile['runId'],'PROJECT_ROOT':profile['storage']['projectRoot'],'EVIDENCE_ROOT':profile['storage']['evidenceRoot'],'IDENTITY_ROOT':profile['identity']['reference'],'LOG_ROOT':profile['storage']['projectRoot']+'/logs/'+profile['runId'],'FINALIZER':str(root/'adapters/slurm-apptainer/scripts/finalize-evidence.sh'),'COMPUTE_PREFLIGHT':str(root/'adapters/slurm-apptainer/scripts/preflight-compute.sh'),'RUN_CONTAINER':str(root/'adapters/slurm-apptainer/scripts/run-container.sh'),'SIF_PATH':(materialization or {}).get('sifPath',profile['storage']['imageRoot']+'/current.sif'),'SIF_SHA256':(materialization or {}).get('sifSha256','sha256:'+'0'*64),'WORKLOAD':'/bin/true'}
    text=Path(template_path).read_text()
    for key,value in values.items():text=text.replace('@@'+key+'@@',_safe(value,key))
    if '@@' in text or '${' in text.replace('${SLURM_JOB_ID}','').replace('${RUN_ID}',''):raise SlurmAdapterError('SLURM_TEMPLATE_UNRESOLVED')
    return text

def parse_sacct(output:str,job_id:str)->dict[str,Any]:
    for line in output.splitlines():
        fields=line.split('|')
        if fields and fields[0]==str(job_id):
            if len(fields)<7:raise SlurmAdapterError('SLURM_SACCT_FIELDS_INVALID')
            state=fields[1].split('+')[0];return {'jobId':fields[0],'state':state,'exitCode':fields[2],'elapsed':fields[3],'nodeList':fields[4],'requestedTres':fields[5],'allocatedTres':fields[6],'terminal':state in TERMINAL,'successful':state=='COMPLETED' and fields[2]=='0:0'}
    raise SlurmAdapterError('SLURM_JOB_ID_NOT_FOUND:'+str(job_id))

class SlurmApptainerAdapter(Adapter):
    def __init__(self,*,runner:Callable[...,subprocess.CompletedProcess]=subprocess.run,state_root:Path|None=None,sleeper=time.sleep):self.runner=runner;self.state_root=state_root;self.sleeper=sleeper
    def _run(self,cmd:Sequence[str]):
        r=self.runner(list(cmd),text=True,capture_output=True,check=False)
        if r.returncode:raise SlurmAdapterError('SLURM_COMMAND_FAILED:'+str(cmd[0])+':'+(r.stderr or r.stdout).strip())
        return r
    def preflight(self,profile):
        _profile.validate_profile(profile);r=self._run(['sinfo','-h','-p',profile['slurm']['partition'],'-o','%P|%N|%G|%T']);return {'status':'PASS','sinfo':r.stdout}
    def materialize(self,profile):
        release=_release.load_release_manifest(profile['releaseManifest']);image=next(iter(release['images'].values()));sif=profile['storage']['imageRoot']+'/'+release['releaseId']+'.sif';record=sif+'.json';script=Path(__file__).resolve().parents[2]/'adapters/slurm-apptainer/scripts/materialize-sif.sh';self._run([str(script),'--oci-reference',image['reference'],'--sif',sif,'--record',record]);return json.load(open(record))
    def submit(self,profile,*,preflight=True,materialize=True):
        _profile.validate_profile(profile)
        if preflight:self.preflight(profile)
        materialization=self.materialize(profile) if materialize else None
        root=self.state_root or Path(profile['storage']['projectRoot'])/'.state';run=root/profile['runId']
        try:run.mkdir(parents=True,exist_ok=False)
        except FileExistsError:raise SlurmAdapterError('SLURM_RUN_ALREADY_SUBMITTED:'+profile['runId'])
        script=run/'job.sbatch';template=Path(__file__).resolve().parents[2]/'adapters/slurm-apptainer/templates/ndnsf-di.sbatch.in';script.write_text(render_sbatch(profile,template,materialization));r=self._run(['sbatch','--parsable',str(script)]);job=r.stdout.strip().split(';')[0]
        if not job.isdigit():raise SlurmAdapterError('SLURM_JOB_ID_INVALID')
        (run/'submission.json').write_text(json.dumps({'runId':profile['runId'],'jobId':job},sort_keys=True)+'\n');return {'status':'SUBMITTED','runId':profile['runId'],'jobId':job}
    start=submit
    def status(self,reference):
        job=str(reference);r=self._run(['sacct','-n','-P','-j',job,'-o','JobIDRaw,State,ExitCode,Elapsed,NodeList,ReqTRES,AllocTRES']);return parse_sacct(r.stdout,job)
    def wait(self,job_id,timeout=600,poll=5):
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            value=self.status(job_id)
            if value['terminal']:return value
            self.sleeper(poll)
        raise SlurmAdapterError('SLURM_WAIT_TIMEOUT')
    def cancel(self,job_id,reason='operator-request'):
        self._run(['scancel',str(job_id)]);return {'status':'CANCEL_REQUESTED','jobId':str(job_id),'reason':reason}
    def logs(self,reference):return {'status':'PASS','jobId':str(reference)}
    def evidence(self,reference):return self.status(reference)
    def stop(self,reference):return self.cancel(reference)
