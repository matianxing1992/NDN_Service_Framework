"""Legacy Slurm/Apptainer adapter with fail-closed submission.

The ``materialize`` method belongs to the older Spec108/110 OCI-to-SIF
workflow. Current Spec170 releases do not enter through this adapter: they
start with the repository's local ``build-local-sif.sh`` command, then pass a
hash-verified SIF directly to TigerCluster for execution.
"""
from __future__ import annotations
import hashlib,importlib.util,json,math,os,re,stat,subprocess,sys,time
from pathlib import Path
from typing import Any,Callable,Mapping,Sequence
if __package__:
    from .base import Adapter
else:
    from adapters.base import Adapter

def _load(name):
    key='ndnsf_container_'+name; value=sys.modules.get(key)
    if value is not None:return value
    spec=importlib.util.spec_from_file_location(key,Path(__file__).resolve().parents[1]/(name+'.py'))
    value=importlib.util.module_from_spec(spec);sys.modules[key]=value;spec.loader.exec_module(value);return value

class SlurmAdapterError(RuntimeError): pass
SAFE=re.compile(r'^[A-Za-z0-9._/:@+-]+$')
TERMINAL={'COMPLETED','FAILED','TIMEOUT','CANCELLED','PREEMPTED','OUT_OF_MEMORY','NODE_FAIL'}

def _timeout(value):
    if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
        raise SlurmAdapterError('SLURM_TIMEOUT_INVALID')
    return value

def _version(value,label):
    if not isinstance(value,str) or re.fullmatch(r'[0-9]+(?:\.[0-9]+)+',value) is None:raise SlurmAdapterError('RUNTIME_VERSION_INVALID:'+label)
    return tuple(int(x) for x in value.split('.'))

def validate_runtime_release(release_record:Mapping[str,Any],materialization:Mapping[str,Any],cluster_snapshot:Mapping[str,Any])->dict[str,Any]:
    _release=_load('release')
    try:_release.validate_immutable_gpu_release_record(dict(release_record))
    except _release.ReleaseError as exc:raise SlurmAdapterError('RUNTIME_RELEASE_INVALID:'+str(exc)) from exc
    if materialization.get('verified') is not True:raise SlurmAdapterError('RUNTIME_SIF_NOT_VERIFIED')
    if materialization.get('ociReference')!=release_record.get('imageReference'):raise SlurmAdapterError('RUNTIME_OCI_REFERENCE_MISMATCH')
    if materialization.get('ociDigest')!=release_record.get('imageDigest'):raise SlurmAdapterError('RUNTIME_OCI_DIGEST_MISMATCH')
    path=Path(str(materialization.get('sifPath','')))
    if not path.is_file():raise SlurmAdapterError('RUNTIME_SIF_MISSING')
    if _release.sha256_file(path)!=materialization.get('sifSha256'):raise SlurmAdapterError('RUNTIME_SIF_TAMPERED')
    driver=cluster_snapshot.get('driverCuda')
    versions=cluster_snapshot.get('apptainerVersions')
    if not isinstance(driver,Mapping) or _version(driver.get('driver'),'driver') < (550,54,14):raise SlurmAdapterError('RUNTIME_DRIVER_TOO_OLD')
    if not isinstance(versions,Mapping) or not versions.get('compute'):raise SlurmAdapterError('RUNTIME_COMPUTE_APPTAINER_MISSING')
    return {'status':'PASS','releaseId':release_record['releaseId'],'imageDigest':release_record['imageDigest'],'sifSha256':materialization['sifSha256'],'driverVersion':driver['driver'],'computeApptainerVersion':versions['compute'],'physicalProduction':'DEFERRED'}

def _safe(value,label):
    text=str(value)
    if not SAFE.fullmatch(text):raise SlurmAdapterError('SLURM_UNSAFE_'+label)
    return text

