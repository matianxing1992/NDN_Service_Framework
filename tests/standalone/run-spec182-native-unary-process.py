#!/usr/bin/env python3
"""Run the Spec182 native protected unary process case.

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
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=ROOT/'.codex-tmp/spec182-r11-b2-fresh-20260910/build')
parser.add_argument('--run-root', type=Path, default=None,
                    help='retain the raw run under this directory')
args=parser.parse_args()
BUILD=args.build.resolve()
CONTROLLER=BUILD/'examples/App_ServiceController'; AUTHORITY=BUILD/'examples/DI_NativeArtifactAuthority'; PROVIDER=BUILD/'examples/di-native-provider'; REQUESTER=BUILD/'examples/DI_NativeRequester'; WORKER=BUILD/'DI_NativeOnnxAssemblyWorker'
# make descriptor/catalog
sys.path[:0]=[str(ROOT/'NDNSF-DistributedInference'),str(ROOT/'NDNSF-DistributedRepo/pythonWrapper')]
from ndnsf_distributed_inference.splitter import AdapterDescriptor, ModelDescriptor, _canonical_bytes
def dg(x): return 'sha256:'+hashlib.sha256(x.encode()).hexdigest()
yolo=json.loads((ROOT/'tests/fixtures/spec182/yolo-semantic-oracle.json').read_text())
src=bytes.fromhex(yolo['model_hex']); src_digest='sha256:'+hashlib.sha256(src).hexdigest()
ad=AdapterDescriptor('yolo26n','1',dg('fixture-adapter-state'),'fixture-abi-v1',('onnx',),('task',),('onnxruntime',),('float32',),dg('fixture-input-schema'),dg('fixture-options-schema'),dg('fixture-result-schema'),dg('fixture-graph-schema'),dg('fixture-split-schema'),dg('fixture-state-schema'),True,True,True)
model=ModelDescriptor('YOLOFixture',dg('YOLOFixture-content'),dg('YOLOFixture-semantics'),yolo['graph_digest'],'onnx','float32',ad)
package=dg('yolo-package-manifest'); profile=dg('yolo-artifact-profile'); asm=dg('yolo-assembler'); offer_policy=dg('yolo-offer-policy')
catalog={"schema":"ndnsf-di-native-request-catalog-v1","model":json.loads(_canonical_bytes(model).decode()),"source":{"data_name":"/catalog/yolo/source","digest":src_digest,"model_manifest_digest":package,"canonical_graph_digest":"sha256:1fc5a2c048d90edff32c573deaf87e3262292455e37e92311f329f3ffedaaacf"},"recipe":{"artifact_profile_digest":profile,"assembler_descriptor_digest":asm,"backend_abi":"onnxruntime-cpu-v1","precision":"float32","quantization":"none","layout":"native","padding":"none","protection_epoch":"epoch-1","max_source_bytes":1000000,"max_assembled_bytes":1000000,"max_nodes":64},"publication":{"artifact_root":"/Model/YOLOFixture/artifacts","package_manifest_digest":package},"input_format":"OPAQUE","max_payload_bytes":4096,"splitter":{"kind":"YOLO","components":[{"candidate_id":"atomic-v1","priority":0,"roles":["FullModel"],"node_names_by_role":{},"input_ingress_role":"FullModel","result_egress_role":"FullModel","merge_kind":"","candidate_digest":dg('atomic-registered'),"semantic_partition":{}}],"postprocessing":{}},"node_mapping":{},"state_inputs":{},"state_outputs":{}}
plan={"version":2,"services":[{"service":"/Inference/NativeUnary","schemaVersion":2,"model":"/Model/YOLOFixture","modelFamily":"yolo","modelFormat":"onnx","plannerKind":"native-yolo-component","runtimeBackend":"onnxruntime","executionPolicy":"DATA_DRIVEN_V2","roles":["FullModel"],"dependencies":[],"planner":{"schemaVersion":2,"modelFamily":"yolo","modelFormat":"onnx","plannerKind":"native-yolo-component","runtimeBackend":"onnxruntime","executionPolicy":"DATA_DRIVEN_V2"}}]}
manifest={"services":[{"name":"/Inference/NativeUnary","model":"/Model/YOLOFixture","roles":["FullModel"],"artifacts":[{"artifact":"/Artifact/YOLO/FullModel","backend":"onnxruntime-cpu","kind":"model","role":"FullModel"}],"input":{"codec":"tensor-bundle","fields":{"images":{"dtype":"float32","shape":[1,3]}}},"output":{"codec":"tensor-bundle","fields":{"predictions":{"dtype":"float32"}}}}]}
# provider plan and manifest files generated later

def policy_text():
 return '''name /example/hello/controller/NDNSF/ControllerPolicy/v1\n\nprovider-policies\n{\n provider-policy\n {\n  for /example/hello/authority\n  allow { /HELLO }\n }\n provider-policy\n {\n  for /example/hello/provider\n  allow { /Inference/NativeUnary\n  /Inference/NativeUnary/ROLE/FullModel }\n }\n}\n\nuser-policies\n{\n user-policy\n {\n  for /example/hello/user\n  allow { /Inference/NativeUnary\n  /HELLO }\n }\n}\n'''
if args.run_root is None:
 run_root=Path(tempfile.mkdtemp(prefix='spec182-r11-b2-probe-',dir='/tmp'))
else:
 run_root=args.run_root.resolve()
 run_root.mkdir(parents=True, exist_ok=True)
run_root.chmod(0o700)
for n in ['shared','bootstrap-store','authority','provider','requester']:(run_root/n).mkdir(mode=0o700)
(run_root/'shared/home').mkdir(); (run_root/'bootstrap-store/home').mkdir()
nfd_socket=run_root/'nfd.sock'; nfdconf=run_root/'nfd.conf'; b.nfd_config(nfdconf,nfd_socket); policy=run_root/'hello.policies'; b.write(policy,policy_text(),0o600)
# bootstrap identities
bootenv=os.environ.copy(); bootenv.update({'HOME':str(run_root/'bootstrap-store/home'),'NDN_CLIENT_PIB':'pib-sqlite3:'+str(run_root/'bootstrap-store/pib'),'NDN_CLIENT_TPM':'tpm-file:'+str(run_root/'bootstrap-store/tpm'),'PATH':'/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin'})
for ident in ['/example/hello/controller','/example/hello/authority','/example/hello/provider','/example/hello/user']:
 subprocess.run(['/usr/local/bin/ndnsec','key-gen','-t','r',ident],env=bootenv,check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
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
(run_root/'authority/trust-schema.conf').write_text((ROOT/'examples/trust-schema.conf').read_text()); (run_root/'provider/trust-schema.conf').write_text((ROOT/'examples/trust-schema.conf').read_text())
# registry for provider
registry={"schemaVersion":1,"status":"CONFIGURED","artifactPolicyAuthority":{"publicKeyAlgorithm":"ed25519","signatureAlgorithm":"ed25519","grantSchema":"ndnsf-di-key-grant-v1","authorityId":"/example/hello/authority","keyId":"model-key","publicKeyPath":"provider/authority-public.pem","publicKeySha256":"sha256:"+hashlib.sha256(authpub.read_bytes()).hexdigest(),"acceptedModelFamilies":["YOLOFixture"],"protectionEpochs":["epoch-1"]}}
(run_root/'provider/trust-root-registry-v1.json').write_text(json.dumps(registry)); shutil.copy2(authpub,run_root/'provider/authority-public.pem'); (run_root/'provider/recipient-private.pem').write_bytes(recippriv.read_bytes()); (run_root/'provider/recipient-private.pem').chmod(0o600); (run_root/'provider/recipient-key-map.json').write_text(json.dumps({'/example/hello/provider':str(run_root/'provider/recipient-private.pem')}))
# authority config
ac={"schema":"ndnsf-di-native-authority-v1","run_for_ms":120000,"permission_bootstrap_ms":30000,"max_grant_ttl_ms":60000,"authority":{"identity":"/example/hello/authority","service":"/HELLO","group":"/example/hello/group","controller_identity":"/example/hello/controller","requester_identity":"/example/hello/user","protection_epoch":"epoch-1","content_key_id":"model-key","trust_schema_file":"trust-schema.conf","authority_private_key_file":"private.pem","requester_public_key_file":"../requester/public.pem","content_key_file":"content-key.bin","allowed_model_manifests":[package],"recipient_public_key_files":{"/example/hello/provider":"../provider/public.pem"},"publication_sources":{package:{"model_name":model.model_name,"model_content_digest":model.content_digest,"canonical_source_digest":src_digest,"artifact_profile_digest":profile}}}}
(run_root/'authority/authority.json').write_text(json.dumps(ac))
# requester files
(run_root/'requester/requester-private.pem').write_bytes(reqpriv.read_bytes());(run_root/'requester/requester-private.pem').chmod(0o600); shutil.copy2(authpub,run_root/'requester/authority-public.pem'); shutil.copy2(offerpub,run_root/'requester/offer-public.pem'); (run_root/'requester/trust-schema.conf').write_text((ROOT/'examples/trust-schema.conf').read_text()); (run_root/'requester/model.onnx').write_bytes(src); (run_root/'requester/catalog.json').write_text(json.dumps(catalog));
# Cert name query from the provider PIB lets the requester validate the
# signed offer without sharing the provider's private NDN key.
from ndn.encoding import Name,Component
with sqlite3.connect(run_root/'provider-store/pib/pib.db') as db: kb=db.execute("select key_name from keys").fetchall()
keyblob=next(bytes(row[0]) for row in kb if b'provider' in bytes(row[0]))
parts,_=Name.decode(keyblob); keyprefix='/'+'/'.join(Component.to_str(x.tobytes()) for x in parts); cert=keyprefix+'/ID-CERT'
# offer policy
op={"schema":"spec180-provider-offer-trust-v1","candidateId":"b2","candidateDigest":offer_policy,"trustSchema":"/example/hello/trust","entries":[{"provider":"/example/hello/provider","service":"/Inference/NativeUnary","keyLocatorPrefix":keyprefix,"signerKeyId":"sha256:"+hashlib.sha256(load_pem_public_key(offerpub.read_bytes(), backend=default_backend()).public_bytes(Encoding.Raw,PublicFormat.Raw)).hexdigest(),"certificateName":cert}]}
# requester config
rc={"schema":"ndnsf-di-native-requester-v1","catalog":dict(catalog,source=dict(catalog['source'],file='model.onnx')),"core":{"requester_identity":"/example/hello/user","authority_identity":"/example/hello/controller","group":"/example/hello/group","trust_schema_file":"trust-schema.conf"},"grant":{"authority_identity":"/example/hello/authority","authority_service":"/HELLO","authority_public_key_file":"authority-public.pem","requester_private_key_file":"requester-private.pem","protection_epoch":"epoch-1"},"offer_admission":{"policy":op,"public_key_files":{op['entries'][0]['signerKeyId']:'offer-public.pem'},"candidate_digest":offer_policy},"limits":{"bootstrap_ms":30000,"max_source_bytes":1000000,"max_assembled_bytes":1000000},"request":{"service":"/Inference/NativeUnary","task":"task","adapter_composition_digest":ad.descriptor_digest,"task_descriptor_digest":dg('task'),"generation_mode":"TOKEN_DIAGNOSTIC","input_layout_digest":dg('input-layout'),"security_policy_digest":dg('security'),"max_candidates":1,"max_policy_ms":1000,"provider_names":["/example/hello/provider"],"max_reentries":1,"no_progress_ms":5000,"timeout_ms":30000,"ack_timeout_ms":5000}}
rc['oracle']={'tensor':'predictions','float32':[4.0,0.0,12.0],'tolerance':1e-5};(run_root/'requester/config.json').write_text(json.dumps(rc))
(run_root/'provider/plan.json').write_text(json.dumps(plan));(run_root/'provider/manifest.json').write_text(json.dumps(manifest));
# input bundle
vals=[1.,-2.,3.]; payload=b''.join(struct.pack('<f',v) for v in vals); outb=b'NDITB001'+struct.pack('<I',1)+struct.pack('<I',6)+b'images'+struct.pack('<I',1)+struct.pack('<I',2)+struct.pack('<q',1)+struct.pack('<q',3)+struct.pack('<Q',len(payload))+payload;(run_root/'requester/input.bin').write_bytes(outb)
# launch env
base=os.environ.copy(); base.update({'NDN_CLIENT_TRANSPORT':'unix://'+str(nfd_socket),'PATH':'/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin','NDNSF_CONTROLLER_GENERATION_STATE':str(run_root/'shared/controller-generation.state'),'NDN_LOG':'ndn_service_framework.*=TRACE'})
def launch(cmd,env,name):
 p=b.run(cmd,env,run_root/(name+'.log'),wait=False)
 (run_root/(name+'.pid')).write_text(str(p.pid)+'\n')
 return p
children=[]
try:
 nfd=launch(['/usr/local/bin/nfd','--config',str(nfdconf)],base,'nfd');children.append(nfd)
 while not nfd_socket.exists(): time.sleep(.05)
 subprocess.run(['/usr/local/bin/nfdc','strategy','set','/example/hello/group','/localhost/nfd/strategy/multicast'],env=base,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 def penv(store):
  e=dict(base);e.update({'HOME':str(store/'home'),'NDN_CLIENT_PIB':'pib-sqlite3:'+str(store/'pib'),'NDN_CLIENT_TPM':'tpm-file:'+str(store/'tpm')});return e
 c=launch([str(CONTROLLER),'--controller-prefix','/example/hello/controller','--policy-file',str(policy),'--ensure-identities','/example/hello/authority,/example/hello/provider,/example/hello/user','--no-serve-certificates','--run-for-ms','120000'],penv(run_root/'controller-store'),'controller');children.append(c); b.wait_marker(c,run_root/'controller.log','ServiceController started...',30)
 a=launch([str(AUTHORITY),'--config',str(run_root/'authority/authority.json')],penv(run_root/'authority-store'),'authority');children.append(a); b.wait_marker(a,run_root/'authority.log','NATIVE_GRANT_AUTHORITY_READY',30)
 pe=penv(run_root/'provider-store')
 shutil.copy2(offerpriv,run_root/'provider/offer-private.pem')
 pm=[str(PROVIDER),'--plan',str(run_root/'provider/plan.json'),'--manifest',str(run_root/'provider/manifest.json'),'--service','/Inference/NativeUnary','--provider','/example/hello/provider','--group','/example/hello/group','--controller','/example/hello/controller','--trust-schema',str(run_root/'provider/trust-schema.conf'),'--roles','FullModel','--serve','--run-for-ms','60000','--artifact-cache-dir',str(run_root/'provider/cache'),'--selection-offer-key-file',str(run_root/'provider/offer-private.pem'),'--offer-backend','onnxruntime-cpu','--offer-can-provision','--offer-has-model']
 pe['SPEC181_GRANT_AUTHORITY_PUBLIC_KEY']=str(run_root/'provider/authority-public.pem');pe['SPEC181_PROVIDER_RECIPIENT_KEY_MAP']=str(run_root/'provider/recipient-key-map.json');pe['NDNSF_DI_WORKER_BINARY']=str(WORKER)
 p=launch(pm,pe,'provider');children.append(p); b.wait_marker(p,run_root/'provider.log','NDNSF_DI_NATIVE_PROVIDER_READY',45)
 # requester direct first
 re=penv(run_root/'requester-store'); re.update({'NDNSF_DI_WORKER_BINARY':str(WORKER)})
 rq=launch([str(REQUESTER),'--config',str(run_root/'requester/config.json'),'--input',str(run_root/'requester/input.bin'),'--output',str(run_root/'requester/output.bin')],re,'requester');children.append(rq); rq.wait();print('requester rc',rq.returncode)
 print('ROOT',run_root)
 for name, markers in {
  'requester.log': ('NATIVE_NUMERICAL_ORACLE_PASS', 'NATIVE_REQUEST_SUCCEEDED'),
  'provider.log': ('NDNSF_DI_GRANT_VERIFICATION', 'NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED'),
 }.items():
  lines=(run_root/name).read_text(errors='replace').splitlines()
  print(name, [line for line in lines if any(marker in line for marker in markers)])
 if rq.returncode: raise RuntimeError('request failed')
finally:
 for p in reversed(children): b.stop(p)
