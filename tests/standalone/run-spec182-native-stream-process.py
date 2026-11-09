#!/usr/bin/env python3
"""Run the Spec182 native C++ token-stream process case.

The driver owns only process lifecycle and private NFD/PIB/TPM setup.  The
    request, grant verification, Provider execution, stream decoding, and
    token oracle remain in the C++ production executables.
"""

import argparse
import copy
import sys, json, signal
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
parser.add_argument('--replacement', action='store_true',
                    help='fail the first Provider after ACK and require a second Provider replacement')
parser.add_argument('--replacement-no-backup', action='store_true',
                    help='fail the only Provider and require one terminal native failure')
parser.add_argument('--stored-model-reuse', action='store_true',
                    help='A publishes to real Repo, then independent B/C use the stored model')
parser.add_argument('--request-time-model-preparation', action='store_true',
                    help='use native source preparation/open before the stored reuse case')
args=parser.parse_args()
if args.request_time_model_preparation and not args.stored_model_reuse:
 parser.error('--request-time-model-preparation requires --stored-model-reuse')
driver_started=time.monotonic()
if args.recovery and not args.conversation:
 parser.error('--recovery requires --conversation')
if args.replacement_no_backup and not args.replacement:
 parser.error('--replacement-no-backup requires --replacement')
if args.replacement and args.conversation:
 parser.error('--replacement cannot be combined with --conversation')
if args.stored_model_reuse and (args.conversation or args.recovery or args.replacement):
 parser.error('--stored-model-reuse owns its independent conversation cases')
# At most six children remain live; each stop is bounded by4+2s. Reserve45s
# inside the existing180s outer watchdog rather than stacking phase timeouts.
work_deadline=driver_started+135
def remaining_timeout(requested):
 if not args.stored_model_reuse:
  return requested
 remaining=work_deadline-time.monotonic()
 if remaining <= 0:
  raise TimeoutError('stored process shared work deadline expired')
 return min(requested,remaining)
def run_command(*command,**options):
 if args.stored_model_reuse:
  options['timeout']=remaining_timeout(options.get('timeout',30))
 return subprocess.run(*command,**options)
def wait_ready(process,log,marker,timeout):
 return b.wait_marker(process,log,marker,remaining_timeout(timeout))
if args.stored_model_reuse:
 def terminate_owned_run(number,frame):
  raise RuntimeError('stored process received termination signal '+str(number))
 signal.signal(signal.SIGTERM,terminate_owned_run)
 signal.signal(signal.SIGINT,terminate_owned_run)
BUILD=args.build.resolve()
CONTROLLER=BUILD/'examples/App_ServiceController'; AUTHORITY=BUILD/'examples/DI_NativeArtifactAuthority'; PROVIDER=BUILD/'examples/di-native-provider'; REQUESTER=BUILD/'examples/DI_NativeRequester'; WORKER=BUILD/'DI_NativeOnnxAssemblyWorker'
provider_a='/example/hello/provider-a' if args.replacement else '/example/hello/provider'
provider_b='/example/hello/provider-b'
provider_names=[provider_a]
repo_provider='/example/hello/repo-provider'
repo_node='/example/hello/repo-node'
repo_service='/Repo/Spec222'
consumer_ids={'consumer-b':'/example/hello/consumer-b',
              'consumer-c':'/example/hello/consumer-c'}
repo_suffixes=('USER_STORAGE_GRANT','USER_STORAGE_USAGE','USER_STORAGE_LIST',
              'USER_STORAGE_DELETE','STORE','STORE_RANGE','COMMIT_RANGES','ABORT_RANGES',
              'INSERT','STORE_MANIFEST','FETCH','FETCH_RANGE','MANIFEST','DATA_RECEIPT',
              'DATA_LOCATION','INVENTORY','CAPABILITY','CACHE_STATUS','STATUS',
              'CATALOG_STATUS','CATALOG_SNAPSHOT')
if args.replacement and not args.replacement_no_backup:
 provider_names.append(provider_b)
