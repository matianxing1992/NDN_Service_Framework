#!/usr/bin/env python3
"""Run the YOLO hybrid split across two real TP Providers.

The driver owns only process lifecycle and private NFD/PIB/TPM setup.  The
request, grant verification, provider execution, response decoding, and
numerical oracle remain in the C++ production executables.
"""

import argparse
import sys, json
import hashlib, os, shutil, subprocess, tempfile, time, sqlite3, struct
from cryptography.hazmat.primitives.serialization import load_pem_public_key, Encoding, PublicFormat
from cryptography.hazmat.backends import default_backend
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tests/standalone'))
import importlib.util
s=importlib.util.spec_from_file_location('b1', str(ROOT/'tests/standalone/run-spec182-native-grant-process.py')); b=importlib.util.module_from_spec(s); s.loader.exec_module(b)
b.install_termination_handlers()
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=Path(os.environ.get("SPEC185_BUILD_DIR", ROOT / "build")))
parser.add_argument('--run-root', type=Path, default=None,
                    help='retain the raw run under this directory')
parser.add_argument('--cache', type=Path, default=None)
parser.add_argument('--variant', choices=['26n','11n'], default='26n')
parser.add_argument('--boundary', default='onnx-node-3')
parser.add_argument('--restart-provider2', action='store_true',
                    help='stop and restart the second Provider before a second request')
parser.add_argument('--revoke', action='store_true',
                    help='revoke the Provider identity from the real Controller before the request')
