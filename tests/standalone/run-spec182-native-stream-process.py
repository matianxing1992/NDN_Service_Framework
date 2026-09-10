#!/usr/bin/env python3
"""Run the Spec182 native C++ token-stream process case.

The driver owns only process lifecycle and private NFD/PIB/TPM setup.  The
    request, grant verification, Provider execution, stream decoding, and
    token oracle remain in the C++ production executables.
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
parser.add_argument('--conversation', action='store_true',
                    help='run two persisted native conversation turns and a wrong-parent negative')
parser.add_argument('--recovery', action='store_true',
                    help='restart the Provider between turns and require safe native rejection')
args=parser.parse_args()
if args.recovery and not args.conversation:
 parser.error('--recovery requires --conversation')
BUILD=args.build.resolve()
CONTROLLER=BUILD/'examples/App_ServiceController'; AUTHORITY=BUILD/'examples/DI_NativeArtifactAuthority'; PROVIDER=BUILD/'examples/di-native-provider'; REQUESTER=BUILD/'examples/DI_NativeRequester'; WORKER=BUILD/'DI_NativeOnnxAssemblyWorker'
# make descriptor/catalog
sys.path[:0]=[str(ROOT/'NDNSF-DistributedInference'),str(ROOT/'NDNSF-DistributedRepo/pythonWrapper')]
from ndnsf_distributed_inference.splitter import AdapterDescriptor, ModelDescriptor, _canonical_bytes
def dg(x): return 'sha256:'+hashlib.sha256(x.encode()).hexdigest()
src=(ROOT/'tests/fixtures/spec175/tiny-causal-lm-v1/one-role/role-0.onnx').read_bytes(); src_digest='sha256:'+hashlib.sha256(src).hexdigest()
stream_role='/LLM/Pipeline/Stage/0'
stream_graph='sha256:fd13cfff524de62eb16c000c68cb9a09adcf5baa1083556acff0dc6c58228274'
ad=AdapterDescriptor('qwen-fixture','1',dg('fixture-adapter-state'),'fixture-abi-v1',('onnx',),('task',),('onnxruntime',),('float32',),dg('fixture-input-schema'),dg('fixture-options-schema'),dg('fixture-result-schema'),dg('fixture-graph-schema'),dg('fixture-split-schema'),dg('fixture-state-schema'),True,True,True)
model=ModelDescriptor('QwenFixture',dg('QwenFixture-content'),dg('QwenFixture-semantics'),stream_graph,'onnx','float32',ad,'pinned-r1')
package=dg('stream-package-manifest'); profile=dg('stream-artifact-profile'); asm=dg('stream-assembler'); offer_policy=dg('stream-offer-policy'); artifact=dg('stream-artifact')
node_mapping={'embedding':[0],'layer-00':list(range(1,8)),'layer-01':list(range(8,15)),'layer-02':list(range(15,22)),'layer-03':list(range(22,29)),'final-norm-head':[29,30]}
state_inputs={stream_role:{'attention_kv_in':['attention_kv_in'],'recurrent_state_in':['recurrent_state_in'],'convolution_state_in':['convolution_state_in']}}
state_outputs={stream_role:{'attention_kv_out':['attention_kv_out'],'recurrent_state_out':['recurrent_state_out'],'convolution_state_out':['convolution_state_out']}}
catalog={"schema":"ndnsf-di-native-request-catalog-v1","model":json.loads(_canonical_bytes(model).decode()),"source":{"data_name":"/catalog/qwen/source","digest":src_digest,"model_manifest_digest":package,"canonical_graph_digest":"sha256:b162fb0cb3735aa3209939521052ca5ff290aadbf9a1f9567f35f7d3dc912ef4"},"recipe":{"artifact_profile_digest":profile,"assembler_descriptor_digest":asm,"backend_abi":"onnxruntime-cpu-v1","precision":"float32","quantization":"none","layout":"native","padding":"none","protection_epoch":"epoch-1","max_source_bytes":1000000,"max_assembled_bytes":1000000,"max_nodes":64},"publication":{"artifact_root":"/Model/QwenFixture/artifacts","package_manifest_digest":package},"input_format":"OPAQUE","max_payload_bytes":4096,"splitter":{"kind":"QWEN","layer_ranges":[[0,4]],"artifact_digests_by_role":{stream_role:artifact},"weight_bytes_by_role":{stream_role:1},"roles":[stream_role],"tensor_degrees":[1],"input_ingress_role":stream_role,"result_egress_role":stream_role},"node_mapping":node_mapping,"state_inputs":state_inputs,"state_outputs":state_outputs}
plan={"version":2,"services":[{"service":"/Inference/NativeStream","schemaVersion":2,"model":"/Model/QwenFixture","modelFamily":"qwen","modelFormat":"onnx","plannerKind":"native-qwen-layer","runtimeBackend":"onnxruntime","executionPolicy":"DATA_DRIVEN_V2","roles":[stream_role],"dependencies":[],"planner":{"schemaVersion":2,"modelFamily":"qwen","modelFormat":"onnx","plannerKind":"native-qwen-layer","runtimeBackend":"onnxruntime","executionPolicy":"DATA_DRIVEN_V2"}}]}
manifest={"services":[{"name":"/Inference/NativeStream","model":"/Model/QwenFixture","roles":[stream_role],"artifacts":[{"artifact":"/Artifact/Qwen/Stage/0","backend":"onnxruntime-cpu","kind":"model","role":stream_role,"metadata":{"streamingGeneration":"true","statefulModel":"true","maxGeneratedTokens":"8","eosTokenIds":"2","samplingDigest":"sha256:f924aa62a0ced09ee7e05e43971f95cdae88f7cc116b2dd9bf6ca8d6661d224b","inputNames":"input_ids,attention_kv_in,recurrent_state_in,convolution_state_in","outputNames":"logits,attention_kv_out,recurrent_state_out,convolution_state_out","stateInputNames":"attention_kv_in,recurrent_state_in,convolution_state_in","stateOutputNames":"attention_kv_out,recurrent_state_out,convolution_state_out","executionProvider":"cpu","fragmentDigest":artifact}}],"input":{"codec":"tensor-bundle","fields":{"input_ids":{"dtype":"int64"}}},"output":{"codec":"json"}}]}
# provider plan and manifest files generated later

def policy_text():
 return '''name /example/hello/controller/NDNSF/ControllerPolicy/v1\n\nprovider-policies\n{\n provider-policy\n {\n  for /example/hello/authority\n  allow { /HELLO }\n }\n provider-policy\n {\n  for /example/hello/provider\n  allow { /Inference/NativeStream\n  /Inference/NativeStream/ROLE/LLM/Pipeline/Stage/0 }\n }\n}\n\nuser-policies\n{\n user-policy\n {\n  for /example/hello/user\n  allow { /Inference/NativeStream\n  /HELLO }\n }\n}\n'''
if args.run_root is None:
 run_root=Path(tempfile.mkdtemp(prefix='spec182-r11-b3-probe-',dir='/tmp'))
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
registry={"schemaVersion":1,"status":"CONFIGURED","artifactPolicyAuthority":{"publicKeyAlgorithm":"ed25519","signatureAlgorithm":"ed25519","grantSchema":"ndnsf-di-key-grant-v1","authorityId":"/example/hello/authority","keyId":"model-key","publicKeyPath":"provider/authority-public.pem","publicKeySha256":"sha256:"+hashlib.sha256(authpub.read_bytes()).hexdigest(),"acceptedModelFamilies":["QwenFixture"],"protectionEpochs":["epoch-1"]}}
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
op={"schema":"spec180-provider-offer-trust-v1","candidateId":"b2","candidateDigest":offer_policy,"trustSchema":"/example/hello/trust","entries":[{"provider":"/example/hello/provider","service":"/Inference/NativeStream","keyLocatorPrefix":keyprefix,"signerKeyId":"sha256:"+hashlib.sha256(load_pem_public_key(offerpub.read_bytes(), backend=default_backend()).public_bytes(Encoding.Raw,PublicFormat.Raw)).hexdigest(),"certificateName":cert}]}
# requester config
rc={"schema":"ndnsf-di-native-requester-v1","catalog":dict(catalog,source=dict(catalog['source'],file='model.onnx')),"core":{"requester_identity":"/example/hello/user","authority_identity":"/example/hello/controller","group":"/example/hello/group","trust_schema_file":"trust-schema.conf"},"grant":{"authority_identity":"/example/hello/authority","authority_service":"/HELLO","authority_public_key_file":"authority-public.pem","requester_private_key_file":"requester-private.pem","protection_epoch":"epoch-1"},"offer_admission":{"policy":op,"public_key_files":{op['entries'][0]['signerKeyId']:'offer-public.pem'},"candidate_digest":offer_policy},"limits":{"bootstrap_ms":30000,"max_source_bytes":1000000,"max_assembled_bytes":1000000},"request":{"service":"/Inference/NativeStream","task":"task","adapter_composition_digest":ad.descriptor_digest,"task_descriptor_digest":dg('task'),"generation_mode":"TOKEN_STREAMING","tokenizer_digest":"sha256:bf0f0fa65dc5aafe690ee497b4c8e2abe408fb4788c5ef035964dc94f15ca5a6","input_layout_digest":dg('input-layout'),"security_policy_digest":dg('security'),"max_candidates":1,"max_policy_ms":1000,"provider_names":["/example/hello/provider"],"max_reentries":1,"no_progress_ms":5000,"timeout_ms":30000,"ack_timeout_ms":5000}}
options={"useCache":True,"outputMode":"TOKEN_STREAMING","generationId":"0123456789abcdef0123456789abcdef","maxNewTokens":8,"tokenizerDigest":"sha256:bf0f0fa65dc5aafe690ee497b4c8e2abe408fb4788c5ef035964dc94f15ca5a6","eosTokenIds":[2],"sampling":{"mode":"Greedy","temperature":0.0,"topK":1,"topP":1.0,"repetitionPenalty":1.0,"seed":1750001},"stopStrings":[],"tokenInputName":"input_ids","stateInputNames":["attention_kv_in","recurrent_state_in","convolution_state_in"],"stateOutputNames":["attention_kv_out","recurrent_state_out","convolution_state_out"]}
(run_root/'requester/options.json').write_text(json.dumps(options)); rc['request']['options_file']='options.json'; rc['stream_oracle']={'token_ids':[4,5,6,7,8,9,10,2]}
if args.conversation:
 conversation_key_dir=run_root/'requester/keys'; conversation_key_dir.mkdir(mode=0o700)
 conversation_key=(conversation_key_dir/'conversation.key'); conversation_key.write_bytes(os.urandom(32)); conversation_key.chmod(0o600)
 role_map_digest=dg(json.dumps([[stream_role,'/example/hello/provider']],separators=(',',':')))
 retention=int(time.time()*1000)+120000
 # The Provider receipt binds this field to the authenticated Selection
 # security-policy snapshot carried by the request contract.
 rc['conversation']={"schema":"ndnsf-di-native-conversation-v1","journal":{"state_root":"conversation-state","identity":"requester-a","keys":[{"id":"active","file":"keys/conversation.key"}],"quota_bytes":67108864,"test_only_allow_ephemeral_state_root":True},"owner":{"requester_identity":"/example/hello/user","service_name":"/Inference/NativeStream","security_domain_digest":dg('security')},"turn":{"conversation_id":"spec182-r11-b4-conversation","parent_context_epoch":0,"service_name":"/Inference/NativeStream","plan_role_map_digest":role_map_digest,"retention_deadline_ms":retention,"mode":"FULL_CONTEXT","generation_id":options['generationId'],"canonical_token_ids":[3],"expected_roles":[stream_role]},"checkpoint_output_file":"conversation-state.json"}
(run_root/'requester/config.json').write_text(json.dumps(rc))
(run_root/'provider/plan.json').write_text(json.dumps(plan));(run_root/'provider/manifest.json').write_text(json.dumps(manifest));
# input bundle
payload=struct.pack('<q',3); outb=b'NDITB001'+struct.pack('<I',1)+struct.pack('<I',9)+b'input_ids'+struct.pack('<I',3)+struct.pack('<I',2)+struct.pack('<q',1)+struct.pack('<q',1)+struct.pack('<Q',len(payload))+payload;(run_root/'requester/input.bin').write_bytes(outb)
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
 pm=[str(PROVIDER),'--plan',str(run_root/'provider/plan.json'),'--manifest',str(run_root/'provider/manifest.json'),'--service','/Inference/NativeStream','--provider','/example/hello/provider','--group','/example/hello/group','--controller','/example/hello/controller','--trust-schema',str(run_root/'provider/trust-schema.conf'),'--roles',stream_role,'--serve','--run-for-ms','60000','--artifact-cache-dir',str(run_root/'provider/cache'),'--tokenizer-json',str(ROOT/'tests/fixtures/spec175/tiny-causal-lm-v1/standalone/tokenizer.json'),'--selection-offer-key-file',str(run_root/'provider/offer-private.pem'),'--offer-backend','onnxruntime-cpu','--offer-can-provision','--offer-has-model']
 pe['SPEC181_GRANT_AUTHORITY_PUBLIC_KEY']=str(run_root/'provider/authority-public.pem');pe['SPEC181_PROVIDER_RECIPIENT_KEY_MAP']=str(run_root/'provider/recipient-key-map.json');pe['NDNSF_DI_WORKER_BINARY']=str(WORKER)
 p=launch(pm,pe,'provider');children.append(p); b.wait_marker(p,run_root/'provider.log','NDNSF_DI_NATIVE_PROVIDER_READY',45)
 # requester direct first (and, when requested, a second process using the
 # native journal checkpoint produced by the first process).
 re=penv(run_root/'requester-store'); re.update({'NDNSF_DI_WORKER_BINARY':str(WORKER)})
 rq=launch([str(REQUESTER),'--config',str(run_root/'requester/config.json'),'--input',str(run_root/'requester/input.bin'),'--output',str(run_root/'requester/output.bin')],re,'requester');children.append(rq); rq.wait();print('requester rc',rq.returncode)
 if args.conversation and rq.returncode == 0:
  state_path=run_root/'requester/conversation-state.json'
  if not state_path.is_file(): raise RuntimeError('conversation checkpoint handoff missing')
  if args.recovery:
   p.kill(); stopped=p.wait(); children.remove(p); print('provider first rc',stopped)
   p=launch(pm,pe,'provider-restart'); children.append(p)
   b.wait_marker(p,run_root/'provider-restart.log','NDNSF_DI_NATIVE_PROVIDER_READY',45)
  second=json.loads((run_root/'requester/config.json').read_text())
  # A resumed turn keeps the conversation parent but must use a fresh
  # request/generation identity.  The native coordinator rejects reusing the
  # first turn's generationId because it would alias request-local state.
  second_generation_id='fedcba9876543210fedcba9876543210'
  second_options=dict(options, generationId=second_generation_id)
  (run_root/'requester/options-second.json').write_text(json.dumps(second_options))
  second['request']['options_file']='options-second.json'
  second['conversation']['turn']={"mode":"APPEND_DELTA","generation_id":second_generation_id,"parent_state_file":"conversation-state.json","delta_token_ids":[3]}
  second.pop('stream_oracle',None)
  second['conversation'].pop('checkpoint_output_file',None)
  (run_root/'requester/config-second.json').write_text(json.dumps(second))
  rq2=launch([str(REQUESTER),'--config',str(run_root/'requester/config-second.json'),'--input',str(run_root/'requester/input.bin'),'--output',str(run_root/'requester/output-second.bin')],re,'requester-second');children.append(rq2); rq2.wait(); print('requester-second rc',rq2.returncode)
  wrong=json.loads((run_root/'requester/config-second.json').read_text())
  wrong['conversation']['turn']['parent_checkpoint_digest']='sha256:'+'0'*64
  (run_root/'requester/config-wrong-parent.json').write_text(json.dumps(wrong))
  rq3=launch([str(REQUESTER),'--config',str(run_root/'requester/config-wrong-parent.json'),'--input',str(run_root/'requester/input.bin'),'--output',str(run_root/'requester/output-wrong-parent.bin')],re,'requester-wrong-parent');children.append(rq3); rq3.wait(); print('requester-wrong-parent rc',rq3.returncode)
 print('ROOT',run_root)
 for name, markers in {
  'requester.log': ('NATIVE_STREAM_ORACLE_PASS', 'NATIVE_REQUEST_SUCCEEDED', 'NATIVE_CONVERSATION_CHECKPOINT_WRITTEN'),
  **({'requester-second.log': (('NATIVE_REQUEST_SUCCEEDED',) if not args.recovery else ('NATIVE_STREAM_FAILED',)), 'requester-wrong-parent.log': ('DI_NATIVE_CONVERSATION_PARENT_MISMATCH',)} if args.conversation else {}),
  'provider.log': ('NDNSF_DI_GRANT_VERIFICATION', 'NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED'),
  **({'provider-restart.log': ('NDNSF_DI_NATIVE_PROVIDER_READY', 'PROVIDER_CONVERSATION_STATE_MISSING')} if args.recovery else {}),
 }.items():
  log_path=run_root/name
  if not log_path.is_file():
   raise RuntimeError(f'missing expected log: {log_path}')
  lines=log_path.read_text(errors='replace').splitlines()
  matched=[line for line in lines if any(marker in line for marker in markers)]
  print(name, matched)
  missing=[marker for marker in markers if not any(marker in line for line in lines)]
  if missing: raise RuntimeError(f'missing expected markers in {name}: {missing}')
 if rq.returncode: raise RuntimeError('request failed')
 if args.conversation and rq.returncode == 0:
  if args.recovery:
   if rq2.returncode == 0: raise RuntimeError('recovery append unexpectedly succeeded')
   second_log=(run_root/'requester-second.log').read_text(errors='replace')
   if 'NATIVE_REQUEST_SUCCEEDED' in second_log or 'NATIVE_CONVERSATION_CHECKPOINT_WRITTEN' in second_log:
    raise RuntimeError('recovery append emitted a success/checkpoint marker')
   restart_log=(run_root/'provider-restart.log').read_text(errors='replace')
   if 'NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED' in restart_log or 'STREAM_EVENT_OBSERVED' in restart_log:
    raise RuntimeError('recovery restart executed or published a duplicate prefix')
  elif rq2.returncode != 0: raise RuntimeError('conversation append request failed')
  if rq3.returncode == 0: raise RuntimeError('wrong parent unexpectedly succeeded')
finally:
 for p in reversed(children): b.stop(p)