# make descriptor/catalog.  These helpers are deliberately pure Python fixture
# metadata; the process driver must not import the native Python wrapper when
# the selected C++ binaries are sanitizer-instrumented.
sys.path.insert(0, str(ROOT/'tests/standalone'))
from spec185_native_fixture import adapter_descriptor, canonical_bytes, digest, model_descriptor
def dg(x): return 'sha256:'+hashlib.sha256(x.encode()).hexdigest()
src=(ROOT/'tests/fixtures/spec175/tiny-causal-lm-v1/one-role/role-0.onnx').read_bytes(); src_digest='sha256:'+hashlib.sha256(src).hexdigest()
stream_role='/LLM/Pipeline/Stage/0'
stream_graph='sha256:fd13cfff524de62eb16c000c68cb9a09adcf5baa1083556acff0dc6c58228274'
ad=adapter_descriptor('qwen-fixture','1',dg('fixture-adapter-state'),'fixture-abi-v1',('onnx',),('task',),('onnxruntime',),('float32',),dg('fixture-input-schema'),dg('fixture-options-schema'),dg('fixture-result-schema'),dg('fixture-graph-schema'),dg('fixture-split-schema'),dg('fixture-state-schema'),True,True,True)
model=model_descriptor('QwenFixture',dg('QwenFixture-content'),dg('QwenFixture-semantics'),stream_graph,'onnx','float32',ad,'pinned-r1')
adapter_digest=digest(ad)
package=dg('stream-package-manifest'); profile=dg('stream-artifact-profile'); asm=dg('stream-assembler'); offer_policy=dg('stream-offer-policy'); artifact=dg('stream-artifact')
node_mapping={'embedding':[0],'layer-00':list(range(1,8)),'layer-01':list(range(8,15)),'layer-02':list(range(15,22)),'layer-03':list(range(22,29)),'final-norm-head':[29,30]}
state_inputs={stream_role:{'attention_kv_in':['attention_kv_in'],'recurrent_state_in':['recurrent_state_in'],'convolution_state_in':['convolution_state_in']}}
state_outputs={stream_role:{'attention_kv_out':['attention_kv_out'],'recurrent_state_out':['recurrent_state_out'],'convolution_state_out':['convolution_state_out']}}
catalog={"schema":"ndnsf-di-native-request-catalog-v1","model":json.loads(canonical_bytes(model).decode()),"source":{"data_name":"/catalog/qwen/source","digest":src_digest,"model_manifest_digest":package,"canonical_graph_digest":"sha256:b162fb0cb3735aa3209939521052ca5ff290aadbf9a1f9567f35f7d3dc912ef4"},"recipe":{"artifact_profile_digest":profile,"assembler_descriptor_digest":asm,"backend_abi":"onnxruntime-cpu-v1","precision":"float32","quantization":"none","layout":"native","padding":"none","protection_epoch":"epoch-1","max_source_bytes":1000000,"max_assembled_bytes":1000000,"max_nodes":64},"publication":{"artifact_root":"/Model/QwenFixture/artifacts","package_manifest_digest":package},"input_format":"OPAQUE","max_payload_bytes":4096,"conversation_input":{"kind":"TENSOR_BUNDLE_TOKEN_IDS","tensor_name":"input_ids"},"splitter":{"kind":"QWEN","layer_ranges":[[0,4]],"artifact_digests_by_role":{stream_role:artifact},"weight_bytes_by_role":{stream_role:1},"roles":[stream_role],"tensor_degrees":[1],"input_ingress_role":stream_role,"result_egress_role":stream_role},"node_mapping":node_mapping,"state_inputs":state_inputs,"state_outputs":state_outputs}
plan={"version":2,"services":[{"service":"/Inference/NativeStream","schemaVersion":2,"model":"/Model/QwenFixture","modelFamily":"qwen","modelFormat":"onnx","plannerKind":"native-qwen-layer","runtimeBackend":"onnxruntime","executionPolicy":"DATA_DRIVEN_V2","roles":[stream_role],"dependencies":[],"planner":{"schemaVersion":2,"modelFamily":"qwen","modelFormat":"onnx","plannerKind":"native-qwen-layer","runtimeBackend":"onnxruntime","executionPolicy":"DATA_DRIVEN_V2"}}]}
manifest={"services":[{"name":"/Inference/NativeStream","model":"/Model/QwenFixture","roles":[stream_role],"artifacts":[{"artifact":"/Artifact/Qwen/Stage/0","backend":"onnxruntime-cpu","kind":"model","role":stream_role,"metadata":{"streamingGeneration":"true","statefulModel":"true","maxGeneratedTokens":"8","eosTokenIds":"2","samplingDigest":"sha256:f924aa62a0ced09ee7e05e43971f95cdae88f7cc116b2dd9bf6ca8d6661d224b","inputNames":"input_ids,attention_kv_in,recurrent_state_in,convolution_state_in","outputNames":"logits,attention_kv_out,recurrent_state_out,convolution_state_out","stateInputNames":"attention_kv_in,recurrent_state_in,convolution_state_in","stateOutputNames":"attention_kv_out,recurrent_state_out,convolution_state_out","executionProvider":"cpu","fragmentDigest":artifact}}],"input":{"codec":"tensor-bundle","fields":{"input_ids":{"dtype":"int64"}}},"output":{"codec":"json"}}]}
# Match the existing native Qwen fixture's explicit resource profile.
# This is a functional tiny-model fixture, not a measured GPU memory profile.
catalog['splitter']['resources_by_role']={stream_role:{
 'backends':['onnxruntime'],'weight_bytes':1 << 20,'workspace_bytes':0,'kv_bytes':0,
 'activation_bytes':0,'transient_bytes':0,'safety_margin':1.0}}
# provider plan and manifest files generated later

def policy_text():
 providers=''.join(' provider-policy\n {\n  for %s\n  allow { /Inference/NativeStream\n  /Inference/NativeStream/ROLE/LLM/Pipeline/Stage/0 }\n }\n' % name for name in provider_names)
 users=' user-policy\n {\n  for /example/hello/user\n  allow { /Inference/NativeStream\n  /HELLO }\n }\n'
 if args.stored_model_reuse:
  services='\n'.join(repo_service+'/'+suffix for suffix in repo_suffixes)
  providers+=' provider-policy\n {\n  for '+repo_provider+'\n  allow { '+services+' }\n }\n'
  users=''.join(' user-policy\n {\n  for '+name+'\n  allow { '+services+' }\n }\n'
                for name in ('/example/hello/user',repo_provider))
  users+=''.join(' user-policy\n {\n  for '+name+'\n  allow { /Inference/NativeStream\n /HELLO }\n }\n'
                 for name in consumer_ids.values())
 return ('name /example/hello/controller/NDNSF/ControllerPolicy/v1\n\n'
         'provider-policies\n{\n provider-policy\n {\n  for /example/hello/authority\n  allow { /HELLO }\n }\n' + providers + '}\n\nuser-policies\n{\n'+users+'}\n')