args=parser.parse_args()
BUILD=args.build.resolve()
CONTROLLER=b.native_program(BUILD, 'App_ServiceController'); AUTHORITY=b.native_program(BUILD, 'DI_NativeArtifactAuthority'); PROVIDER=b.native_program(BUILD, 'di-native-provider'); REQUESTER=b.native_program(BUILD, 'DI_NativeRequester'); WORKER=b.native_program(BUILD, "DI_NativeOnnxAssemblyWorker", source_subdir="", installed_subdir="libexec/ndnsf-di")
# make descriptor/catalog.  These helpers are deliberately pure Python fixture
# metadata; the process driver must not import the native Python wrapper when
# the selected C++ binaries are sanitizer-instrumented.
sys.path.insert(0, str(ROOT/'tests/standalone'))
from di_native_fixture import adapter_descriptor, canonical_bytes, digest, model_descriptor, fixture_path
def dg(x): return 'sha256:'+hashlib.sha256(x.encode()).hexdigest()
YOLO = {
 '26n': {'dir': Path(os.environ.get('SPEC261_ASSET_ROOT','/usr/local/lib/ndnsf/spec261/models'))/'yolo26n', 'name': 'YOLO26n',
   'planning': 'sha256:0dda659f16ce8efa7f81e95a7e6a3df1753550a9281e8618e39103464b969302',
   'canonical': 'sha256:55c7615315259bcdcfa1e7d4a11a899eec24656358302b5595e29057305d6a55',
   'initializer': 'sha256:f0820471225a9492718ae3cf637f34304e14734567d6ec698c6854642c0ed09b',
   'output': 'predictions',
   'conv_input': '/model/model.0/act/Mul_output_0', 'conv_weight': 'onnx::Conv_1568',
   'conv_bias': 'onnx::Conv_1569', 'conv_output': '/model/model.1/conv/Conv_output_0',
   'max_nodes': 1024},
 '11n': {'dir': Path(os.environ.get('SPEC261_ASSET_ROOT','/usr/local/lib/ndnsf/spec261/models'))/'yolo11n', 'name': 'YOLO11n',
   'planning': 'sha256:78ceb0afd63a3a4270ce5bcb9556425d358b48c977a10a54c1f029926c5782fb',
   'canonical': 'sha256:e8a26e0045c331dc0a58602dda602c66fc5c5877c973bfaf73ff7738c0d147f3',
   'initializer': 'sha256:7655f01d6f5f721c7b2b418570833cd8e0e81a0e2065b477474c5e108079190e',
   'output': 'output0',
   'conv_input': '/model/model.0/act/Mul_output_0', 'conv_weight': 'model.model.1.conv.weight',
   'conv_bias': 'model.model.1.conv.bias', 'conv_output': '/model/model.1/conv/Conv_output_0',
   'max_nodes': 1024},
}
variant = args.variant
cache = (args.cache or YOLO[variant]['dir']).resolve()
src = (cache / 'model.onnx').read_bytes() if (cache / 'model.onnx').exists() else (cache / f'yolo{variant}.onnx').read_bytes()
weights_path = cache / 'model.weights' if (cache / 'model.weights').exists() else cache / f'yolo{variant}.weights'
weights = weights_path.read_bytes()
input_raw = (cache / 'input.f32').read_bytes()
oracle_values = json.loads((cache / 'oracle.json').read_text())
src_digest='sha256:'+hashlib.sha256(src).hexdigest()
ad=adapter_descriptor('spec261','1',dg('fixture-adapter-state'),'fixture-abi-v1',('onnx',),('task',),('onnxruntime',),('float32',),dg('fixture-input-schema'),dg('fixture-options-schema'),dg('fixture-result-schema'),dg('fixture-graph-schema'),dg('fixture-split-schema'),dg('fixture-state-schema'),True,True,True)
model=model_descriptor(YOLO[variant]['name'],dg(YOLO[variant]['name']+'-content'),dg(YOLO[variant]['name']+'-semantics'),YOLO[variant]['planning'],'onnx','float32',ad)
adapter_digest=digest(ad)
package=dg('yolo-source-package'); profile=dg('yolo-artifact-profile'); asm=dg('yolo-assembler'); offer_policy=dg('yolo-offer-policy')
catalog={"schema":"ndnsf-di-native-request-catalog-v1","model":json.loads(canonical_bytes(model).decode()),"source":{"data_name":"/catalog/source","digest":src_digest,"model_manifest_digest":package,"canonical_graph_digest":YOLO[variant]['canonical'],"initializer_file":"model.weights","initializer_digest":YOLO[variant]['initializer']},"recipe":{"artifact_profile_digest":profile,"assembler_descriptor_digest":asm,"backend_abi":"onnxruntime-cpu-v1","precision":"float32","quantization":"none","layout":"native","padding":"none","protection_epoch":"epoch-1","max_source_bytes":67108864,"max_assembled_bytes":134217728,"max_nodes":YOLO[variant]['max_nodes']},"publication":{"artifact_root":"/Model/"+YOLO[variant]['name']+"/artifacts","package_manifest_digest":package},"input_format":"OPAQUE","max_payload_bytes":8388608,"splitter":{"kind":"ONNX_SOURCE","selection_mode":"hybrid","roles":["Prefix","Rank0","Rank1","Suffix"],"boundary_node":args.boundary,"input_ingress_role":"Prefix","result_egress_role":"Suffix","postprocessing":{},"input_channels":16,"conv_input_axis":"input-channel","activation_shape":[1,16,320,320],"partial_shape":[1,32,160,160],"activation_dtype":"float32","conv_input_tensor":YOLO[variant]["conv_input"],"conv_weight_initializer":YOLO[variant]["conv_weight"],"conv_bias_initializer":YOLO[variant]["conv_bias"],"conv_output_tensor":YOLO[variant]["conv_output"],"conv_weight_bytes":18432,"conv_bias_bytes":128,"conv_pads":[1,1,1,1],"conv_strides":[2,2],"conv_dilations":[1,1]},"node_mapping":{},"state_inputs":{},"state_outputs":{}}
plan={"version":2,"services":[{"service":"/Inference/NativeUnary","schemaVersion":2,"model":"/Model/"+YOLO[variant]['name'],"modelFamily":"yolo","modelFormat":"onnx","plannerKind":"native-source-full","runtimeBackend":"onnxruntime","executionPolicy":"DATA_DRIVEN_V2","roles":["Prefix","Rank0","Rank1","Suffix"],"dependencies":[],"planner":{"schemaVersion":2,"modelFamily":"yolo","modelFormat":"onnx","plannerKind":"native-source-full","runtimeBackend":"onnxruntime","executionPolicy":"DATA_DRIVEN_V2"}}]}
manifest={"services":[{"name":"/Inference/NativeUnary","model":"/Model/"+YOLO[variant]['name'],"roles":["Prefix","Rank0","Rank1","Suffix"],"artifacts":[{"artifact":"/Artifact/YOLO/Prefix","backend":"onnxruntime-cpu","kind":"model","role":"Prefix"},{"artifact":"/Artifact/YOLO/Rank0","backend":"onnxruntime-cpu","kind":"model","role":"Rank0"},{"artifact":"/Artifact/YOLO/Rank1","backend":"onnxruntime-cpu","kind":"model","role":"Rank1"},{"artifact":"/Artifact/YOLO/Suffix","backend":"onnxruntime-cpu","kind":"model","role":"Suffix"}]}]}
# provider plan and manifest files generated later

