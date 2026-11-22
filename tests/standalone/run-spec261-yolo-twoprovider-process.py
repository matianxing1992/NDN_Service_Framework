#!/usr/bin/env python3
"""Run a protected ONNX source split across two real Providers.

The driver owns only process lifecycle and private NFD/PIB/TPM setup.  The
request, grant verification, provider execution, response decoding, and
numerical oracle remain in the C++ production executables.
"""

import argparse
import ast
import math
import signal
import sys, json
import hashlib, os, shutil, subprocess, tempfile, time, sqlite3, struct
import resource
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
parser.add_argument('--initializer-sha256',
                    help='explicit sha256: digest of a prepared initializer object')
parser.add_argument('--frozen-fixtures', action='store_true',
                    help='use the tracked fixed input and independent detection reference')
parser.add_argument('--mode', choices=['component', 'hybrid'], default='hybrid')
parser.add_argument('--segment-manifest-dependency',
                    help='try manifest authentication on this stateless hybrid dependency')
parser.add_argument('--variant', choices=['26n','11n','ffn','attention-ffn','wan'], default='26n')
parser.add_argument('--whole-model-tp', action='store_true',
                    help='use the two-rank whole-model source plan for YOLO26n')
parser.add_argument('--matched-tp', action='store_true',
                    help='run full, cold TP and warm TP requests on one fixed case')
parser.add_argument('--tp-cancel', action='store_true',
                    help='cancel with the first YOLO Rank1 contribution withheld, then recover')
parser.add_argument('--wan-reference', type=Path, help='frozen input.bundle and mixed oracle.json')
parser.add_argument('--boundary', default='onnx-node-3')
parser.add_argument('--case', action='append', default=[],
                    help='case directory name under the model assets (repeatable)')
parser.add_argument('--no-local-nfd', action='store_true',
                    help='use external per-actor transports instead of a private NFD')
parser.add_argument('--restart-provider2', action='store_true',
                    help='stop and restart the second Provider before a second request')
parser.add_argument('--revoke', action='store_true',
                    help='revoke the Provider identity from the real Controller before the request')
parser.add_argument('--ffn-cancel', action='store_true',
                    help='withhold the first Rank1 partial and cancel the in-flight FFN request')
args=parser.parse_args()
if args.frozen_fixtures and args.case:
 parser.error('--frozen-fixtures selects the single tracked case')
if args.whole_model_tp and args.variant != '26n':
 parser.error('--whole-model-tp requires --variant 26n')
if args.matched_tp and (not args.whole_model_tp or len(args.case) != 1):
 parser.error('--matched-tp requires --whole-model-tp and one --case')
if args.tp_cancel and (not args.whole_model_tp or len(args.case) != 1 or args.matched_tp):
 parser.error('--tp-cancel requires --whole-model-tp and one --case without --matched-tp')
if args.mode == 'component' and (args.whole_model_tp or args.variant not in ('26n', '11n')):
 parser.error('--mode component requires YOLO without --whole-model-tp')
if args.segment_manifest_dependency and (args.mode != 'hybrid' or args.whole_model_tp or
                                        args.variant not in ('26n', '11n')):
 parser.error('--segment-manifest-dependency requires a stateless YOLO hybrid fixture')
if args.variant not in ('26n', '11n') and (args.frozen_fixtures or args.initializer_sha256):
 parser.error('--frozen-fixtures and --initializer-sha256 require a YOLO variant')
BUILD=args.build.resolve()
CONTROLLER = b.native_program(BUILD, 'App_ServiceController')
AUTHORITY = b.native_program(BUILD, 'DI_NativeArtifactAuthority')
PROVIDER=b.native_program(BUILD, 'di-native-provider'); REQUESTER=b.native_program(BUILD, 'DI_NativeRequester'); WORKER=b.native_program(BUILD, "DI_NativeOnnxAssemblyWorker", source_subdir="", installed_subdir="libexec/ndnsf-di")
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
wan = variant == 'wan'
blocks = variant == 'attention-ffn' or wan
ffn = variant == 'ffn' or blocks
if wan != (args.wan_reference is not None):
 parser.error('--wan-reference is required only for Wan')
if wan and args.ffn_cancel:
 parser.error('use the bounded FFN fixture for fault injection')
if args.ffn_cancel and not ffn:
 parser.error('--ffn-cancel requires the FFN fixture')
if ffn and (args.cache is None or args.case or args.revoke):
 parser.error('the FFN variant needs --cache with the generated fixture')
subject = ({'name': 'Spec263Blocks' if blocks else 'Spec263Ffn', 'output': 'y', 'max_nodes': 256 if blocks else 32, 'initializer': ''}
           if ffn else YOLO[variant])
if wan:
 subject.update(name='Wan21T2V13BDenoiser', output='flow', max_nodes=4096)
cache = (args.cache or subject['dir']).resolve()
reference = args.wan_reference.resolve() if wan else cache
model_path = cache / 'model.onnx'
if not model_path.is_file() and not ffn:
 model_path = cache / f'yolo{variant}.onnx'