if args.run_root is None:
 run_root=Path(tempfile.mkdtemp(prefix='spec182-r11-b3-probe-',dir='/tmp'))
else:
 run_root=args.run_root.resolve()
 run_root.mkdir(parents=True, exist_ok=True)
run_root.chmod(0o700)
provider_dirs=[('provider', provider_a)]
if args.replacement and not args.replacement_no_backup: provider_dirs.append(('provider-b', provider_b))
extra_dirs=['repo']+list(consumer_ids) if args.stored_model_reuse else []
for n in ['shared','bootstrap-store','authority','requester']+[d for d,_ in provider_dirs]+extra_dirs:(run_root/n).mkdir(mode=0o700)
(run_root/'shared/home').mkdir(); (run_root/'bootstrap-store/home').mkdir()
nfd_socket=b.bounded_nfd_socket(run_root); nfdconf=run_root/'nfd.conf'; b.nfd_config(nfdconf,nfd_socket); policy=run_root/'hello.policies'; b.write(policy,policy_text(),0o600)
# bootstrap identities
bootenv=os.environ.copy(); bootenv.update({'HOME':str(run_root/'bootstrap-store/home'),'NDN_CLIENT_PIB':'pib-sqlite3:'+str(run_root/'bootstrap-store/pib'),'NDN_CLIENT_TPM':'tpm-file:'+str(run_root/'bootstrap-store/tpm'),'PATH':'/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin'})
extra_identities=[repo_provider]+list(consumer_ids.values()) if args.stored_model_reuse else []
root_certificate=run_command(['/usr/local/bin/ndnsec','key-gen','-t','r','/example/hello'],
                             env=bootenv,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE).stdout