def policy_text():
 return """name /example/hello/controller/NDNSF/ControllerPolicy/v1

provider-policies
{
 provider-policy
 {
  for /example/hello/authority
  allow { /HELLO }
 }
 provider-policy
 {
  for /example/hello/provider
  allow { /Inference/NativeUnary
  /Inference/NativeUnary/ROLE/Prefix
  /Inference/NativeUnary/ROLE/Rank0 }
 }
 provider-policy
 {
  for /example/hello/provider2
  allow { /Inference/NativeUnary
  /Inference/NativeUnary/ROLE/Rank1
  /Inference/NativeUnary/ROLE/Suffix }
 }
}

user-policies
{
 user-policy
 {
  for /example/hello/user
  allow { /Inference/NativeUnary
  /HELLO }
 }
}
"""
if args.run_root is None:
 run_root=Path(tempfile.mkdtemp(prefix='spec182-r11-b2-probe-',dir='/tmp'))
else:
 run_root=args.run_root.resolve()
 run_root.mkdir(parents=True, exist_ok=True)
run_root.chmod(0o700)
for n in ['shared','bootstrap-store','authority','provider','provider2','requester']:(run_root/n).mkdir(mode=0o700)
(run_root/'shared/home').mkdir(); (run_root/'bootstrap-store/home').mkdir()
nfd_socket=b.bounded_nfd_socket(run_root); nfdconf=run_root/'nfd.conf'; b.nfd_config(nfdconf,nfd_socket); policy=run_root/'hello.policies'; b.write(policy,policy_text(),0o600)
# bootstrap identities
bootenv=os.environ.copy(); bootenv.update({'HOME':str(run_root/'bootstrap-store/home'),'NDN_CLIENT_PIB':'pib-sqlite3:'+str(run_root/'bootstrap-store/pib'),'NDN_CLIENT_TPM':'tpm-file:'+str(run_root/'bootstrap-store/tpm'),'PATH':'/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin'})
actor_identities=['/example/hello/controller','/example/hello/authority','/example/hello/provider','/example/hello/provider2','/example/hello/user']
root_certificate=b.provision_actor_identities(run_root,bootenv,actor_identities)
fixture_schema=b.actor_trust_schema(actor_identities)
b.write(run_root/'shared/trust-schema.conf',fixture_schema,0o644)
for directory in ['authority','provider','provider2','requester']:
 b.write(run_root/directory/'root.cert',root_certificate,0o644)
for n in ['controller-store','authority-store','provider-store','provider2-store','requester-store']:
 shutil.copytree(run_root/'bootstrap-store',run_root/n)
 with sqlite3.connect(run_root/n/'pib/pib.db') as db: db.execute('UPDATE tpmInfo SET tpm_locator=?',(f'tpm-file:{run_root/n}/tpm',)); db.commit()
b.remove_private_keys_except(run_root/'controller-store',['/example/hello/controller'])
b.remove_private_keys_except(run_root/'authority-store',['/example/hello/authority'])
b.remove_private_keys_except(run_root/'provider-store',['/example/hello/provider'])
b.remove_private_keys_except(run_root/'provider2-store',['/example/hello/provider2'])
b.remove_private_keys_except(run_root/'requester-store',['/example/hello/user'])
# external keys
def edkey(d,prefix):
 prefix.mkdir(parents=True, exist_ok=True)
 priv=prefix/'private.pem'; pub=prefix/'public.pem'; subprocess.run(['/usr/bin/openssl','genpkey','-algorithm','ED25519','-out',str(priv)],check=True,stdout=subprocess.DEVNULL); subprocess.run(['/usr/bin/openssl','pkey','-in',str(priv),'-pubout','-out',str(pub)],check=True,stdout=subprocess.DEVNULL); priv.chmod(0o600); pub.chmod(0o644); return priv,pub