src = model_path.read_bytes()
weights_path = cache / 'model.weights' if (cache / 'model.weights').exists() else cache / f'yolo{variant}.weights'
weights = None if ffn else weights_path.read_bytes()
initializer_digest = args.initializer_sha256 or subject['initializer']
if weights is not None and initializer_digest != 'sha256:' + hashlib.sha256(weights).hexdigest():
 parser.error('initializer object does not match the expected SHA-256')

def read_frozen_float32(path, shape):
 with path.open('rb') as stream:
  if stream.read(8) != b'\x93NUMPY\x01\x00':
   raise ValueError(f'unexpected NumPy version: {path}')
  header_size = struct.unpack('<H', stream.read(2))[0]
  if header_size > 4096:
   raise ValueError(f'unexpected NumPy header size: {path}')
  header = ast.literal_eval(stream.read(header_size).decode('ascii').strip())
  if header != {'descr': '<f4', 'fortran_order': False, 'shape': shape}:
   raise ValueError(f'unexpected NumPy tensor layout: {path}')
  payload = stream.read()
 if len(payload) != 4 * math.prod(shape):
  raise ValueError(f'unexpected NumPy tensor size: {path}')
 return payload

if args.frozen_fixtures:
 fixtures = ROOT / 'tests/fixtures/spec261' / ('yolo' + variant)
 input_raw = read_frozen_float32(fixtures / 'fixed-input.npy', (1, 3, 640, 640))
 predictions = read_frozen_float32(fixtures / 'oracle-predictions.npy', (1, 300, 6))
 oracle_values = [value for (value,) in struct.iter_unpack('<f', predictions)]
else:
 input_raw = (reference / ('input.bundle' if blocks else 'input.f32')).read_bytes()
 oracle_values = json.loads((reference / 'oracle.json').read_text())
src_digest='sha256:'+hashlib.sha256(src).hexdigest()
ad=adapter_descriptor('spec261','1',dg('fixture-adapter-state'),'fixture-abi-v1',('onnx',),('task',),('onnxruntime',),('float32',),dg('fixture-input-schema'),dg('fixture-options-schema'),dg('fixture-result-schema'),dg('fixture-graph-schema'),dg('fixture-split-schema'),dg('fixture-state-schema'),True,True,True)
model=model_descriptor(subject['name'],dg(subject['name']+'-content'),dg(subject['name']+'-semantics'),subject.get('planning',dg('fixture-placeholder')),'onnx','float32',ad)
if wan:
 exported = json.loads((cache / 'export.json').read_text())
 weights_path = cache / 'model.onnx.data'
 sidecar_digest = 'sha256:' + exported['sidecarSha256']
 if src_digest != 'sha256:' + exported['graphSha256'] or \
     weights_path.stat().st_size != exported['sidecarBytes'] or \
     exported['operators'].get('MatMulNBits') != 292 or \
     exported['blockRange'] != [27, 29] or exported['contextTokens'] != 12:
  raise ValueError('Wan source export identity differs')
 model['content_digest'] = src_digest
 model['quantization_subtype'] = 'weight_only_int4'
 model['source_revision'] = 'b7f0d36dfbc20355d3184ad63c709d888cdb640f'
 initializer_digest = sidecar_digest
if ffn:
 inspect = b.native_program(BUILD, 'native-source-inspect', source_subdir='',
                            installed_subdir='libexec/ndnsf-tests')
 with tempfile.TemporaryDirectory(prefix='spec263-inspect-') as workspace:
  descriptor = Path(workspace) / 'descriptor.json'
  descriptor.write_bytes(canonical_bytes({'model': model}))
  facts = json.loads(subprocess.check_output(
   [str(inspect), '--planning', str(descriptor), str(model_path)] +
   ([str(weights_path), sidecar_digest] if wan else [])))
 model['graph_digest'] = facts['planning_graph_digest']
 subject['canonical'] = facts['canonical_graph_digest']
adapter_digest=digest(ad)
package=dg(subject['name']+'-source-package') if ffn else dg('yolo-source-package')
if wan:
 package = dg(src_digest + sidecar_digest)
profile=dg(subject['name']+'-artifact-profile') if ffn else dg('yolo-artifact-profile')
asm=dg('yolo-assembler'); offer_policy=dg('yolo-offer-policy')
source = {'data_name': '/catalog/source', 'digest': src_digest,
          'model_manifest_digest': package, 'canonical_graph_digest': subject['canonical']}
provider_roles = {
 'provider': ['Prefix', 'Rank0'] if args.mode == 'hybrid' else ['Prefix'],
 'provider2': ['Rank1', 'Suffix'] if args.mode == 'hybrid' else ['Suffix'],
}
roles = [role for assigned in provider_roles.values() for role in assigned]
splitter = {'kind': 'ONNX_SOURCE', 'selection_mode': args.mode, 'roles': roles,
            'input_ingress_role': 'Prefix', 'result_egress_role': 'Suffix',
            'postprocessing': {}}
if wan:
 source.update(bytes=len(src), initializer_file='model.onnx.data',
               initializer_digest=sidecar_digest, initializer_bytes=weights_path.stat().st_size)