(run_root/'shared/root.cert').write_bytes(root_certificate)
actor_identities=['/example/hello/controller','/example/hello/authority']+provider_names+['/example/hello/user']+extra_identities
for ident in actor_identities:
 request=run_command(['/usr/local/bin/ndnsec','key-gen','-t','r',ident],env=bootenv,
                     check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE).stdout
 certificate=run_command(['/usr/local/bin/ndnsec','cert-gen','-s','/example/hello',
                         '-i','fixture','-'],input=request,env=bootenv,check=True,
                         stdout=subprocess.PIPE,stderr=subprocess.PIPE).stdout
 run_command(['/usr/local/bin/ndnsec','cert-install','-I','-'],input=certificate,env=bootenv,
             check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
fixture_schema=(ROOT/'examples/trust-schema.conf').read_text()
role_pattern='<user|8=user|provider|8=provider|controller|8=controller>'
if role_pattern not in fixture_schema:
 raise RuntimeError('example SVS signer policy changed; update fixture actor mapping')
actor_roles=sorted({name.rsplit('/',1)[1] for name in actor_identities})
fixture_schema=fixture_schema.replace(role_pattern,
 '<'+'|'.join(value for role in actor_roles for value in (role,'8='+role))+'>')
(run_root/'shared/trust-schema.conf').write_text(fixture_schema)
for directory in ['authority','requester']+[name for name,_ in provider_dirs]:
 (run_root/directory/'root.cert').write_bytes(root_certificate)
store_dirs=[('controller-store',['/example/hello/controller']),('authority-store',['/example/hello/authority']),('provider-store',[provider_a]),('requester-store',['/example/hello/user'])]
if args.replacement and not args.replacement_no_backup: store_dirs.insert(3,('provider-b-store',[provider_b]))
if args.stored_model_reuse:
 store_dirs.extend([('repo-store',[repo_provider])]+[(name+'-store',[identity])
                   for name,identity in consumer_ids.items()])
for n,_ in store_dirs:
 shutil.copytree(run_root/'bootstrap-store',run_root/n)
 with sqlite3.connect(run_root/n/'pib/pib.db') as db: db.execute('UPDATE tpmInfo SET tpm_locator=?',(f'tpm-file:{run_root/n}/tpm',)); db.commit()
for n,keep in store_dirs:
 actor_env=dict(bootenv)
 actor_env.update({'HOME':str(run_root/n/'home'),
                   'NDN_CLIENT_PIB':'pib-sqlite3:'+str(run_root/n/'pib'),
                   'NDN_CLIENT_TPM':'tpm-file:'+str(run_root/n/'tpm')})
 run_command(['/usr/local/bin/ndnsec','set-default',keep[0]],env=actor_env,check=True,
             stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
 b.remove_private_keys_except(run_root/n,keep)
# external keys
def edkey(d,prefix):
 prefix.mkdir(parents=True, exist_ok=True)
 priv=prefix/'private.pem'; pub=prefix/'public.pem'; run_command(['/usr/bin/openssl','genpkey','-algorithm','ED25519','-out',str(priv)],check=True,stdout=subprocess.DEVNULL); run_command(['/usr/bin/openssl','pkey','-in',str(priv),'-pubout','-out',str(pub)],check=True,stdout=subprocess.DEVNULL); priv.chmod(0o600); pub.chmod(0o644); return priv,pub
reqpriv,reqpub=edkey(0,run_root/'requester'); authpriv,authpub=edkey(0,run_root/'authority')
provider_keys={}
for dirname,name in provider_dirs:
 recippriv,recippub=edkey(0,run_root/dirname)
 offerpriv,offerpub=edkey(0,run_root/dirname/'offer')
 provider_keys[name]={'dir':dirname,'recipient_private':recippriv,'recipient_public':recippub,'offer_private':offerpriv,'offer_public':offerpub}
recippriv=provider_keys[provider_a]['recipient_private']; recippub=provider_keys[provider_a]['recipient_public']
offerpriv=provider_keys[provider_a]['offer_private']; offerpub=provider_keys[provider_a]['offer_public']
(run_root/'authority/content-key.bin').write_bytes(b'spec182-b2-content-key-000000000'); (run_root/'authority/content-key.bin').chmod(0o600)
(run_root/'authority/trust-schema.conf').write_text(fixture_schema)
for dirname,_ in provider_dirs: (run_root/dirname/'trust-schema.conf').write_text(fixture_schema)
# registry for provider
registry={"schemaVersion":1,"status":"CONFIGURED","artifactPolicyAuthority":{"publicKeyAlgorithm":"ed25519","signatureAlgorithm":"ed25519","grantSchema":"ndnsf-di-key-grant-v1","authorityId":"/example/hello/authority","keyId":"model-key","publicKeyPath":"provider/authority-public.pem","publicKeySha256":"sha256:"+hashlib.sha256(authpub.read_bytes()).hexdigest(),"acceptedModelFamilies":["QwenFixture"],"protectionEpochs":["epoch-1"]}}
for name,data in provider_keys.items():
 provider_dir=run_root/data['dir']
 (provider_dir/'trust-root-registry-v1.json').write_text(json.dumps(registry)); shutil.copy2(authpub,provider_dir/'authority-public.pem')
 (provider_dir/'recipient-private.pem').write_bytes(data['recipient_private'].read_bytes()); (provider_dir/'recipient-private.pem').chmod(0o600)
 (provider_dir/'recipient-key-map.json').write_text(json.dumps({name:str(provider_dir/'recipient-private.pem')}))
# authority config
recipient_public_key_files={name:'../'+data['dir']+'/public.pem' for name,data in provider_keys.items()}
ac={"schema":"ndnsf-di-native-authority-v1","run_for_ms":120000,"permission_bootstrap_ms":30000,"max_grant_ttl_ms":60000,"authority":{"identity":"/example/hello/authority","service":"/HELLO","group":"/example/hello/group","controller_identity":"/example/hello/controller","requester_identity":"/example/hello/user","protection_epoch":"epoch-1","content_key_id":"model-key","trust_schema_file":"trust-schema.conf","authority_private_key_file":"private.pem","requester_public_key_file":"../requester/public.pem","content_key_file":"content-key.bin","allowed_model_manifests":[package],"recipient_public_key_files":recipient_public_key_files,"publication_sources":{package:{"model_name":model["model_name"],"model_content_digest":model["content_digest"],"canonical_source_digest":src_digest,"artifact_profile_digest":profile}}}}
if args.stored_model_reuse:
 ac['run_for_ms']=180000
(run_root/'authority/authority.json').write_text(json.dumps(ac))
# requester files
(run_root/'requester/requester-private.pem').write_bytes(reqpriv.read_bytes());(run_root/'requester/requester-private.pem').chmod(0o600); shutil.copy2(authpub,run_root/'requester/authority-public.pem'); (run_root/'requester/trust-schema.conf').write_text(fixture_schema); (run_root/'requester/model.onnx').write_bytes(src); (run_root/'requester/catalog.json').write_text(json.dumps(catalog));
# Cert name query from the provider PIB lets the requester validate the
# signed offer without sharing the provider's private NDN key.
from ndn.encoding import Name,Component
offer_entries=[]; public_key_files={}
for index,name in enumerate(provider_names):
 data=provider_keys[name]
 with sqlite3.connect(run_root/(data['dir']+'-store')/'pib/pib.db') as db: kb=db.execute("select key_name from keys").fetchall()
 keyprefix=None
 for row in kb:
  parts,_=Name.decode(bytes(row[0])); candidate='/'+'/'.join(Component.to_str(x.tobytes()) for x in parts)
  if candidate.startswith(name+'/'):
   keyprefix=candidate; break
 if keyprefix is None: raise RuntimeError('provider identity key missing: '+name)
 cert=keyprefix+'/ID-CERT'
 offer_public=data['offer_public']; offer_key_id='sha256:'+hashlib.sha256(load_pem_public_key(offer_public.read_bytes(), backend=default_backend()).public_bytes(Encoding.Raw,PublicFormat.Raw)).hexdigest()
 offer_file='offer-public.pem' if len(provider_names)==1 else 'offer-%s-public.pem' % ('a' if index==0 else 'b')
 shutil.copy2(offer_public,run_root/'requester'/offer_file)
 public_key_files[offer_key_id]=offer_file
 offer_entries.append({"provider":name,"service":"/Inference/NativeStream","keyLocatorPrefix":keyprefix,"signerKeyId":offer_key_id,"certificateName":cert})
# offer policy
op={"schema":"spec180-provider-offer-trust-v1","candidateId":"b2","candidateDigest":offer_policy,"trustSchema":"/example/hello/trust","entries":offer_entries}
# requester config
rc={"schema":"ndnsf-di-native-requester-v1","catalog":dict(catalog,source=dict(catalog['source'],file='model.onnx')),"core":{"requester_identity":"/example/hello/user","authority_identity":"/example/hello/controller","group":"/example/hello/group","trust_schema_file":"trust-schema.conf"},"grant":{"authority_identity":"/example/hello/authority","authority_service":"/HELLO","authority_public_key_file":"authority-public.pem","requester_private_key_file":"requester-private.pem","protection_epoch":"epoch-1"},"offer_admission":{"policy":op,"public_key_files":public_key_files,"candidate_digest":offer_policy},"limits":{"bootstrap_ms":30000,"max_source_bytes":1000000,"max_assembled_bytes":1000000},"request":{"service":"/Inference/NativeStream","task":"task","adapter_composition_digest":adapter_digest,"task_descriptor_digest":dg('task'),"generation_mode":"TOKEN_STREAMING","tokenizer_digest":"sha256:bf0f0fa65dc5aafe690ee497b4c8e2abe408fb4788c5ef035964dc94f15ca5a6","input_layout_digest":dg('input-layout'),"security_policy_digest":dg('security'),"max_candidates":1,"max_policy_ms":1000,"provider_names":provider_names,"max_reentries":1,"no_progress_ms":5000,"timeout_ms":30000,"ack_timeout_ms":5000}}
if args.replacement:
 rc['request']['allow_replacement']=True; rc['request']['max_replacements']=1
options={"useCache":True,"outputMode":"TOKEN_STREAMING","generationId":"0123456789abcdef0123456789abcdef","maxNewTokens":8,"tokenizerDigest":"sha256:bf0f0fa65dc5aafe690ee497b4c8e2abe408fb4788c5ef035964dc94f15ca5a6","eosTokenIds":[2],"sampling":{"mode":"Greedy","temperature":0.0,"topK":1,"topP":1.0,"repetitionPenalty":1.0,"seed":1750001},"stopStrings":[],"tokenInputName":"input_ids","stateInputNames":["attention_kv_in","recurrent_state_in","convolution_state_in"],"stateOutputNames":["attention_kv_out","recurrent_state_out","convolution_state_out"]}
(run_root/'requester/options.json').write_text(json.dumps(options)); rc['request']['options_file']='options.json'; rc['stream_oracle']={'token_ids':[4,5,6,7,8,9,10,2]}
if args.conversation or args.stored_model_reuse:
 conversation_key_dir=run_root/'requester/keys'; conversation_key_dir.mkdir(mode=0o700)
 conversation_key=(conversation_key_dir/'conversation.key'); conversation_key.write_bytes(os.urandom(32)); conversation_key.chmod(0o600)
 role_map_digest=dg(json.dumps([[stream_role,'/example/hello/provider']],separators=(',',':')))
 retention=int(time.time()*1000)+120000
 # The Provider receipt binds this field to the authenticated Selection
 # security-policy snapshot carried by the request contract.
 rc['conversation']={"schema":"ndnsf-di-native-conversation-v1","journal":{"state_root":"conversation-state","identity":"requester-a","keys":[{"id":"active","file":"keys/conversation.key"}],"quota_bytes":67108864,"test_only_allow_ephemeral_state_root":True},"owner":{"requester_identity":"/example/hello/user","service_name":"/Inference/NativeStream","security_domain_digest":dg('security')},"turn":{"conversation_id":"spec182-r11-b4-conversation","parent_context_epoch":0,"service_name":"/Inference/NativeStream","plan_role_map_digest":role_map_digest,"retention_deadline_ms":retention,"mode":"FULL_CONTEXT","generation_id":options['generationId'],"canonical_token_ids":[3],"expected_roles":[stream_role]},"checkpoint_output_file":"conversation-state.json"}
(run_root/'requester/config.json').write_text(json.dumps(rc))
repo_target={'repo_node':repo_node,'provider_identity':repo_provider,'service_prefix':repo_service}
if args.stored_model_reuse:
 preparer=copy.deepcopy(rc)
 preparer.pop('conversation',None)
 preparer.pop('stream_oracle',None)
 preparer['model_input']={'mode':'source' if args.request_time_model_preparation else 'publish-stored',
                         'repo_target':repo_target,
                         'preparation_state_file':'stored-preparation.json',
                         'private_handle_file':'stored-handle.json'}
 (run_root/'requester/prepare.json').write_text(json.dumps(preparer))
 requester_keys={}
 for name,identity in consumer_ids.items():
  directory=run_root/name
  private,public=edkey(0,directory)
  shutil.copy2(private,directory/'requester-private.pem')
  requester_keys[identity]='../'+name+'/public.pem'
  for filename in ('authority-public.pem','trust-schema.conf','root.cert')+tuple(public_key_files.values()):
   shutil.copy2(run_root/'requester'/filename,directory/filename)
  consumer=copy.deepcopy(rc)
  consumer['statistics_file']='statistics.jsonl'
  consumer['core']['requester_identity']=identity
  consumer['catalog']['source'].pop('file',None)
  consumer['model_input']={'mode':'stored','repo_target':repo_target,
                           'private_handle_file':'stored-handle.json'}
  consumer_options=dict(options,generationId=hashlib.sha256(name.encode()).hexdigest()[:32])
  (directory/'options.json').write_text(json.dumps(consumer_options))
  journal=consumer['conversation']
  journal['journal']['identity']=name
  journal['owner']['requester_identity']=identity
  journal['turn']['conversation_id']='spec222-stored-'+name
  journal['turn']['generation_id']=consumer_options['generationId']
  (directory/'keys').mkdir(mode=0o700)
  key=directory/'keys/conversation.key'
  key.write_bytes(os.urandom(32)); key.chmod(0o600)
  (directory/'config.json').write_text(json.dumps(consumer))
 ac['authority'].pop('requester_identity')
 ac['authority'].pop('requester_public_key_file')
 ac['authority']['requester_public_key_files']=requester_keys
 (run_root/'authority/authority.json').write_text(json.dumps(ac))
for dirname,_ in provider_dirs:
 (run_root/dirname/'plan.json').write_text(json.dumps(plan)); (run_root/dirname/'manifest.json').write_text(json.dumps(manifest))
# input bundle
payload=struct.pack('<q',3); outb=b'NDITB001'+struct.pack('<I',1)+struct.pack('<I',9)+b'input_ids'+struct.pack('<I',3)+struct.pack('<I',2)+struct.pack('<q',1)+struct.pack('<q',1)+struct.pack('<Q',len(payload))+payload;(run_root/'requester/input.bin').write_bytes(outb)
if args.stored_model_reuse:
 for name in consumer_ids:
  (run_root/name/'input.bin').write_bytes(outb)
# launch env
base=os.environ.copy(); base.update({'NDN_CLIENT_TRANSPORT':'unix://'+str(nfd_socket),'PATH':'/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin','NDNSF_CONTROLLER_GENERATION_STATE':str(run_root/'shared/controller-generation.state'),'NDN_LOG':'ndn_service_framework.*=TRACE:ndnsf.di.NativeRequester=WARN','NDNSF_DI_RUNTIME_TIMING':'1'})
# Qualification oracles consume the native evidence component explicitly;
# ordinary runtime logging remains controlled by its application configuration.
base['NDN_LOG']+=':ndnsf.di.RuntimeEvidence=WARN'
if args.stored_model_reuse:
 base['NDN_LOG']=base['NDN_LOG'].replace('ndn_service_framework.*=TRACE',
                                      'ndn_service_framework.*=WARN')
def launch(cmd,env,name):
 p=b.run(cmd,env,run_root/(name+'.log'),wait=False)
 (run_root/(name+'.pid')).write_text(str(p.pid)+'\n')
 starts[p.pid]=(name,time.monotonic())
 return p
starts={}
measurements={'scope':'native tiny causal-LM stored-model process qualification',
              'source_bytes':len(src),'phases':[],'first_failure':None,'forced_cleanup':[],
              'cleanup_errors':[],'live_processes':[]}
def wait_phase(process,timeout=60):
 name,started=starts[process.pid]
 try:
  result=process.wait(timeout=remaining_timeout(timeout))
 finally:
  measurements['phases'].append({'name':name,'wall_seconds':time.monotonic()-started,
                                 'cpu_seconds':None,'exit_code':process.poll()})
 if result != 0:
  raise RuntimeError(name+' exited '+str(result)+'; see '+str(run_root/(name+'.log')))
 return result
def refresh_consumer_retention(directory):
 path=directory/'config.json'
 config=json.loads(path.read_text())
 # Conversation grant expiry is the retention deadline, bounded by Authority policy.
 config['conversation']['turn']['retention_deadline_ms']=int(time.time()*1000)+ac['max_grant_ttl_ms']
 path.write_text(json.dumps(config))
children=[]
try:
 nfd=launch(['/usr/local/bin/nfd','--config',str(nfdconf)],base,'nfd');children.append(nfd)
 nfd_deadline=time.monotonic()+remaining_timeout(10)
 while not nfd_socket.exists():
  if nfd.poll() is not None or time.monotonic() >= nfd_deadline:
   raise RuntimeError('private NFD socket unavailable; see '+str(run_root/'nfd.log'))
  time.sleep(.05)
 run_command(['/usr/local/bin/nfdc','strategy','set','/example/hello/group','/localhost/nfd/strategy/multicast'],env=base,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 def penv(store):
  e=dict(base);e.update({'HOME':str(store/'home'),'NDN_CLIENT_PIB':'pib-sqlite3:'+str(store/'pib'),'NDN_CLIENT_TPM':'tpm-file:'+str(store/'tpm')});return e
 ensure_identities=','.join(['/example/hello/authority']+provider_names+['/example/hello/user']+extra_identities)
 c=launch([str(CONTROLLER),'--controller-prefix','/example/hello/controller','--policy-file',str(policy),'--trust-schema',str(run_root/'shared/trust-schema.conf'),'--ensure-identities',ensure_identities,'--no-serve-certificates','--run-for-ms','180000' if args.stored_model_reuse else '120000'],penv(run_root/'controller-store'),'controller');children.append(c); wait_ready(c,run_root/'controller.log','ServiceController started...',30)
 if args.stored_model_reuse:
  repo_env=penv(run_root/'repo-store')
  repo_env['NDNSF_DEPLOYMENT_BOOT_ID']='spec222-owned-repo'
  repo=launch([str(BUILD/'NDNSF-DistributedRepo/DistributedRepoServer'),
               '--repo-node',repo_node,'--provider-identity',repo_provider,
               '--database',str(run_root/'repo/store.sqlite'),'--repo-mode','persistent',
               '--group-prefix','/example/hello/group',
               '--authority-identity','/example/hello/controller',
               '--service-prefix',repo_service,
               '--trust-schema',str(run_root/'authority/trust-schema.conf')],repo_env,'repo')
  children.append(repo)
  wait_ready(repo,run_root/'repo.log','NDNSF_REPO_READY',60)
  prepare_env=penv(run_root/'requester-store')
  prepare_env['NDNSF_DI_WORKER_BINARY']=str(WORKER)
  preparer=launch([str(REQUESTER),'--config',str(run_root/'requester/prepare.json'),
                   '--prepare-only'],prepare_env,'preparer')
  children.append(preparer)
  wait_phase(preparer)
  print('preparer rc',preparer.returncode)
  handle=run_root/'requester/stored-handle.json'
  if args.request_time_model_preparation:
   original_handle=handle.read_bytes()
   (run_root/'requester/model.onnx').unlink()
   ready=launch([str(REQUESTER),'--config',str(run_root/'requester/prepare.json'),
                 '--prepare-only'],prepare_env,'preparer-ready')
   children.append(ready)
   wait_phase(ready)
   print('preparer-ready rc',ready.returncode)
   measurements['source_preparation']={'ready_exit_code':ready.returncode,
       'source_absent':not (run_root/'requester/model.onnx').exists(),
       'same_private_handle':handle.read_bytes()==original_handle}
  for name in consumer_ids:
   shutil.copy2(handle,run_root/name/'stored-handle.json')
   (run_root/name/'stored-handle.json').chmod(0o600)
  for filename in ('model.onnx','private.pem','requester-private.pem',
                   'stored-handle.json','stored-preparation.json'):
   (run_root/'requester'/filename).unlink(missing_ok=True)
  shutil.rmtree(run_root/'requester-store')
  shutil.rmtree(run_root/'bootstrap-store')
  restart_started=time.monotonic()
  restart_exits=[]
  measurements['infrastructure_restart']={'exits':restart_exits,'wall_seconds':None,
                                          'same_stores_and_credentials':True}
  for process in (repo,c,nfd):
   stopped_at=time.monotonic()
   status=b.stop(process,timeout=remaining_timeout(4))
   restart_exits.append({'name':starts[process.pid][0],'exit_code':status,
                         'stop_wall_seconds':time.monotonic()-stopped_at})
   measurements['infrastructure_restart']['wall_seconds']=time.monotonic()-restart_started
   if status != 0:
    raise RuntimeError('infrastructure restart did not drain '+starts[process.pid][0]+
                       ' exit='+str(status))
  state_path=run_root/'shared/controller-generation.state'
  shutil.copy2(state_path,run_root/'shared/controller-before-restart.state')
  (run_root/'shared/controller-before-restart.state').chmod(0o600)
  if nfd_socket.exists() or nfd_socket.is_symlink():
   if nfd_socket.is_symlink() or not nfd_socket.is_socket():
    raise RuntimeError('owned NFD restart path is not a socket')
   nfd_socket.unlink()
  nfd=launch(nfd.args,base,'nfd-restart');children.append(nfd)
  nfd_deadline=time.monotonic()+remaining_timeout(10)
  while not nfd_socket.exists():
   if nfd.poll() is not None or time.monotonic() >= nfd_deadline:
    raise RuntimeError('restarted private NFD socket unavailable')
   time.sleep(.05)
  run_command(['/usr/local/bin/nfdc','strategy','set','/example/hello/group',
               '/localhost/nfd/strategy/multicast'],env=base,check=True,
               stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  c=launch(c.args,penv(run_root/'controller-store'),'controller-restart');children.append(c)
  wait_ready(c,run_root/'controller-restart.log','ServiceController started...',30)
  repo=launch(repo.args,repo_env,'repo-restart');children.append(repo)
  wait_ready(repo,run_root/'repo-restart.log','NDNSF_REPO_READY',60)
  measurements['infrastructure_restart']['wall_seconds']=time.monotonic()-restart_started
  # With CS disabled and A gone, only the restored Repo route can serve A's Data.
  routes=run_command(['/usr/local/bin/nfdc','route','list'],env=base,check=True,
                        text=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
  (run_root/'routes-after-preparer.txt').write_text(routes.stdout)
  measurements['preparer_offline']=preparer.poll() == 0
  measurements['source_removed']=not (run_root/'requester/model.onnx').exists()
  measurements['credential_store_removed']=not (run_root/'requester-store').exists()
  measurements['fixture_setup_seconds']=time.monotonic()-driver_started
 a=launch([str(AUTHORITY),'--config',str(run_root/'authority/authority.json')],penv(run_root/'authority-store'),'authority');children.append(a); wait_ready(a,run_root/'authority.log','NATIVE_GRANT_AUTHORITY_READY',30)
 def provider_command(name):
  data=provider_keys[name]; provider_dir=run_root/data['dir']
  shutil.copy2(data['offer_private'],provider_dir/'offer-private.pem')
  return [str(PROVIDER),'--plan',str(provider_dir/'plan.json'),'--manifest',str(provider_dir/'manifest.json'),'--service','/Inference/NativeStream','--provider',name,'--group','/example/hello/group','--controller','/example/hello/controller','--trust-schema',str(provider_dir/'trust-schema.conf'),'--roles',stream_role,'--serve','--run-for-ms','60000','--artifact-cache-dir',str(provider_dir/'cache'),'--tokenizer-json',str(ROOT/'tests/fixtures/spec175/tiny-causal-lm-v1/standalone/tokenizer.json'),'--selection-offer-key-file',str(provider_dir/'offer-private.pem'),'--offer-backend','onnxruntime-cpu','--offer-can-provision','--offer-has-model']
 provider_processes={}
 provider_envs={}
 for index,name in enumerate(provider_names):
  data=provider_keys[name]; provider_dir=run_root/data['dir']; store=run_root/(data['dir']+'-store')
  pe=penv(store); pe['SPEC181_GRANT_AUTHORITY_PUBLIC_KEY']=str(provider_dir/'authority-public.pem'); pe['SPEC181_PROVIDER_RECIPIENT_KEY_MAP']=str(provider_dir/'recipient-key-map.json'); pe['NDNSF_DI_WORKER_BINARY']=str(WORKER)
  provider_log='provider' if index == 0 else 'provider-b'
  command=provider_command(name)
  if args.stored_model_reuse:
   command[command.index('--run-for-ms')+1]='180000'
  process=launch(command,pe,provider_log); children.append(process); wait_ready(process,run_root/(provider_log+'.log'),'NDNSF_DI_NATIVE_PROVIDER_READY',45)
  provider_processes[name]=process; provider_envs[name]=pe
 p=provider_processes[provider_a]; pe=provider_envs[provider_a]; pm=provider_command(provider_a)
 # requester direct first (and, when requested, a second process using the
 # native journal checkpoint produced by the first process).
 requester_dir=run_root/('consumer-b' if args.stored_model_reuse else 'requester')
 requester_store=run_root/('consumer-b-store' if args.stored_model_reuse else 'requester-store')
 if args.stored_model_reuse:
  refresh_consumer_retention(requester_dir)
 re=penv(requester_store); re.update({'NDNSF_DI_WORKER_BINARY':str(WORKER)})
 rq=launch([str(REQUESTER),'--config',str(requester_dir/'config.json'),'--input',str(requester_dir/'input.bin'),'--output',str(requester_dir/'output.bin')],re,'requester');children.append(rq)
 stopped_provider_a=False
 if args.replacement:
  # Wait until the requester has received A's authenticated ACK.  Pausing A
  # after ACK collection keeps it in the admitted candidate set while making
  # the first Selection/stream attempt time out; B must satisfy recovery.
  wait_ready(rq,run_root/'requester.log','providerName='+provider_a+' status=true',30)
  os.kill(p.pid,signal.SIGSTOP); stopped_provider_a=True
 if args.stored_model_reuse:
  wait_phase(rq)
 else:
  rq.wait()
 print('requester rc',rq.returncode)
 if args.conversation and (run_root/'requester/conversation-state.json').is_file():
  state_path=run_root/'requester/conversation-state.json'
  if args.recovery:
   p.kill(); stopped=p.wait(); children.remove(p); print('provider first rc',stopped)
   p=launch(pm,pe,'provider-restart'); children.append(p)
   wait_ready(p,run_root/'provider-restart.log','NDNSF_DI_NATIVE_PROVIDER_READY',45)
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
 elif args.conversation:
  # Leave the missing checkpoint for the C++ oracle to report from the
  # retained requester log; this driver only controls process lifecycle.
  print('conversation checkpoint handoff unavailable')
 if args.stored_model_reuse:
  directory=run_root/'consumer-c'
  refresh_consumer_retention(directory)
  env=penv(run_root/'consumer-c-store'); env['NDNSF_DI_WORKER_BINARY']=str(WORKER)
  consumer=launch([str(REQUESTER),'--config',str(directory/'config.json'),
                   '--input',str(directory/'input.bin'),'--output',str(directory/'output.bin')],
                  env,'requester-c')
  children.append(consumer)
  wait_phase(consumer)
  print('requester-c rc',consumer.returncode)
 print('ROOT',run_root)
 # The Python process driver deliberately stops at lifecycle orchestration.
 # The C++ parent selector reads these retained logs and owns every business
 # oracle for stream, conversation, recovery and replacement outcomes.
except BaseException as error:
 measurements['first_failure']=str(error)
 raise
finally:
 if 'stopped_provider_a' in locals() and stopped_provider_a and p.poll() is None:
  os.kill(p.pid,signal.SIGCONT)
 active_failure=sys.exc_info()[0] is not None
 for p in reversed(children):
  try:
   b.stop(p)
  except Exception as error:
   if not args.stored_model_reuse:
    raise
   measurements['cleanup_errors'].append(starts[p.pid][0]+': '+str(error))
  finally:
   p._ndnsf_log_handle.close()
  if args.stored_model_reuse and p.poll() == -signal.SIGKILL:
   measurements['forced_cleanup'].append(starts[p.pid][0])
 if args.stored_model_reuse:
  measurements['live_processes']=[p.pid for p in children if p.poll() is None]
  measurements['total_wall_seconds']=time.monotonic()-driver_started
  cleanup_failed=(measurements['forced_cleanup'] or measurements['cleanup_errors']
                  or measurements['live_processes'])
  if cleanup_failed and measurements['first_failure'] is None:
   measurements['first_failure']='owned process cleanup failed'
  (run_root/'stored-process-measurements.json').write_text(json.dumps(measurements,indent=2)+'\n')
  if not active_failure and cleanup_failed:
   raise RuntimeError('stored process cleanup was not clean; see measurements')