reqpriv,reqpub=edkey(0,run_root/'requester'); authpriv,authpub=edkey(0,run_root/'authority'); recippriv,recippub=edkey(0,run_root/'provider'); offerpriv,offerpub=edkey(0,run_root/'provider/offer')
recip2priv,recip2pub=edkey(0,run_root/'provider2'); offer2priv,offer2pub=edkey(0,run_root/'provider2/offer')
(run_root/'authority/content-key.bin').write_bytes(b'spec182-b2-content-key-000000000'); (run_root/'authority/content-key.bin').chmod(0o600)
(run_root/'authority/trust-schema.conf').write_text(fixture_schema); (run_root/'provider/trust-schema.conf').write_text(fixture_schema); (run_root/'provider2/trust-schema.conf').write_text(fixture_schema)
# registry for provider
registry={"schemaVersion":1,"status":"CONFIGURED","artifactPolicyAuthority":{"publicKeyAlgorithm":"ed25519","signatureAlgorithm":"ed25519","grantSchema":"ndnsf-di-key-grant-v1","authorityId":"/example/hello/authority","keyId":"model-key","publicKeyPath":"provider/authority-public.pem","publicKeySha256":"sha256:"+hashlib.sha256(authpub.read_bytes()).hexdigest(),"acceptedModelFamilies":[YOLO[variant]["name"]],"protectionEpochs":["epoch-1"]}}
(run_root/'provider/trust-root-registry-v1.json').write_text(json.dumps(registry));
registry2=json.loads(json.dumps(registry)); registry2['artifactPolicyAuthority']['publicKeyPath']='provider2/authority-public.pem'
(run_root/'provider2/trust-root-registry-v1.json').write_text(json.dumps(registry2)); shutil.copy2(authpub,run_root/'provider/authority-public.pem'); shutil.copy2(authpub,run_root/'provider2/authority-public.pem'); (run_root/'provider/recipient-private.pem').write_bytes(recippriv.read_bytes()); (run_root/'provider/recipient-private.pem').chmod(0o600); (run_root/'provider2/recipient-private.pem').write_bytes(recip2priv.read_bytes()); (run_root/'provider2/recipient-private.pem').chmod(0o600); (run_root/'provider/recipient-key-map.json').write_text(json.dumps({'/example/hello/provider':str(run_root/'provider/recipient-private.pem')})); (run_root/'provider2/recipient-key-map.json').write_text(json.dumps({'/example/hello/provider2':str(run_root/'provider2/recipient-private.pem')}))
# authority config
ac={"schema":"ndnsf-di-native-authority-v1","run_for_ms":120000,"permission_bootstrap_ms":30000,"max_grant_ttl_ms":60000,"authority":{"identity":"/example/hello/authority","service":"/HELLO","group":"/example/hello/group","controller_identity":"/example/hello/controller","requester_identity":"/example/hello/user","protection_epoch":"epoch-1","content_key_id":"model-key","trust_schema_file":"trust-schema.conf","authority_private_key_file":"private.pem","requester_public_key_file":"../requester/public.pem","content_key_file":"content-key.bin","allowed_model_manifests":[package],"recipient_public_key_files":{"/example/hello/provider":"../provider/public.pem","/example/hello/provider2":"../provider2/public.pem"},"publication_sources":{package:{"model_name":model["model_name"],"model_content_digest":model["content_digest"],"canonical_source_digest":src_digest,"initializer_object_digest":YOLO[variant]["initializer"],"artifact_profile_digest":profile}}}}
(run_root/'authority/authority.json').write_text(json.dumps(ac))
# requester files
(run_root/'requester/requester-private.pem').write_bytes(reqpriv.read_bytes());(run_root/'requester/requester-private.pem').chmod(0o600); shutil.copy2(authpub,run_root/'requester/authority-public.pem'); shutil.copy2(offerpub,run_root/'requester/offer-public.pem'); shutil.copy2(offer2pub,run_root/'requester/offer2-public.pem'); (run_root/'requester/trust-schema.conf').write_text(fixture_schema); (run_root/'requester/model.onnx').write_bytes(src); (run_root/'requester/model.weights').write_bytes(weights); (run_root/'requester/catalog.json').write_text(json.dumps(catalog));
# Cert name query from the provider PIB lets the requester validate the
# signed offer without sharing the provider's private NDN key.
cert=b.certificate_name(bootenv,'/example/hello/provider')
keyprefix=b.certificate_key_name(cert)
cert2=b.certificate_name(bootenv,'/example/hello/provider2')
keyprefix2=b.certificate_key_name(cert2)