if blocks:
 splitter.update(json.loads((cache / 'source-anchors.json').read_text()))
elif ffn:
 splitter['ffn_windows'] = [
  {'up_node': 'onnx-node-1', 'down_node': 'onnx-node-4'},
  {'up_node': 'onnx-node-5', 'down_node': 'onnx-node-7'},
 ]
else:
 source.update(initializer_file='model.weights', initializer_digest=initializer_digest)
 splitter['boundary_node'] = args.boundary
 if args.whole_model_tp:
  splitter.pop('boundary_node')
  splitter.update(selection_mode='tensor_parallel', roles=['Rank0', 'Rank1'],
                  input_ingress_role='Rank0', result_egress_role='Rank0')
 elif args.mode == 'hybrid':
  splitter.update(input_channels=16, conv_input_axis='input-channel',
   activation_shape=[1,16,320,320], partial_shape=[1,32,160,160], activation_dtype='float32',
   conv_input_tensor=subject['conv_input'], conv_weight_initializer=subject['conv_weight'],
   conv_bias_initializer=subject['conv_bias'], conv_output_tensor=subject['conv_output'],
   conv_weight_bytes=18432, conv_bias_bytes=128, conv_pads=[1,1,1,1],
   conv_strides=[2,2], conv_dilations=[1,1])
catalog={"schema":"ndnsf-di-native-request-catalog-v1","model":json.loads(canonical_bytes(model).decode()),"source":source,"recipe":{"artifact_profile_digest":profile,"assembler_descriptor_digest":asm,"backend_abi":"onnxruntime-cpu-v1","precision":"float32","quantization":"none","layout":"native","padding":"none","protection_epoch":"epoch-1","max_source_bytes":67108864,"max_assembled_bytes":134217728,"max_nodes":subject['max_nodes']},"publication":{"artifact_root":"/Model/"+subject['name']+"/artifacts","package_manifest_digest":package},"input_format":"OPAQUE","max_payload_bytes":8388608,"splitter":splitter,"node_mapping":{},"state_inputs":{},"state_outputs":{}}
if wan:
 wan_source_bytes = 1792 << 20
 # The materializer charges the fetched closure plus two copies of the
 # serialized graph. Wan stores large tensors externally and scales inline.
 wan_assembled_bytes = wan_source_bytes + (256 << 20)
 # Each Provider can assemble both of its roles at once. Allow two cache
 # reservations, including their 64 KiB metadata allowances, and keep room
 # for the preceding uncut artifact in the same Provider.
 wan_cache_bytes = 2 * (wan_source_bytes + wan_assembled_bytes + 65536) + wan_assembled_bytes
 wan_cache_mib = (wan_cache_bytes + (1 << 20) - 1) >> 20
 catalog["recipe"].update(quantization="weight_only_int4", max_source_bytes=wan_source_bytes,
                           max_assembled_bytes=wan_assembled_bytes)
 catalog["publication"]["max_publication_bytes"] = 2 << 30
plan={"version":2,"services":[{"service":"/Inference/NativeUnary","schemaVersion":2,"model":"/Model/"+subject['name'],"modelFamily":"yolo","modelFormat":"onnx","plannerKind":"native-source-full","runtimeBackend":"onnxruntime","executionPolicy":"DATA_DRIVEN_V2","roles":roles,"dependencies":[],"planner":{"schemaVersion":2,"modelFamily":"yolo","modelFormat":"onnx","plannerKind":"native-source-full","runtimeBackend":"onnxruntime","executionPolicy":"DATA_DRIVEN_V2"}}]}
manifest={"services":[{"name":"/Inference/NativeUnary","model":"/Model/"+subject['name'],"roles":roles,"artifacts":[{"artifact":"/Artifact/YOLO/"+role,"backend":"onnxruntime-cpu","kind":"model","role":role} for role in roles]}]}
# provider plan and manifest files generated later