def _require_single_node_legacy_path(profile:Mapping[str,Any]) -> None:
    """Keep the compatibility adapter from silently dropping extra nodes."""
    if profile['slurm']['nodes'] > 1:
        raise SlurmAdapterError(
            'SLURM_MULTINODE_TOPOLOGY_RUNNER_REQUIRED:'
            ' legacy single-container sbatch cannot execute process-map/routes;'
            ' use run-allocation-topology.sh'
        )

def render_sbatch(profile:Mapping[str,Any],template_path:Path|str,materialization:Mapping[str,Any]|None=None)->str:
    _load('profile').validate_profile(dict(profile)); _require_single_node_legacy_path(profile); s=profile['slurm']; root=Path(__file__).resolve().parents[2]
    gpu_count=s['gpu']['count']
    gpu_directive='' if gpu_count == 0 else f'#SBATCH --gres=gpu:{s["gpu"]["type"]}:{gpu_count}'
    values={'JOB_NAME':s.get('jobName','ndnsf-di'),'PARTITION':s['partition'],'ACCOUNT':s.get('account') or 'devs','QOS':s.get('qos') or 'normal','WALL_TIME':s['wallTime'],'NODES':s['nodes'],'TASKS_PER_NODE':s['tasksPerNode'],'CPUS_PER_TASK':s['cpusPerTask'],'MEMORY':s['memory'],'GPU_TYPE':s['gpu']['type'],'GPU_COUNT':gpu_count,'RUN_ID':profile['runId'],'PROJECT_ROOT':profile['storage']['projectRoot'],'EVIDENCE_ROOT':profile['storage']['evidenceRoot'],'IDENTITY_ROOT':profile['identity']['reference'],'LOG_ROOT':profile['storage']['projectRoot']+'/logs/'+profile['runId'],'FINALIZER':str(root/'adapters/slurm-apptainer/scripts/finalize-evidence.sh'),'COMPUTE_PREFLIGHT':str(root/'adapters/slurm-apptainer/scripts/preflight-compute.sh'),'RUN_CONTAINER':str(root/'adapters/slurm-apptainer/scripts/run-container.sh'),'SIF_PATH':(materialization or {}).get('sifPath',profile['storage']['imageRoot']+'/current.sif'),'SIF_SHA256':(materialization or {}).get('sifSha256','sha256:'+'0'*64),'WORKLOAD':'/bin/true'}
    text=Path(template_path).read_text()
    text=text.replace('@@GPU_DIRECTIVE@@',gpu_directive)
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
    def _run(self,cmd:Sequence[str],*,timeout=None,input=None):
        options = {} if timeout is None else {'timeout': _timeout(timeout)}
        if input is not None:
            options['input'] = input
            options['encoding'] = 'utf-8'
        try:
            r=self.runner(list(cmd),text=True,capture_output=True,check=False,**options)
        except subprocess.TimeoutExpired as exc:
            raise SlurmAdapterError('SLURM_COMMAND_TIMEOUT:'+str(cmd[0])) from exc
        if r.returncode:raise SlurmAdapterError('SLURM_COMMAND_FAILED:'+str(cmd[0])+':'+(r.stderr or r.stdout).strip())
        return r
    def preflight(self,profile):
        _load('profile').validate_profile(profile);r=self._run(['sinfo','-h','-p',profile['slurm']['partition'],'-o','%P|%N|%G|%T']);return {'status':'PASS','sinfo':r.stdout}
    def materialize(self,profile):
        # Kept for Spec108/110 compatibility and regression tests only.  The
        # current Spec170 route never calls this OCI-to-SIF operation.
        release=_load('release').load_release_manifest(profile['releaseManifest']);image=next(iter(release['images'].values()));sif=profile['storage']['imageRoot']+'/'+release['releaseId']+'.sif';record=sif+'.json';script=Path(__file__).resolve().parents[2]/'adapters/slurm-apptainer/scripts/materialize-sif.sh';self._run([str(script),'--oci-reference',image['reference'],'--sif',sif,'--record',record]);return json.load(open(record))
    def validate_release(self,release_record,materialization,cluster_snapshot):return validate_runtime_release(release_record,materialization,cluster_snapshot)
    def submit(self,profile,*,preflight=True,materialize=True):
        _load('profile').validate_profile(profile)
        _require_single_node_legacy_path(profile)
        if preflight:self.preflight(profile)
        materialization=self.materialize(profile) if materialize else None
        root=self.state_root or Path(profile['storage']['projectRoot'])/'.state';run=root/profile['runId']
        try:run.mkdir(parents=True,exist_ok=False)
        except FileExistsError:raise SlurmAdapterError('SLURM_RUN_ALREADY_SUBMITTED:'+profile['runId'])
        script=run/'job.sbatch';template=Path(__file__).resolve().parents[2]/'adapters/slurm-apptainer/templates/ndnsf-di.sbatch.in';script.write_text(render_sbatch(profile,template,materialization));r=self._run(['sbatch','--parsable',str(script)]);job=r.stdout.strip().split(';')[0]
        if not job.isdigit():raise SlurmAdapterError('SLURM_JOB_ID_INVALID')
        (run/'submission.json').write_text(json.dumps({'runId':profile['runId'],'jobId':job},sort_keys=True)+'\n');return {'status':'SUBMITTED','runId':profile['runId'],'jobId':job}
    start=submit
    def status(self,reference,*,timeout=30):
        job=str(reference);r=self._run(['sacct','-n','-P','-j',job,'-o','JobIDRaw,State,ExitCode,Elapsed,NodeList,ReqTRES,AllocTRES'],timeout=_timeout(timeout));return parse_sacct(r.stdout,job)
    def wait(self,job_id,timeout=600,poll=5):
        deadline=time.monotonic()+_timeout(timeout)
        if type(poll) not in (int, float) or not math.isfinite(poll) or poll < 0:
            raise SlurmAdapterError('SLURM_POLL_INVALID')
        while True:
            remaining=deadline-time.monotonic()
            if remaining <= 0:
                raise SlurmAdapterError('SLURM_WAIT_TIMEOUT')
            try:
                value=self.status(job_id,timeout=remaining)
            except SlurmAdapterError as exc:
                if str(exc).startswith('SLURM_COMMAND_TIMEOUT:'):
                    raise SlurmAdapterError('SLURM_WAIT_TIMEOUT') from exc
                raise
            if value['terminal']:return value
            remaining=deadline-time.monotonic()
            if remaining <= 0:
                raise SlurmAdapterError('SLURM_WAIT_TIMEOUT')
            self.sleeper(min(poll,remaining))
    def cancel(self,job_id,reason='operator-request',*,timeout=30):
        self._run(['scancel',str(job_id)],timeout=_timeout(timeout));return {'status':'CANCEL_REQUESTED','jobId':str(job_id),'reason':reason}
    def logs(self,reference):return {'status':'PASS','jobId':str(reference)}
    def evidence(self,reference):return self.status(reference)
    def stop(self,reference):return self.cancel(reference)

    @staticmethod
    def _deployment_comment(reference):
        required = {'schema', 'boot_id', 'configuration_sha256', 'release_sha256',
                    'script_sha256', 'script', 'owner_uid', 'submitted_after',
                    'job_id', 'job_name', 'cluster'}
        if not isinstance(reference, Mapping) or set(reference) != required:
            raise SlurmAdapterError('SLURM_REFERENCE_INVALID')
        if reference['schema'] != 'ndnsf-di-slurm-reference-v1':
            raise SlurmAdapterError('SLURM_REFERENCE_SCHEMA_INVALID')
        for key, length in [('boot_id', 32), ('configuration_sha256', 64),
                            ('release_sha256', 64), ('script_sha256', 64)]:
            if not isinstance(reference[key], str) or not re.fullmatch(
                    '[0-9a-f]{' + str(length) + '}', reference[key]):
                raise SlurmAdapterError('SLURM_REFERENCE_BINDING_INVALID:' + key)
        if type(reference['owner_uid']) is not int or reference['owner_uid'] != os.getuid():
            raise SlurmAdapterError('SLURM_REFERENCE_OWNER_MISMATCH')
        name = reference['job_name']
        if (not isinstance(name, str) or len(name) > 128 or
                not re.fullmatch('[A-Za-z0-9_.-]+', name) or
                not name.endswith('-' + reference['boot_id'])):
            raise SlurmAdapterError('SLURM_REFERENCE_JOB_NAME_INVALID')
        cluster = reference['cluster']
        if cluster is not None and (not isinstance(cluster, str) or
                                    not re.fullmatch('[A-Za-z0-9_.-]+', cluster)):
            raise SlurmAdapterError('SLURM_REFERENCE_CLUSTER_INVALID')
        job = reference['job_id']
        if job is not None and (not isinstance(job, str) or not re.fullmatch('[0-9]+', job)):
            raise SlurmAdapterError('SLURM_REFERENCE_JOB_ID_INVALID')
        since = reference['submitted_after']
        if type(since) not in (float, int) or not math.isfinite(since) or since < 0:
            raise SlurmAdapterError('SLURM_REFERENCE_TIME_INVALID')
        path = reference['script']
        if not isinstance(path, str) or not Path(path).is_absolute() or '..' in Path(path).parts:
            raise SlurmAdapterError('SLURM_REFERENCE_SCRIPT_INVALID')
        return 'ndnsf-di:' + ':'.join(reference[k] for k in (
            'boot_id', 'configuration_sha256', 'release_sha256'))

    @staticmethod
    def _deployment_script(path):
        path = Path(path)
        info = path.lstat()
        if (path.resolve() != path or not stat.S_ISREG(info.st_mode) or
                info.st_uid != os.getuid() or info.st_mode & 0o077 or
                info.st_size > 1024 * 1024):
            raise SlurmAdapterError('SLURM_DEPLOYMENT_SCRIPT_INVALID')
        with path.open('rb') as source:
            content = source.read(1024 * 1024 + 1)
        if len(content) > 1024 * 1024:
            raise SlurmAdapterError('SLURM_DEPLOYMENT_SCRIPT_INVALID')
        after = path.lstat()
        if (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns) != (
                after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns):
            raise SlurmAdapterError('SLURM_DEPLOYMENT_SCRIPT_CHANGED')
        return content

    def prepare_deployment(self, config, script, *, boot_id, release_sha256):
        """Prepare the sole reference that the locked caller must persist before submit."""
        if config.get('schema') != 'ndnsf-di-deployment-v1' or config['platform']['kind'] != 'slurm-apptainer':
            raise SlurmAdapterError('SLURM_NORMALIZED_DEPLOYMENT_REQUIRED')
        script = Path(script).absolute()
        reference = dict(schema='ndnsf-di-slurm-reference-v1', boot_id=boot_id,
                         configuration_sha256=hashlib.sha256(json.dumps(
                             config, sort_keys=True, separators=(',', ':')).encode()).hexdigest(),
                         release_sha256=release_sha256, script_sha256=hashlib.sha256(
                             self._deployment_script(script)).hexdigest(),
                         script=str(script), owner_uid=os.getuid(), submitted_after=max(0, time.time() - 60),
                         job_id=None, cluster=None,
                         job_name=config['platform']['slurm'].get('jobName', 'ndnsf-di') + '-' + boot_id)
        self._deployment_comment(reference)
        return reference

    def submit_deployment(self, config, reference, *, timeout=30):
        """Submit once; a timeout requires reconciliation, never another submission."""
        comment = self._deployment_comment(reference)
        if reference['job_id'] is not None:
            raise SlurmAdapterError('SLURM_DEPLOYMENT_ALREADY_SUBMITTED')
        digest = hashlib.sha256(json.dumps(config, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        if digest != reference['configuration_sha256']:
            raise SlurmAdapterError('SLURM_DEPLOYMENT_CONFIGURATION_CHANGED')
        content = self._deployment_script(reference['script'])
        if hashlib.sha256(content).hexdigest() != reference['script_sha256']:
            raise SlurmAdapterError('SLURM_DEPLOYMENT_SCRIPT_CHANGED')
        slurm = config['platform']['slurm']
        options = {'job-name': reference['job_name'], 'comment': comment,
                   'partition': slurm['partition'], 'account': slurm['account'],
                   'time': slurm['wallTime'], 'nodes': slurm['nodes'],
                   'ntasks-per-node': slurm['tasksPerNode'], 'cpus-per-task': slurm['cpusPerTask'],
                   'mem': slurm['memory'], 'export': 'NONE',
                   'chdir': str(Path(reference['script']).parent),
                   'output': str(Path(reference['script']).parent / 'slurm-%j.out'),
                   'error': str(Path(reference['script']).parent / 'slurm-%j.err')}
        if slurm.get('qos'):
            options['qos'] = slurm['qos']
        hosts = [node['host'] for node in config['nodes']]
        if any(not isinstance(host, str) or not re.fullmatch('[A-Za-z0-9][A-Za-z0-9_.-]*', host)
               for host in hosts):
            raise SlurmAdapterError('SLURM_DEPLOYMENT_HOST_INVALID')
        options['nodelist'] = ','.join(hosts)
        if slurm['gpu']['count']:
            options['gres'] = 'gpu:' + _safe(slurm['gpu']['type'], 'GPU') + ':' + str(slurm['gpu']['count'])
        command = ['sbatch', '--parsable'] + ['--' + key + '=' + str(value)
                                            for key, value in options.items()]
        try:
            script_text = content.decode('utf-8')
        except UnicodeError as exc:
            raise SlurmAdapterError('SLURM_DEPLOYMENT_SCRIPT_ENCODING_INVALID') from exc
        result = self._run(command, timeout=_timeout(timeout), input=script_text)
        match = re.fullmatch(r'([0-9]+)(?:;([A-Za-z0-9_.-]+))?', result.stdout.strip())
        if match is None:
            raise SlurmAdapterError('SLURM_JOB_ID_INVALID')
        return dict(reference, job_id=match.group(1), cluster=match.group(2))

    def deployment_status(self, reference, *, timeout=30):
        """Reconcile a boot/config/release binding without inferring actor cleanup."""
        comment = self._deployment_comment(reference)
        deadline = time.monotonic() + _timeout(timeout)
        cluster = ['--clusters=' + reference['cluster']] if reference['cluster'] else []
        queue = self._run(['squeue', '--noheader', '--user=' + str(reference['owner_uid']),
                           '--name=' + reference['job_name'], '--format=%i|%U|%k|%T|%N'] + cluster,
                          timeout=deadline - time.monotonic())
        since = time.strftime('%Y-%m-%dT%H:%M:%S', time.localtime(reference['submitted_after']))
        account = self._run(['sacct', '-n', '-P', '--user=' + str(reference['owner_uid']),
                             '--name=' + reference['job_name'], '--starttime=' + since,
                             '-o', 'JobIDRaw,UID,Comment%256,State%40,ExitCode,NodeList%1024,JobName%128,Submit%32'] + cluster,
                            timeout=deadline - time.monotonic())
        found = {}
        # A live queue record takes precedence over lagging accounting data.
        for output, active in ((account.stdout, False), (queue.stdout, True)):
            for line in output.splitlines():
                fields = [value.strip() for value in line.split('|')]
                if not fields or not re.fullmatch('[0-9]+', fields[0]):
                    continue  # Ignore steps and unrelated array task records.
                known = reference['job_id'] is not None and fields[0] == reference['job_id']
                if len(fields) != (5 if active else 8):
                    raise SlurmAdapterError('SLURM_DEPLOYMENT_QUERY_INVALID')
                # Some sites do not persist job comments in accounting. Bind
                # those rows through the exact boot-qualified name and submit
                # time from the private reference, never a numeric job ID alone.
                named_account = not active and fields[6] == reference['job_name']
                if fields[2] != comment and not known and not (named_account and not fields[2]):
                    continue
                if (fields[1] != str(reference['owner_uid']) or
                        ((active or fields[2]) and fields[2] != comment)):
                    raise SlurmAdapterError('SLURM_DEPLOYMENT_OWNERSHIP_MISMATCH')
                if not active:
                    try:
                        submitted = time.mktime(time.strptime(fields[7], '%Y-%m-%dT%H:%M:%S'))
                    except (ValueError, OverflowError):
                        raise SlurmAdapterError('SLURM_DEPLOYMENT_QUERY_INVALID')
                    if (not named_account or submitted < reference['submitted_after'] or
                            submitted > time.time() + 60):
                        raise SlurmAdapterError('SLURM_DEPLOYMENT_OWNERSHIP_MISMATCH')
                if reference['job_id'] is not None and not known:
                    raise SlurmAdapterError('SLURM_DEPLOYMENT_BINDING_AMBIGUOUS')
                states = fields[3].split('+')[0].split()
                if not states:
                    raise SlurmAdapterError('SLURM_DEPLOYMENT_QUERY_INVALID')
                state = states[0]
                terminal = not active and state in TERMINAL
                found[fields[0]] = dict(job_id=fields[0], state=state, terminal=terminal,
                                       exit_code=None if active else fields[4],
                                       node_list=fields[4] if active else fields[5],
                                       status='TERMINAL' if terminal else 'ACTIVE',
                                       live_verified=active,
                                       actor_cleanup='UNVERIFIED')
        if len(found) > 1:
            raise SlurmAdapterError('SLURM_DEPLOYMENT_BINDING_AMBIGUOUS')
        if not found:
            return dict(status='UNKNOWN', reference=dict(reference), terminal=False,
                        actor_cleanup='UNVERIFIED')
        value = next(iter(found.values()))
        value['reference'] = dict(reference, job_id=value['job_id'])
        if (value['terminal'] and value['state'] == 'CANCELLED' and
                value['node_list'] == 'None assigned'):
            # A cancelled pending job has no node audit. Require independent,
            # bound proof including requeued allocations, not only the latest record.
            proof = self._run(['sacct', '--duplicates', '-n', '-P', '-j', value['job_id'], '-o',
                               'JobIDRaw,UID,JobName%128,Start%32,NodeList%1024,'
                               'AllocNodes,AllocCPUS,ElapsedRaw'] + cluster,
                              timeout=deadline - time.monotonic())
            rows = [[field.strip() for field in line.split('|')]
                    for line in proof.stdout.splitlines()
                    if re.fullmatch('[0-9]+', line.split('|')[0].strip())]
            expected = [value['job_id'], str(reference['owner_uid']),
                        reference['job_name'], 'None', 'None assigned', '0', '0', '0']
            if rows == [expected]:
                value['never_allocated'] = True
                value['never_allocated_evidence'] = dict(zip(
                    ('job_id', 'owner_uid', 'job_name', 'start', 'node_list',
                     'allocated_nodes', 'allocated_cpus', 'elapsed_seconds'), expected))
        return value

    def cancel_deployment(self, reference, *, timeout=30):
        """Cancel only a currently verified job; cancellation is not a ZERO audit."""
        deadline = time.monotonic() + _timeout(timeout)
        value = self.deployment_status(reference, timeout=deadline - time.monotonic())
        if value['status'] == 'UNKNOWN' or value['terminal']:
            return value
        if not value.get('live_verified'):
            return dict(value, status='UNKNOWN', terminal=False)
        verified = value['reference']
        cluster = ['--clusters=' + verified['cluster']] if verified['cluster'] else []
        self._run(['scancel'] + cluster + [verified['job_id']],
                  timeout=deadline - time.monotonic())
        return dict(value, status='CANCEL_REQUESTED', terminal=False)