# offer policy
sig1="sha256:"+hashlib.sha256(load_pem_public_key(offerpub.read_bytes(), backend=default_backend()).public_bytes(Encoding.Raw,PublicFormat.Raw)).hexdigest()
sig2="sha256:"+hashlib.sha256(load_pem_public_key(offer2pub.read_bytes(), backend=default_backend()).public_bytes(Encoding.Raw,PublicFormat.Raw)).hexdigest()
op={"schema":"spec180-provider-offer-trust-v1","candidateId":"b2","candidateDigest":offer_policy,"trustSchema":"/example/hello/trust","entries":[{"provider":"/example/hello/provider","service":"/Inference/NativeUnary","keyLocatorPrefix":keyprefix,"signerKeyId":sig1,"certificateName":cert},{"provider":"/example/hello/provider2","service":"/Inference/NativeUnary","keyLocatorPrefix":keyprefix2,"signerKeyId":sig2,"certificateName":cert2}]}
# requester config
rc={"schema":"ndnsf-di-native-requester-v1","catalog":dict(catalog,source=dict(catalog['source'],file='model.onnx')),"core":{"requester_identity":"/example/hello/user","authority_identity":"/example/hello/controller","group":"/example/hello/group","trust_schema_file":"trust-schema.conf"},"grant":{"authority_identity":"/example/hello/authority","authority_service":"/HELLO","authority_public_key_file":"authority-public.pem","requester_private_key_file":"requester-private.pem","protection_epoch":"epoch-1"},"offer_admission":{"policy":op,"public_key_files":{sig1:'offer-public.pem',sig2:'offer2-public.pem'},"candidate_digest":offer_policy},"limits":{"bootstrap_ms":60000,"max_source_bytes":67108864,"max_assembled_bytes":134217728},"request":{"service":"/Inference/NativeUnary","task":"task","adapter_composition_digest":adapter_digest,"task_descriptor_digest":dg('task'),"generation_mode":"TOKEN_DIAGNOSTIC","input_layout_digest":dg('input-layout'),"security_policy_digest":dg('security'),"max_candidates":2,"max_policy_ms":1000,"provider_names":["/example/hello/provider","/example/hello/provider2"],"max_reentries":1,"no_progress_ms":5000,"timeout_ms":30000,"ack_timeout_ms":5000}}

(run_root/'provider/plan.json').write_text(json.dumps(plan));(run_root/'provider/manifest.json').write_text(json.dumps(manifest));(run_root/'provider2/plan.json').write_text(json.dumps(plan));(run_root/'provider2/manifest.json').write_text(json.dumps(manifest));
# input bundle: deterministic Conv input and its independent ORT oracle
oracle=json.loads((ROOT/'tests/fixtures/spec261/conv-single-oracle.json').read_text())
dims=[1,3,640,640]
hdr=b'NDITB001'+struct.pack('<I',1)+struct.pack('<I',6)+b'images'+struct.pack('<I',1)+struct.pack('<I',len(dims))
for d in dims: hdr+=struct.pack('<q',d)
hdr+=struct.pack('<Q',len(input_raw))
(run_root/'requester/input.bin').write_bytes(hdr+input_raw)
rc['oracle']={'tensor':YOLO[variant]['output'],'float32':oracle_values,'tolerance':1e-4,
 'row_width':6,'detections':True,'coordinate_tolerance':0.01,'score_tolerance':1e-4,'iou_floor':0.999}
