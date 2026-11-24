#!/bin/sh
# Build a fresh Spec 242/243 deployment root and add everything the continuity driver requires.
# Usage: prepare-continuity-root.sh <root> <model-source> <oracle-model>
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
FIXTURE="$HERE/../../fixtures/spec242"
ROOT=${1:?usage: prepare-continuity-root.sh NEW_ABSOLUTE_ROOT [MODEL ORACLE_MODEL [PREFIX]]}
MODEL=${2:-"$FIXTURE/four-layer-conversation.onnx"}
ORACLE_MODEL=${3:-"$FIXTURE/four-layer-conversation.onnx"}
PREFIX=${4:-/usr/local}
case "$ROOT" in /*) ;; *) echo "use an absolute fixture root" >&2; exit 1 ;; esac
if [ -e "$ROOT" ] || [ -L "$ROOT" ]; then
  echo "fixture root already exists: $ROOT" >&2
  exit 1
fi
python3 "$FIXTURE/prepare-deployment.py" \
  --output "$ROOT" --prefix "$PREFIX" \
  --release-manifest "$PREFIX/share/ndnsf/stack-install-manifest.json" \
  --journal-root "$ROOT/journal" --scenario hybrid >/dev/null
mkdir -m 700 "$ROOT/recovery"
python3 - "$ROOT" "$MODEL" "$ORACLE_MODEL" <<'PY'
import json, shutil, sys
from pathlib import Path
root, model, oracle_model = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
base = json.loads((root / 'deployment.json').read_text())
def write(name, doc):
    (root / name).write_text(json.dumps(doc, indent=2))
stored = json.loads(json.dumps(base))
stored['requester']['request']['policy_revalidation_period_ms'] = 2000
stored['operation'] = 'run'
stored['requester']['model_input'] = {'mode': 'stored',
    'private_handle_file': str(root / 'model-handle.json'),
    'repo_target': base['requester']['model_input']['repo_target']}
stored['requester']['conversation'] = {
    'schema': 'ndnsf-di-native-conversation-v1',
    'owner': {'requester_identity':
                  next(a['identity'] for a in base['actors'] if a['kind'] == 'requester'),
              'service_name': base['network']['service']},
    'journal': {'state_root': str(root / 'conversation-journal'),
                'identity': 'conversation',
                'keys': [{'id': 'conversation', 'file': str(root / 'conversation-journal.key')}]},
    'turn': {'mode': 'FULL_CONTEXT'}}
stored['security']['controller_recovery_checkpoint'] = str(root / 'recovery/controller.checkpoint')
stored['requester']['catalog']['source'].pop('file', None)
stored['requester']['catalog']['source'].pop('initializer_file', None)
requester = next(a for a in stored['actors'] if a['kind'] == 'requester')
# Both requester and Repo hold this permission in the generated policy. The DI
# stored-model read does not query the user's quota usage.
service = stored['requester']['model_input']['repo_target']['service_prefix'] + '/USER_STORAGE_USAGE'
for actor in stored['actors']:
    if actor['kind'] == 'controller':
        actor.setdefault('parameters', {}).update({
            'revoke-trigger-file': str(root / 'revocation.trigger'),
            'revoke-kind': 'service', 'revoke-identity': requester['identity'],
            'revoke-service': service, 'revoke-attribute': '/PERMISSION' + service})
import secrets
key_file = root / 'conversation-journal.key'
key_file.write_bytes(secrets.token_bytes(32))
key_file.chmod(0o600)
private = root / 'model-source.private'
reference = root / 'model-source.oracle'
shutil.copyfile(model, private)
shutil.copyfile(oracle_model, reference)
prepare = json.loads(json.dumps(stored))
prepare['operation'] = 'prepare'
prepare['requester']['model_input'] = {'mode': 'publish-stored',
    'preparation_state_file': str(root / 'prepare-state.json'),
    'private_handle_file': str(root / 'model-handle.json'),
    'repo_target': base['requester']['model_input']['repo_target']}
prepare['requester']['catalog']['source']['file'] = str(private)
prepare['security']['controller_recovery_bootstrap'] = True
write('stored.json', stored)
write('prepare.json', prepare)
# The oracle compares this descriptor against prepare.requester.catalog.source with only the file
# keys removed, so copy that block instead of inventing fields.
source = dict(prepare['requester']['catalog']['source'])
source.pop('initializer_file', None)
source['file'] = str(reference)
write('oracle-source.json', {'schema': 'ndnsf-di-model-reference-source-v1', 'source': source})
print('root prepared:', root)
PY
python3 "$HERE/three-rank-continuity.py" "$ROOT" --check-fixture