def policy_text():
 prefix_grants, suffix_grants = (
  "\n  ".join("/Inference/NativeUnary/ROLE/" + role for role in assigned)
  for assigned in provider_roles.values()
 )
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
  %s }
 }
 provider-policy
 {
  for /example/hello/provider2
  allow { /Inference/NativeUnary
  %s }
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
""" % (prefix_grants, suffix_grants)
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
registry={"schemaVersion":1,"status":"CONFIGURED","artifactPolicyAuthority":{"publicKeyAlgorithm":"ed25519","signatureAlgorithm":"ed25519","grantSchema":"ndnsf-di-key-grant-v1","authorityId":"/example/hello/authority","keyId":"model-key","publicKeyPath":"provider/authority-public.pem","publicKeySha256":"sha256:"+hashlib.sha256(authpub.read_bytes()).hexdigest(),"acceptedModelFamilies":[subject["name"]],"protectionEpochs":["epoch-1"]}}
(run_root/'provider/trust-root-registry-v1.json').write_text(json.dumps(registry));
registry2=json.loads(json.dumps(registry)); registry2['artifactPolicyAuthority']['publicKeyPath']='provider2/authority-public.pem'
(run_root/'provider2/trust-root-registry-v1.json').write_text(json.dumps(registry2)); shutil.copy2(authpub,run_root/'provider/authority-public.pem'); shutil.copy2(authpub,run_root/'provider2/authority-public.pem'); (run_root/'provider/recipient-private.pem').write_bytes(recippriv.read_bytes()); (run_root/'provider/recipient-private.pem').chmod(0o600); (run_root/'provider2/recipient-private.pem').write_bytes(recip2priv.read_bytes()); (run_root/'provider2/recipient-private.pem').chmod(0o600); (run_root/'provider/recipient-key-map.json').write_text(json.dumps({'/example/hello/provider':str(run_root/'provider/recipient-private.pem')})); (run_root/'provider2/recipient-key-map.json').write_text(json.dumps({'/example/hello/provider2':str(run_root/'provider2/recipient-private.pem')}))
# authority config
# Every case adds one request; keep the actor lifetimes above the whole
# session so a later case does not fail on an expired grant or fetch window.
# A case takes about 30 s on the high-latency topology, so budget a minute each
# plus the fixed startup allowance.
RUN_FOR_MS=1_800_000 if wan else 120000+60000*len(args.case)
ac={"schema":"ndnsf-di-native-authority-v1","run_for_ms":RUN_FOR_MS,"permission_bootstrap_ms":30000,"max_grant_ttl_ms":60000,"authority":{"identity":"/example/hello/authority","service":"/HELLO","group":"/example/hello/group","controller_identity":"/example/hello/controller","requester_identity":"/example/hello/user","protection_epoch":"epoch-1","content_key_id":"model-key","trust_schema_file":"trust-schema.conf","authority_private_key_file":"private.pem","requester_public_key_file":"../requester/public.pem","content_key_file":"content-key.bin","allowed_model_manifests":[package],"recipient_public_key_files":{"/example/hello/provider":"../provider/public.pem","/example/hello/provider2":"../provider2/public.pem"},"publication_sources":{package:{"model_name":model["model_name"],"model_content_digest":model["content_digest"],"canonical_source_digest":src_digest,"initializer_object_digest":initializer_digest,"artifact_profile_digest":profile}}}}
if wan:
 ac['max_grant_ttl_ms']=600000
if args.case:
 # A longer request window needs a matching grant policy ceiling.
 ac['max_grant_ttl_ms']=300000
(run_root/'authority/authority.json').write_text(json.dumps(ac))
# requester files
(run_root/'requester/requester-private.pem').write_bytes(reqpriv.read_bytes());(run_root/'requester/requester-private.pem').chmod(0o600); shutil.copy2(authpub,run_root/'requester/authority-public.pem'); shutil.copy2(offerpub,run_root/'requester/offer-public.pem'); shutil.copy2(offer2pub,run_root/'requester/offer2-public.pem'); (run_root/'requester/trust-schema.conf').write_text(fixture_schema); (run_root/'requester/model.onnx').write_bytes(src); (run_root/'requester/catalog.json').write_text(json.dumps(catalog));
if wan:
 # Keep the externally stored initializers immutable and range-backed.
 # The catalog's Repo owner opens the file at its resolved absolute path.
 source['initializer_file'] = str(weights_path)
elif weights is not None:
 (run_root/'requester/model.weights').write_bytes(weights)

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
# Exact TensorBundle framing is consumed by the native requester.
dims = [1, 3] if ffn else [1, 3, 640, 640]
input_name = b'x' if ffn else b'images'
hdr = b'NDITB001' + struct.pack('<II', 1, len(input_name)) + input_name
hdr += struct.pack('<II', 1, len(dims)) + struct.pack('<' + 'q' * len(dims), *dims)
hdr += struct.pack('<Q', len(input_raw))
(run_root/'requester/input.bin').write_bytes(input_raw if blocks else hdr+input_raw)
if args.case:
 rc['request']['timeout_ms']=180000
 rc['request']['no_progress_ms']=60000
if wan:
 # Native preparation reserves the graph, its separate initializer object,
 # the assembly ceiling and bounded catalog overhead before publication.
 rc['limits'].update(bootstrap_ms=540000, max_source_bytes=wan_source_bytes,
                     max_assembled_bytes=wan_assembled_bytes,
                     max_prepared_bytes=2 * wan_source_bytes + wan_assembled_bytes + (32 << 20))
 rc['repository'] = {'path': 'canonical-repo', 'max_bytes': 4 << 30}
 rc['encrypted_repository'] = {'path': 'encrypted-repo', 'max_bytes': 4 << 30}
 rc['request'].update(timeout_ms=540000, no_progress_ms=120000, max_policy_ms=5000)
 rc['oracle'] = oracle_values
elif blocks:
 rc['oracle'] = dict(oracle_values, rtol=0.0)
elif ffn:
 rc['oracle'] = oracle_values
else:
 rc['oracle']={'tensor':subject['output'],'float32':oracle_values,'tolerance':1e-4,
  'row_width':6,'detections':True,'coordinate_tolerance':0.01,'score_tolerance':1e-4,'iou_floor':0.999}
if args.segment_manifest_dependency:
 rc['request']['segment_manifest_dependency_id'] = args.segment_manifest_dependency
(run_root/'requester/config.json').write_text(json.dumps(rc))
# launch env
base=os.environ.copy(); base.update({'NDN_CLIENT_TRANSPORT':'unix://'+str(nfd_socket),'PATH':'/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin','NDNSF_CONTROLLER_GENERATION_STATE':str(run_root/'shared/controller-generation.state'),'NDNSF_DI_RUNTIME_TIMING':'1'})
base.setdefault('NDN_LOG', 'ndn_service_framework.*=TRACE:ndnsf.di.NativeRequester=WARN')
# The process oracle reads readiness and execution evidence from this logger.
base['NDN_LOG'] += ':ndnsf.di.RuntimeEvidence=WARN'
def wrap_for(role):
 pid=os.environ.get('SPEC261_NETNS_PID_'+role,'')
 if not pid: return []
 mnexec=shutil.which('mnexec', path=base['PATH'])
 if mnexec is None:
  raise RuntimeError('mnexec is required for the MiniNDN namespace')
 return [mnexec,'-a',pid]

def network_bytes():
 counters = {}
 namespaces = set()
 for role in ("requester", "provider", "provider2", "controller"):
  namespace = wrap_for(role)
  if not namespace or tuple(namespace) in namespaces:
   continue
  namespaces.add(tuple(namespace))
  raw = subprocess.check_output(namespace + ["cat", "/proc/net/dev"], text=True, timeout=5)
  totals = [0, 0]
  for line in raw.splitlines()[2:]:
   name, values = line.split(":", 1)
   if name.strip() != "lo":
    fields = values.split()
    totals[0] += int(fields[0])
    totals[1] += int(fields[8])
  counters[role] = totals
 return counters


def wait_measured_phase(process, actors, timeout):
 deadline = time.monotonic() + timeout
 peak_rss = {role: 0 for role in actors}
 peak_total = 0
 min_available = None
 while True:
  sampled = 0
  for role, actor in actors.items():
   try:
    fields = Path(f"/proc/{actor.pid}/status").read_text().splitlines()
   except FileNotFoundError:
    continue
   rss = next((int(line.split()[1]) for line in fields if line.startswith("VmRSS:")), 0)
   peak_rss[role] = max(peak_rss[role], rss)
   sampled += rss
  peak_total = max(peak_total, sampled)
  available = next(int(line.split()[1]) for line in Path("/proc/meminfo").read_text().splitlines()
                   if line.startswith("MemAvailable:"))
  min_available = available if min_available is None else min(min_available, available)
  remaining = deadline - time.monotonic()
  if remaining <= 0:
   raise subprocess.TimeoutExpired(process.args, timeout)
  try:
   process.wait(timeout=min(0.25, remaining))
   return {"actorPeakSampledRssKiB": peak_rss, "sampledActorPeakTotalRssKiB": peak_total,
           "minHostAvailableKiB": min_available}
  except subprocess.TimeoutExpired:
   pass


def launch(cmd,env,name,role=''):
 full=wrap_for(role)+[str(value) for value in cmd]
 p=b.run(full,env,run_root/(name+'.log'),wait=False)
 (run_root/(name+'.pid')).write_text(str(p.pid)+'\n')
 return p
children=[]
def transport_for(role):
 return os.environ.get('SPEC261_TRANSPORT_'+role,'')
try:
 if not args.no_local_nfd:
  nfd=launch(['/usr/local/bin/nfd','--config',str(nfdconf)],base,'nfd');children.append(nfd)
  nfd_deadline=time.monotonic()+30
  while not nfd_socket.exists():
   if time.monotonic()>nfd_deadline: raise RuntimeError('NFD socket did not appear within 30s')
   time.sleep(.05)
  subprocess.run(['/usr/local/bin/nfdc','strategy','set','/example/hello/group','/localhost/nfd/strategy/multicast'],env=base,check=True,stdout=subprocess.PIPE,stderr=subprocess.PIPE)
 def penv(store,role=''):
  env=b.actor_environment(base,store)
  transport=transport_for(role)
  if transport: env['NDN_CLIENT_TRANSPORT']=transport
  elif args.no_local_nfd:
   raise RuntimeError('SPEC261_TRANSPORT_MISSING: ' + (role or 'unnamed-actor'))
  return env
 controller_command=[str(CONTROLLER),'--controller-prefix','/example/hello/controller','--policy-file',str(policy),'--trust-schema',str(run_root/'shared/trust-schema.conf'),'--ensure-identities','/example/hello/authority,/example/hello/provider,/example/hello/provider2,/example/hello/user','--no-serve-certificates']
 revoke_after_ms=60000
 if args.revoke:
  controller_command += ['--revoke-after-ms',str(revoke_after_ms),'--revoke-kind','identity','--revoke-identity','/example/hello/provider']
 controller_command += ['--run-for-ms',str(RUN_FOR_MS)]
 controller_started_at=time.monotonic()
 c=launch(controller_command,penv(run_root/'controller-store','controller'),'controller','controller');children.append(c); b.wait_marker(c,run_root/'controller.log','ServiceController started...',30)
 a=launch([str(AUTHORITY),'--config',str(run_root/'authority/authority.json')],penv(run_root/'authority-store','authority'),'authority','authority');children.append(a); b.wait_marker(a,run_root/'authority.log','NATIVE_GRANT_AUTHORITY_READY',30)
 providers=[('/example/hello/'+tag, ','.join(provider_roles[tag]), tag, offerkey)
            for tag, offerkey in [('provider', offerpriv), ('provider2', offer2priv)]]
 def start_provider(identity,roles,tag,offerkey,*,fault=False):
  pe=penv(run_root/(tag+'-store'),tag)
  shutil.copy2(offerkey,run_root/(tag+'/offer-private.pem'))
  pm=[str(PROVIDER),'--plan',str(run_root/'provider/plan.json'),'--manifest',str(run_root/(tag+'/manifest.json')),'--service','/Inference/NativeUnary','--provider',identity,'--group','/example/hello/group','--controller','/example/hello/controller','--trust-schema',str(run_root/(tag+'/trust-schema.conf')),'--roles',roles,'--workers','4','--handler-threads','4','--serve','--run-for-ms',str(RUN_FOR_MS),'--artifact-cache-dir',str(run_root/(tag+'/cache')),'--selection-offer-key-file',str(run_root/(tag+'/offer-private.pem')),'--offer-backend','onnxruntime-cpu','--offer-can-provision','--offer-has-model']
  if args.frozen_fixtures:
   pm += ['--statistics-file', str(run_root/(tag+'-statistics.jsonl'))]
  if args.segment_manifest_dependency:
   pm.append('--enable-segment-manifest')
  if wan:
   pm += ['--artifact-cache-max-mib',str(wan_cache_mib),'--repo-fetch-timeout-ms','120000',
          '--assembly-timeout-ms','480000']
  if args.case:
   # Each authenticated request keeps one resident per role and each resident
   # holds its role template, so a multi-sample session needs a larger bounded
   # template cache than the default.
   pm += ['--artifact-cache-entries','64','--repo-fetch-timeout-ms','30000']
  log_tag = tag
  if fault:
   pm[0] = str(b.native_program(BUILD, 'di-native-fault-provider'))
   pm += ['--fault-type', 'late-old-output', '--fault-role', 'Rank1',
          '--fault-delay-ms', '4500' if args.tp_cancel else '2000']
   log_tag += '-fault'
  pe['SPEC181_GRANT_AUTHORITY_PUBLIC_KEY']=str(run_root/(tag+'/authority-public.pem'));pe['SPEC181_PROVIDER_RECIPIENT_KEY_MAP']=str(run_root/(tag+'/recipient-key-map.json'));pe['NDNSF_DI_WORKER_BINARY']=str(WORKER)
  ps=launch(pm,pe,log_tag,tag);children.append(ps); b.wait_marker(ps,run_root/(log_tag+'.log'),'NDNSF_DI_NATIVE_PROVIDER_READY',60)
  return ps
 running={}
 for identity,roles,tag,offerkey in providers:
  running[tag]=start_provider(identity,roles,tag,offerkey)
 re=penv(run_root/'requester-store','requester'); re.update({'NDNSF_DI_WORKER_BINARY':str(WORKER)})
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
 if ffn:
  requester_root = run_root / 'requester'
  phases = ['full', 'hybrid-cold', 'hybrid-warm']
  if args.ffn_cancel:
   phases += ['hybrid-cancel', 'hybrid-recovery']
  for phase in phases:
   if phase == 'hybrid-cancel':
    b.stop(running['provider2'])
    running['provider2'] = start_provider('/example/hello/provider2', 'Rank1,Suffix',
                                         'provider2', offer2priv, fault=True)
   cfg = json.loads(json.dumps(rc))
   if phase == 'full':
    cfg['catalog']['splitter'] = {
     'kind': 'ONNX_SOURCE', 'selection_mode': 'full', 'roles': ['Prefix'],
     'input_ingress_role': 'Prefix', 'result_egress_role': 'Prefix',
    }
    placements = {'Prefix': '/example/hello/provider'}
   else:
    placements = {'Prefix': '/example/hello/provider', 'Rank0': '/example/hello/provider',
                  'Rank1': '/example/hello/provider2', 'Suffix': '/example/hello/provider2'}
   cfg['fixed_placement'] = {'role_providers': placements}
   config_path = requester_root / ('config-' + phase + '.json')
   config_path.write_text(json.dumps(cfg))
   interfaces_before = network_bytes() if wan else {}
   before = resource.getrusage(resource.RUSAGE_CHILDREN)
   started = time.monotonic()
   process = launch([str(REQUESTER), '--config', str(config_path),
                     '--input', str(requester_root / 'input.bin'),
                     '--output', str(requester_root / ('output-' + phase + '.bin'))],
                    re, 'requester-' + phase, 'requester')
   children.append(process)
   if phase == 'hybrid-cancel':
    fault_log = run_root / 'provider2-fault.log'
    injected = 'NDNSF_DI_EXPERIMENT_FAULT_INJECTED type=late-old-output role=Rank1 session='
    b.wait_marker(running['provider2'], fault_log, injected, timeout=15)
    match = next(line for line in fault_log.read_text(errors='replace').splitlines()
                 if injected in line)
    session = match.split(injected, 1)[1].split()[0]
    deadline = time.monotonic() + 1
    marker = f'DEPENDENCY_FETCH status=begin requestId={session} '
    withheld = '/8=step/8=Rank1%2FRUN%2F2/' if blocks else '/8=step/8=Rank1%2FRUN%2F0/'
    while time.monotonic() < deadline and process.poll() is None:
     lines = (run_root / 'provider.log').read_text(errors='replace').splitlines()
     if any(marker in line and ' role=Rank0 ' in line and
            withheld in line for line in lines):
      break
     time.sleep(0.025)
    else:
     raise RuntimeError('Rank0 did not wait on the withheld Rank1 partial')
    process.send_signal(signal.SIGINT)
    (run_root / 'cancelled-request.txt').write_text(session + '\n')
   if wan:
    measurements = wait_measured_phase(process, {**running, "requester": process,
                                            "controller": c, "authority": a},
                                  (rc['limits']['bootstrap_ms'] +
                                   rc['request']['timeout_ms']) / 1000 + 30)
   else:
    process.wait(timeout=120)
   after = resource.getrusage(resource.RUSAGE_CHILDREN)
   receipt = {'phase': phase, 'exit': process.returncode,
              'requesterWallMs': round((time.monotonic() - started) * 1000, 3),
              'requesterCpuMs': round((after.ru_utime + after.ru_stime -
                                       before.ru_utime - before.ru_stime) * 1000, 3)}
   if wan:
    interfaces_after = network_bytes()
    measurements["nodeInterfaceBytes"] = {role: {
     "rx": counts[0] - interfaces_before[role][0],
     "tx": counts[1] - interfaces_before[role][1],
    } for role, counts in interfaces_after.items()}
    receipt.update(measurements)
   with (run_root / 'requester-trials.jsonl').open('a') as trials:
    trials.write(json.dumps(receipt) + '\n')
   print('requester phase', phase, 'rc', process.returncode)
   if phase == 'hybrid-cancel' and process.returncode != 1:
    raise RuntimeError('cancelled requester did not exit at its cancellation boundary')
   if phase != 'hybrid-cancel' and process.returncode != 0:
    raise SystemExit(process.returncode)
  raise SystemExit(0)
 # requester direct first
 if args.case:
  failures=[]
  for name in args.case:
   case_dir=subject['dir']/'cases'/name
   if not (case_dir/'input.f32').is_file() or not (case_dir/'oracle.json').is_file():
    raise RuntimeError('SPEC261_CASE_MISSING: '+str(case_dir))
   raw=(case_dir/'input.f32').read_bytes()
   values=json.loads((case_dir/'oracle.json').read_text())['float32']
   dims=[1,3,640,640]
   hdr=b'NDITB001'+struct.pack('<I',1)+struct.pack('<I',6)+b'images'+struct.pack('<I',1)+struct.pack('<I',len(dims))
   for d in dims: hdr+=struct.pack('<q',d)
   hdr+=struct.pack('<Q',len(raw))
   input_path=run_root/'requester'/('input-'+name+'.bin')
   input_path.write_bytes(hdr+raw)
   cfg=json.loads((run_root/'requester/config.json').read_text())
   cfg['oracle']={'tensor':subject['output'],'float32':values,'tolerance':1e-4,
     'row_width':6,'detections':True,'coordinate_tolerance':0.01,'score_tolerance':1e-4,'iou_floor':0.999}
   phases = [name + '-' + suffix for suffix in ('full', 'tp-cold', 'tp-warm')] \
    if args.matched_tp else [name]
   if args.tp_cancel:
    phases = [name + '-tp-cancel', name + '-tp-recovery']
   for phase in phases:
    cancelling = args.tp_cancel and phase.endswith('-tp-cancel')
    if cancelling:
     b.stop(running['provider2'])
     running['provider2'] = start_provider('/example/hello/provider2', 'Rank1,Suffix',
                                          'provider2', offer2priv, fault=True)
    elif args.tp_cancel:
     if b.stop(running['provider2'], timeout=15) != 0:
      raise RuntimeError('cancelled YOLO fault Provider did not exit cleanly')
     running['provider2'] = start_provider('/example/hello/provider2', 'Rank1,Suffix',
                                          'provider2', offer2priv)
    phase_config = json.loads(json.dumps(cfg))
    if args.matched_tp:
     if phase.endswith('-full'):
      phase_config['catalog']['splitter'] = {
       'kind': 'ONNX_SOURCE', 'selection_mode': 'full', 'roles': ['Prefix'],
       'input_ingress_role': 'Prefix', 'result_egress_role': 'Prefix',
      }
      placements = {'Prefix': '/example/hello/provider'}
     else:
      placements = {'Rank0': '/example/hello/provider', 'Rank1': '/example/hello/provider2'}
     phase_config['fixed_placement'] = {'role_providers': placements}
    config_path = run_root/'requester'/('config-'+phase+'.json')
    output_path = run_root/'requester'/('output-'+phase+'.bin')
    config_path.write_text(json.dumps(phase_config))
    if args.matched_tp:
     interfaces_before = network_bytes()
     before = resource.getrusage(resource.RUSAGE_CHILDREN)
     started = time.monotonic()
    rqc = launch([str(REQUESTER), '--config', str(config_path), '--input', str(input_path),
                  '--output', str(output_path)], re, 'requester-'+phase, 'requester')
    children.append(rqc)
    if cancelling:
     fault_log = run_root / 'provider2-fault.log'
     injected = 'NDNSF_DI_EXPERIMENT_FAULT_INJECTED type=late-old-output role=Rank1 session='
     b.wait_marker(rqc, fault_log, injected, timeout=150)
     session = next(line.split(injected, 1)[1].split()[0]
                    for line in fault_log.read_text(errors='replace').splitlines()
                    if injected in line)
     marker = f'DEPENDENCY_FETCH status=begin requestId={session} '
     withheld = '/8=step/8=Rank1%2Fcall%2F0/'
     deadline = time.monotonic() + 4
     while time.monotonic() < deadline and rqc.poll() is None:
      lines = (run_root / 'provider.log').read_text(errors='replace').splitlines()
      if any(marker in line and ' role=Rank0 ' in line and withheld in line for line in lines):
       break
      time.sleep(0.025)
     else:
      raise RuntimeError('Rank0 did not wait on the withheld YOLO contribution')
     rqc.send_signal(signal.SIGINT)
     (run_root / 'cancelled-request.txt').write_text(session + '\n')
    try:
     if args.matched_tp:
      usage = wait_measured_phase(rqc, {**running, 'requester': rqc,
                                        'controller': c, 'authority': a}, 180)
     else:
      rqc.wait(timeout=180)
    except subprocess.TimeoutExpired:
     b.stop(rqc)
     failures.append(phase+':timeout')
     break
    print('requester case', phase, 'rc', rqc.returncode)
    if args.matched_tp:
     after = resource.getrusage(resource.RUSAGE_CHILDREN)
     usage.update(phase=phase, exit=rqc.returncode,
                  requesterWallMs=round((time.monotonic() - started) * 1000, 3),
                  requesterCpuMs=round((after.ru_utime + after.ru_stime -
                                       before.ru_utime - before.ru_stime) * 1000, 3),
                  inputSha256=hashlib.sha256(input_path.read_bytes()).hexdigest())
     usage['nodeInterfaceBytes'] = {
      role: {'rx': counts[0] - interfaces_before[role][0],
             'tx': counts[1] - interfaces_before[role][1]}
      for role, counts in network_bytes().items()}
     if rqc.returncode == 0:
      usage['outputSha256'] = hashlib.sha256(output_path.read_bytes()).hexdigest()
     with (run_root/'requester-trials.jsonl').open('a') as trials:
      trials.write(json.dumps(usage) + '\n')
    if rqc.returncode != (1 if cancelling else 0):
     failures.append(phase)
     break
  print('ROOT',run_root)
  if failures:
   raise SystemExit(1)
  # Case mode already produced the evidence this invocation owns.
  raise SystemExit(0)
 request_started = time.monotonic()
 rq=launch([str(REQUESTER),'--config',str(run_root/'requester/config.json'),'--input',str(run_root/'requester/input.bin'),'--output',str(run_root/'requester/output.bin')],re,'requester','requester');children.append(rq)
 try:
  rq.wait(timeout=120)
 except subprocess.TimeoutExpired:
  b.stop(rq)
  raise RuntimeError('requester did not finish within 120s')
 print('requester rc',rq.returncode)
 if args.frozen_fixtures:
  (run_root/'request-timing.json').write_text(json.dumps({
   'boundary': 'requester-launch-to-process-exit', 'wallSeconds': time.monotonic() - request_started,
   'exitCode': rq.returncode,
  }))
 if rq.returncode != 0:
  raise SystemExit(rq.returncode)
 if args.restart_provider2:
  b.stop(running['provider2'])
  time.sleep(1.0)
  running['provider2']=start_provider(*providers[1])
  rq2=launch([str(REQUESTER),'--config',str(run_root/'requester/config.json'),'--input',str(run_root/'requester/input.bin'),'--output',str(run_root/'requester/output-restarted.bin')],re,'requester-restarted','requester');children.append(rq2)
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
 cleanup = []
 for p in reversed(children):
  exit_code = b.stop(p)
  cleanup.append({'pid': p.pid, 'exitCode': exit_code,
                  'forced': exit_code == -signal.SIGKILL, 'reaped': p.poll() is not None})
 if args.frozen_fixtures:
  (run_root/'actor-cleanup.json').write_text(json.dumps(cleanup))
 if ffn or args.whole_model_tp:
  exits = [{'pid': actor['pid'], 'exit': actor['exitCode']} for actor in cleanup]
  (run_root / 'process-cleanup.json').write_text(json.dumps(exits) + '\n')