(run_root/'requester/config.json').write_text(json.dumps(rc))
# launch env
base=os.environ.copy(); base.update({'NDN_CLIENT_TRANSPORT':'unix://'+str(nfd_socket),'PATH':'/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin','NDNSF_CONTROLLER_GENERATION_STATE':str(run_root/'shared/controller-generation.state'),'NDNSF_DI_RUNTIME_TIMING':'1'})
base.setdefault('NDN_LOG', 'ndn_service_framework.*=TRACE:ndnsf.di.NativeRequester=WARN')
# The process oracle reads readiness and execution evidence from this logger.
base['NDN_LOG'] += ':ndnsf.di.RuntimeEvidence=WARN'
def launch(cmd,env,name):
 p=b.run(cmd,env,run_root/(name+'.log'),wait=False)
 (run_root/(name+'.pid')).write_text(str(p.pid)+'\n')
 return p
children=[]
try:
 nfd=launch(['/usr/local/bin/nfd','--config',str(nfdconf)],base,'nfd');children.append(nfd)
 nfd_deadline=time.monotonic()+30
 while not nfd_socket.exists():
  if time.monotonic()>nfd_deadline: raise RuntimeError('NFD socket did not appear within 30s')
  time.sleep(.05)
 subprocess.run(['/usr/local/bin/nfdc','strategy','set','/example/hello/group','/localhost/nfd/strategy/multicast'],env=base,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 def penv(store):
  return b.actor_environment(base,store)
 controller_command=[str(CONTROLLER),'--controller-prefix','/example/hello/controller','--policy-file',str(policy),'--trust-schema',str(run_root/'shared/trust-schema.conf'),'--ensure-identities','/example/hello/authority,/example/hello/provider,/example/hello/provider2,/example/hello/user','--no-serve-certificates']
 revoke_after_ms=60000
 if args.revoke:
  controller_command += ['--revoke-after-ms',str(revoke_after_ms),'--revoke-kind','identity','--revoke-identity','/example/hello/provider']
 controller_command += ['--run-for-ms','120000']
 controller_started_at=time.monotonic()
 c=launch(controller_command,penv(run_root/'controller-store'),'controller');children.append(c); b.wait_marker(c,run_root/'controller.log','ServiceController started...',30)
 a=launch([str(AUTHORITY),'--config',str(run_root/'authority/authority.json')],penv(run_root/'authority-store'),'authority');children.append(a); b.wait_marker(a,run_root/'authority.log','NATIVE_GRANT_AUTHORITY_READY',30)
 providers=[('/example/hello/provider','Prefix,Rank0','provider',offerpriv),('/example/hello/provider2','Rank1,Suffix','provider2',offer2priv)]
 def start_provider(identity,roles,tag,offerkey):
  pe=penv(run_root/(tag+'-store'))
  shutil.copy2(offerkey,run_root/(tag+'/offer-private.pem'))
  pm=[str(PROVIDER),'--plan',str(run_root/'provider/plan.json'),'--manifest',str(run_root/(tag+'/manifest.json')),'--service','/Inference/NativeUnary','--provider',identity,'--group','/example/hello/group','--controller','/example/hello/controller','--trust-schema',str(run_root/(tag+'/trust-schema.conf')),'--roles',roles,'--workers','4','--handler-threads','4','--serve','--run-for-ms','120000','--artifact-cache-dir',str(run_root/(tag+'/cache')),'--selection-offer-key-file',str(run_root/(tag+'/offer-private.pem')),'--offer-backend','onnxruntime-cpu','--offer-can-provision','--offer-has-model']
  pe['SPEC181_GRANT_AUTHORITY_PUBLIC_KEY']=str(run_root/(tag+'/authority-public.pem'));pe['SPEC181_PROVIDER_RECIPIENT_KEY_MAP']=str(run_root/(tag+'/recipient-key-map.json'));pe['NDNSF_DI_WORKER_BINARY']=str(WORKER)
  ps=launch(pm,pe,tag);children.append(ps); b.wait_marker(ps,run_root/(tag+'.log'),'NDNSF_DI_NATIVE_PROVIDER_READY',60)
  return ps
 running={}
 for identity,roles,tag,offerkey in providers:
  running[tag]=start_provider(identity,roles,tag,offerkey)
 re=penv(run_root/'requester-store'); re.update({'NDNSF_DI_WORKER_BINARY':str(WORKER)})
 if args.revoke:
  # Establish the independent pre-revocation success first.  The later
  # request is started only after Controller publication; its native error and
  # the retained Provider log boundary let the C++ driver prove receiver-side enforcement.
  baseline_requester=launch([str(REQUESTER),'--config',str(run_root/'requester/config.json'),'--input',str(run_root/'requester/input.bin'),'--output',str(run_root/'requester/baseline-output.bin')],re,'requester-baseline');children.append(baseline_requester)
  baseline_wait_budget=controller_started_at + revoke_after_ms / 1000.0 - time.monotonic() - 1.0
  if baseline_wait_budget <= 0:
   raise RuntimeError('scheduled revocation window elapsed before baseline request')
  try:
   baseline_requester.wait(timeout=baseline_wait_budget)
  except subprocess.TimeoutExpired:
   b.stop(baseline_requester)
   raise RuntimeError('baseline request did not complete before scheduled revocation')
  print('requester-baseline rc',baseline_requester.returncode)
  time.sleep(0.2)
  provider_snapshot=(run_root/'provider.log').read_bytes()
  boundary=provider_snapshot.rfind(b'\n') + 1
  (run_root/'provider-baseline-boundary').write_text(str(boundary)+'\n')
  print('NATIVE_REQUEST_BASELINE_PROCESS_EXITED')
  # The Controller publishes the scheduled revocation after the baseline
  # request window.  Wait against that explicit schedule plus a bounded
  # startup/transport margin instead of a shorter fixed timeout.
  revoke_wait_seconds=max(30, int(revoke_after_ms / 1000) + 30)
  b.wait_marker(c,run_root/'controller.log','NDNSF_REVOCATION_APPLIED success=1',revoke_wait_seconds)
 # requester direct first
 rq=launch([str(REQUESTER),'--config',str(run_root/'requester/config.json'),'--input',str(run_root/'requester/input.bin'),'--output',str(run_root/'requester/output.bin')],re,'requester');children.append(rq)
 try:
  rq.wait(timeout=120)
 except subprocess.TimeoutExpired:
  b.stop(rq)
  raise RuntimeError('requester did not finish within 120s')
 print('requester rc',rq.returncode)
 if rq.returncode != 0:
  raise SystemExit(rq.returncode)
 if args.restart_provider2:
  b.stop(running['provider2'])
  time.sleep(1.0)
  running['provider2']=start_provider('/example/hello/provider2','Rank1,Suffix','provider2',offer2priv)
  rq2=launch([str(REQUESTER),'--config',str(run_root/'requester/config.json'),'--input',str(run_root/'requester/input.bin'),'--output',str(run_root/'requester/output-restarted.bin')],re,'requester-restarted');children.append(rq2)
  try:
   rq2.wait(timeout=120)
  except subprocess.TimeoutExpired:
   b.stop(rq2)
   raise RuntimeError('restarted-provider requester did not finish within 120s')
  print('requester after restart rc',rq2.returncode)
  if rq2.returncode != 0:
   raise SystemExit(rq2.returncode)
 print('ROOT',run_root)
 # This script owns process startup, scheduled revocation and cleanup only.
 # Business success/failure, grant identity and Provider execution are
 # asserted by the C++ parent selector from these retained logs.  Keeping the
 # Python layer free of those assertions prevents a Python marker check from
 # being mistaken for native qualification evidence.
 if args.revoke:
  time.sleep(0.2)
  print('NATIVE_REQUEST_REVOKE_ORCHESTRATION_COMPLETE')
finally:
 for p in reversed(children): b.stop(p)
