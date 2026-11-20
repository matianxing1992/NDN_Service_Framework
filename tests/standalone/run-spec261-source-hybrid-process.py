#!/usr/bin/env python3
"""Run the Spec 261 generic ONNX_SOURCE hybrid native process case.

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
src=(ROOT/'tests/fixtures/spec261/conv-single.onnx').read_bytes(); src_digest='sha256:'+hashlib.sha256(src).hexdigest()
PLANNING_DIGEST='sha256:6c3e39cb7fd9f6ca6e4358e6a66809462f4f0c07f731b25810fbf1665ffe8b06'
CANONICAL_DIGEST='sha256:1a8a259dedba38f4c99641532b99c9e42c2fa5e6f1391104ff242a930b296a22'
ad=adapter_descriptor('spec261','1',dg('fixture-adapter-state'),'fixture-abi-v1',('onnx',),('task',),('onnxruntime',),('float32',),dg('fixture-input-schema'),dg('fixture-options-schema'),dg('fixture-result-schema'),dg('fixture-graph-schema'),dg('fixture-split-schema'),dg('fixture-state-schema'),True,True,True)
model=model_descriptor('ConvSingle',dg('conv-single-content'),dg('conv-single-semantics'),PLANNING_DIGEST,'onnx','float32',ad)
adapter_digest=digest(ad)
package=dg('yolo-package-manifest'); profile=dg('yolo-artifact-profile'); asm=dg('yolo-assembler'); offer_policy=dg('yolo-offer-policy')
catalog={"schema":"ndnsf-di-native-request-catalog-v1","model":json.loads(canonical_bytes(model).decode()),"source":{"data_name":"/catalog/source","digest":src_digest,"model_manifest_digest":package,"canonical_graph_digest":CANONICAL_DIGEST},"recipe":{"artifact_profile_digest":profile,"assembler_descriptor_digest":asm,"backend_abi":"onnxruntime-cpu-v1","precision":"float32","quantization":"none","layout":"native","padding":"none","protection_epoch":"epoch-1","max_source_bytes":1000000,"max_assembled_bytes":1000000,"max_nodes":64},"publication":{"artifact_root":"/Model/YOLOFixture/artifacts","package_manifest_digest":package},"input_format":"OPAQUE","max_payload_bytes":16384,"splitter":{"kind":"ONNX_SOURCE","selection_mode":"hybrid","roles":["Prefix","Rank0","Rank1","Suffix"],"boundary_node":"onnx-node-1","input_ingress_role":"Prefix","result_egress_role":"Suffix","postprocessing":{},"input_channels":16,"conv_input_axis":"input-channel","activation_shape":[1,16,8,8],"partial_shape":[1,4,8,8],"activation_dtype":"float32","conv_input_tensor":"scaled","conv_weight_initializer":"w_conv","conv_bias_initializer":"b_conv","conv_output_tensor":"conv_out","conv_weight_bytes":2304,"conv_bias_bytes":16,"conv_pads":[1,1,1,1],"conv_strides":[1,1],"conv_dilations":[1,1]},"node_mapping":{},"state_inputs":{},"state_outputs":{}}
plan={"version":2,"services":[{"service":"/Inference/NativeUnary","schemaVersion":2,"model":"/Model/ConvSingle","modelFamily":"spec261","modelFormat":"onnx","plannerKind":"native-source-hybrid","runtimeBackend":"onnxruntime","executionPolicy":"DATA_DRIVEN_V2","roles":["Prefix","Rank0","Rank1","Suffix"],"dependencies":[],"planner":{"schemaVersion":2,"modelFamily":"spec261","modelFormat":"onnx","plannerKind":"native-source-hybrid","runtimeBackend":"onnxruntime","executionPolicy":"DATA_DRIVEN_V2"}}]}
manifest={"services":[{"name":"/Inference/NativeUnary","model":"/Model/ConvSingle","roles":["Prefix","Rank0","Rank1","Suffix"],"artifacts":[{"artifact":"/Artifact/Source/Prefix","backend":"onnxruntime-cpu","kind":"model","role":"Prefix"},{"artifact":"/Artifact/Source/Rank0","backend":"onnxruntime-cpu","kind":"model","role":"Rank0"},{"artifact":"/Artifact/Source/Rank1","backend":"onnxruntime-cpu","kind":"model","role":"Rank1"},{"artifact":"/Artifact/Source/Suffix","backend":"onnxruntime-cpu","kind":"model","role":"Suffix"}],"inputs":{"Prefix":{"codec":"tensor-bundle","fields":{"images":{"dtype":"float32","shape":[1,16,8,8]}}},"Rank0":{"codec":"tensor-bundle","fields":{"scaled":{"dtype":"float32","shape":[1,16,8,8]}}},"Rank1":{"codec":"tensor-bundle","fields":{"scaled":{"dtype":"float32","shape":[1,16,8,8]}}},"Suffix":{"codec":"tensor-bundle","fields":{"conv_out":{"dtype":"float32","shape":[1,4,8,8]}}}},"outputs":{"Prefix":{"codec":"tensor-bundle","fields":{"scaled":{"dtype":"float32","shape":[1,16,8,8]}}},"Rank0":{"codec":"tensor-bundle","fields":{"conv_out":{"dtype":"float32","shape":[1,4,8,8]}}},"Rank1":{"codec":"tensor-bundle","fields":{"conv_out":{"dtype":"float32","shape":[1,4,8,8]}}},"Suffix":{"codec":"tensor-bundle","fields":{"predictions":{"dtype":"float32","shape":[1,4,8,8]}}}}}]}
# provider plan and manifest files generated later

def policy_text():
 return '''name /example/hello/controller/NDNSF/ControllerPolicy/v1\n\nprovider-policies\n{\n provider-policy\n {\n  for /example/hello/authority\n  allow { /HELLO }\n }\n provider-policy\n {\n  for /example/hello/provider\n  allow { /Inference/NativeUnary\n  /Inference/NativeUnary/ROLE/Prefix
  /Inference/NativeUnary/ROLE/Rank0
  /Inference/NativeUnary/ROLE/Rank1
  /Inference/NativeUnary/ROLE/Suffix }\n }\n}\n\nuser-policies\n{\n user-policy\n {\n  for /example/hello/user\n  allow { /Inference/NativeUnary\n  /HELLO }\n }\n}\n'''
if args.run_root is None:
 run_root=Path(tempfile.mkdtemp(prefix='spec182-r11-b2-probe-',dir='/tmp'))
else:
 run_root=args.run_root.resolve()
 run_root.mkdir(parents=True, exist_ok=True)
run_root.chmod(0o700)
for n in ['shared','bootstrap-store','authority','provider','requester']:(run_root/n).mkdir(mode=0o700)
(run_root/'shared/home').mkdir(); (run_root/'bootstrap-store/home').mkdir()
nfd_socket=b.bounded_nfd_socket(run_root); nfdconf=run_root/'nfd.conf'; b.nfd_config(nfdconf,nfd_socket); policy=run_root/'hello.policies'; b.write(policy,policy_text(),0o600)
# bootstrap identities
bootenv=os.environ.copy(); bootenv.update({'HOME':str(run_root/'bootstrap-store/home'),'NDN_CLIENT_PIB':'pib-sqlite3:'+str(run_root/'bootstrap-store/pib'),'NDN_CLIENT_TPM':'tpm-file:'+str(run_root/'bootstrap-store/tpm'),'PATH':'/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin'})
actor_identities=['/example/hello/controller','/example/hello/authority','/example/hello/provider','/example/hello/user']
root_certificate=b.provision_actor_identities(run_root,bootenv,actor_identities)
fixture_schema=b.actor_trust_schema(actor_identities)
b.write(run_root/'shared/trust-schema.conf',fixture_schema,0o644)
for directory in ['authority','provider','requester']:
 b.write(run_root/directory/'root.cert',root_certificate,0o644)
for n in ['controller-store','authority-store','provider-store','requester-store']:
 shutil.copytree(run_root/'bootstrap-store',run_root/n)
 with sqlite3.connect(run_root/n/'pib/pib.db') as db: db.execute('UPDATE tpmInfo SET tpm_locator=?',(f'tpm-file:{run_root/n}/tpm',)); db.commit()
b.remove_private_keys_except(run_root/'controller-store',['/example/hello/controller'])
b.remove_private_keys_except(run_root/'authority-store',['/example/hello/authority'])
b.remove_private_keys_except(run_root/'provider-store',['/example/hello/provider'])
b.remove_private_keys_except(run_root/'requester-store',['/example/hello/user'])
# external keys
def edkey(d,prefix):
 prefix.mkdir(parents=True, exist_ok=True)
 priv=prefix/'private.pem'; pub=prefix/'public.pem'; subprocess.run(['/usr/bin/openssl','genpkey','-algorithm','ED25519','-out',str(priv)],check=True,stdout=subprocess.DEVNULL); subprocess.run(['/usr/bin/openssl','pkey','-in',str(priv),'-pubout','-out',str(pub)],check=True,stdout=subprocess.DEVNULL); priv.chmod(0o600); pub.chmod(0o644); return priv,pub
reqpriv,reqpub=edkey(0,run_root/'requester'); authpriv,authpub=edkey(0,run_root/'authority'); recippriv,recippub=edkey(0,run_root/'provider'); offerpriv,offerpub=edkey(0,run_root/'provider/offer')
(run_root/'authority/content-key.bin').write_bytes(b'spec182-b2-content-key-000000000'); (run_root/'authority/content-key.bin').chmod(0o600)
(run_root/'authority/trust-schema.conf').write_text(fixture_schema); (run_root/'provider/trust-schema.conf').write_text(fixture_schema)
# registry for provider
registry={"schemaVersion":1,"status":"CONFIGURED","artifactPolicyAuthority":{"publicKeyAlgorithm":"ed25519","signatureAlgorithm":"ed25519","grantSchema":"ndnsf-di-key-grant-v1","authorityId":"/example/hello/authority","keyId":"model-key","publicKeyPath":"provider/authority-public.pem","publicKeySha256":"sha256:"+hashlib.sha256(authpub.read_bytes()).hexdigest(),"acceptedModelFamilies":["ConvSingle"],"protectionEpochs":["epoch-1"]}}
(run_root/'provider/trust-root-registry-v1.json').write_text(json.dumps(registry)); shutil.copy2(authpub,run_root/'provider/authority-public.pem'); (run_root/'provider/recipient-private.pem').write_bytes(recippriv.read_bytes()); (run_root/'provider/recipient-private.pem').chmod(0o600); (run_root/'provider/recipient-key-map.json').write_text(json.dumps({'/example/hello/provider':str(run_root/'provider/recipient-private.pem')}))
# authority config
ac={"schema":"ndnsf-di-native-authority-v1","run_for_ms":120000,"permission_bootstrap_ms":30000,"max_grant_ttl_ms":60000,"authority":{"identity":"/example/hello/authority","service":"/HELLO","group":"/example/hello/group","controller_identity":"/example/hello/controller","requester_identity":"/example/hello/user","protection_epoch":"epoch-1","content_key_id":"model-key","trust_schema_file":"trust-schema.conf","authority_private_key_file":"private.pem","requester_public_key_file":"../requester/public.pem","content_key_file":"content-key.bin","allowed_model_manifests":[package],"recipient_public_key_files":{"/example/hello/provider":"../provider/public.pem"},"publication_sources":{package:{"model_name":model["model_name"],"model_content_digest":model["content_digest"],"canonical_source_digest":src_digest,"artifact_profile_digest":profile}}}}
(run_root/'authority/authority.json').write_text(json.dumps(ac))
# requester files
(run_root/'requester/requester-private.pem').write_bytes(reqpriv.read_bytes());(run_root/'requester/requester-private.pem').chmod(0o600); shutil.copy2(authpub,run_root/'requester/authority-public.pem'); shutil.copy2(offerpub,run_root/'requester/offer-public.pem'); (run_root/'requester/trust-schema.conf').write_text(fixture_schema); (run_root/'requester/model.onnx').write_bytes(src); (run_root/'requester/catalog.json').write_text(json.dumps(catalog));
# Cert name query from the provider PIB lets the requester validate the
# signed offer without sharing the provider's private NDN key.
cert=b.certificate_name(bootenv,'/example/hello/provider')
keyprefix=b.certificate_key_name(cert)

# offer policy
op={"schema":"spec180-provider-offer-trust-v1","candidateId":"b2","candidateDigest":offer_policy,"trustSchema":"/example/hello/trust","entries":[{"provider":"/example/hello/provider","service":"/Inference/NativeUnary","keyLocatorPrefix":keyprefix,"signerKeyId":"sha256:"+hashlib.sha256(load_pem_public_key(offerpub.read_bytes(), backend=default_backend()).public_bytes(Encoding.Raw,PublicFormat.Raw)).hexdigest(),"certificateName":cert}]}
# requester config
rc={"schema":"ndnsf-di-native-requester-v1","catalog":dict(catalog,source=dict(catalog['source'],file='model.onnx')),"core":{"requester_identity":"/example/hello/user","authority_identity":"/example/hello/controller","group":"/example/hello/group","trust_schema_file":"trust-schema.conf"},"grant":{"authority_identity":"/example/hello/authority","authority_service":"/HELLO","authority_public_key_file":"authority-public.pem","requester_private_key_file":"requester-private.pem","protection_epoch":"epoch-1"},"offer_admission":{"policy":op,"public_key_files":{op['entries'][0]['signerKeyId']:'offer-public.pem'},"candidate_digest":offer_policy},"limits":{"bootstrap_ms":30000,"max_source_bytes":1000000,"max_assembled_bytes":1000000},"request":{"service":"/Inference/NativeUnary","task":"task","adapter_composition_digest":adapter_digest,"task_descriptor_digest":dg('task'),"generation_mode":"TOKEN_DIAGNOSTIC","input_layout_digest":dg('input-layout'),"security_policy_digest":dg('security'),"max_candidates":1,"max_policy_ms":1000,"provider_names":["/example/hello/provider"],"max_reentries":1,"no_progress_ms":5000,"timeout_ms":30000,"ack_timeout_ms":5000}}
rc['oracle']={'tensor':'predictions','float32':[4.0,0.0,12.0],'tolerance':1e-5};(run_root/'requester/config.json').write_text(json.dumps(rc))
(run_root/'provider/plan.json').write_text(json.dumps(plan));(run_root/'provider/manifest.json').write_text(json.dumps(manifest));
# input bundle: deterministic Conv input and its independent ORT oracle
oracle=json.loads((ROOT/'tests/fixtures/spec261/conv-single-oracle.json').read_text())
vals=[((i%13)-6)/8.0 for i in range(1024)]; payload=b''.join(struct.pack('<f',v) for v in vals)
dims=[1,16,8,8]
# TensorBundle member: count, name, elementType(1=float32), rank, dims, payload size.
hdr=b'NDITB001'+struct.pack('<I',1)+struct.pack('<I',6)+b'images'+struct.pack('<I',1)+struct.pack('<I',len(dims))
for d in dims: hdr+=struct.pack('<q',d)
hdr+=struct.pack('<Q',len(payload))
(run_root/'requester/input.bin').write_bytes(hdr+payload)
rc['oracle']={'tensor':'predictions','float32':oracle['float32'],'tolerance':1e-4}
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
 controller_command=[str(CONTROLLER),'--controller-prefix','/example/hello/controller','--policy-file',str(policy),'--trust-schema',str(run_root/'shared/trust-schema.conf'),'--ensure-identities','/example/hello/authority,/example/hello/provider,/example/hello/user','--no-serve-certificates']
 revoke_after_ms=60000
 if args.revoke:
  controller_command += ['--revoke-after-ms',str(revoke_after_ms),'--revoke-kind','identity','--revoke-identity','/example/hello/provider']
 controller_command += ['--run-for-ms','120000']
 controller_started_at=time.monotonic()
 c=launch(controller_command,penv(run_root/'controller-store'),'controller');children.append(c); b.wait_marker(c,run_root/'controller.log','ServiceController started...',30)
 a=launch([str(AUTHORITY),'--config',str(run_root/'authority/authority.json')],penv(run_root/'authority-store'),'authority');children.append(a); b.wait_marker(a,run_root/'authority.log','NATIVE_GRANT_AUTHORITY_READY',30)
 pe=penv(run_root/'provider-store')
 shutil.copy2(offerpriv,run_root/'provider/offer-private.pem')
 pm=[str(PROVIDER),'--plan',str(run_root/'provider/plan.json'),'--manifest',str(run_root/'provider/manifest.json'),'--service','/Inference/NativeUnary','--provider','/example/hello/provider','--group','/example/hello/group','--controller','/example/hello/controller','--trust-schema',str(run_root/'provider/trust-schema.conf'),'--roles','Prefix,Rank0,Rank1,Suffix','--workers','4','--handler-threads','4','--serve','--run-for-ms','120000','--artifact-cache-dir',str(run_root/'provider/cache'),'--selection-offer-key-file',str(run_root/'provider/offer-private.pem'),'--offer-backend','onnxruntime-cpu','--offer-can-provision','--offer-has-model']
 pe['SPEC181_GRANT_AUTHORITY_PUBLIC_KEY']=str(run_root/'provider/authority-public.pem');pe['SPEC181_PROVIDER_RECIPIENT_KEY_MAP']=str(run_root/'provider/recipient-key-map.json');pe['NDNSF_DI_WORKER_BINARY']=str(WORKER)
 p=launch(pm,pe,'provider');children.append(p); b.wait_marker(p,run_root/'provider.log','NDNSF_DI_NATIVE_PROVIDER_READY',45)
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
 print('ROOT',run_root)
 if rq.returncode != 0:
  raise SystemExit(rq.returncode)
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
