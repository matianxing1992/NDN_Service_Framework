"""Run-specific authorization projection, not native prepare qualification."""
import copy
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from apps.yolo import configuration_for_run
from runtime.yolo_profile import ClosureError, application_sync_prefix


def inputs():
    roles = ['BackboneNeck', 'DetectShard0', 'DetectShard1', 'Merge']
    plan = dict(schema='tiger-yolo-run-plan-v1', namespace='/new/run',
                identities={role: '/new/run/' + role for role in roles + ['controller', 'user', 'repo']})
    template = {'services': [{'name': '/YOLO', 'roles': roles,
        'dependencies': [{'producers': ['BackboneNeck'], 'consumers': ['DetectShard0'], 'tensors': ['x']}],
        'model': '/Model/YOLO', 'model_family': 'YOLO26n',
        'users': ['/old/user'], 'providers': [{'identity': '/old/provider', 'roles': roles}]}],
        'runtime': {}, 'trust': {'app_roots': ['/old']}, 'authorization_summary': {'old': 'unused'}}
    return template, plan


def test_run_projection_preserves_model_graph_and_limits_capabilities():
    template, plan = inputs()
    original = copy.deepcopy(template)
    output = configuration_for_run(template, plan)
    assert template == original
    service = output['services'][0]
    assert service['dependencies'] == template['services'][0]['dependencies']
    assert service['model'] == '/Model/YOLO'
    assert service['users'] == ['/new/run/user']
    assert {p['identity']: p['roles'] for p in service['providers']} == {
        '/new/run/' + r: [r] for r in template['services'][0]['roles']}
    assert output['controller'] == '/new/run/controller'
    assert output['runtime']['provider_prefix'] == '/new/run'
    assert output['runtime']['application_name'] == '/new/run'
    assert output['group'] == '/new/run/sync'
    assert output['trust']['anchor_file'] == '/config/root.cert'
    assert output['trust']['app_roots'] == ['/new/run']
    assert 'authorization_summary' not in output


def test_sync_prefix_uses_application_name_not_provider_prefix():
    template, plan = inputs()
    plan['applicationName'] = '/new/run/yolo-app'
    output = configuration_for_run(template, plan)
    assert output['group'] == '/new/run/yolo-app/sync'
    assert output['runtime']['provider_prefix'] == '/new/run'
    assert output['runtime']['application_name'] == '/new/run/yolo-app'


@pytest.mark.parametrize('name', ['', 'relative/app', '/', '/new/run/app/',
                                  '/new/run//app', '/new/run/app with-space'])
def test_sync_prefix_rejects_noncanonical_application_name(name):
    with pytest.raises(ClosureError, match='APPLICATION_NAME'):
        application_sync_prefix(name)


def test_sync_prefix_appends_to_existing_absolute_application_name():
    assert application_sync_prefix('/new/run/yolo-app') == '/new/run/yolo-app/sync'


@pytest.mark.parametrize('fault', ['missing-role', 'foreign-identity', 'local-model', 'extra-service'])
def test_projection_rejects_unqualified_shape(fault):
    template, plan = inputs()
    if fault == 'missing-role':
        template['services'][0]['roles'].remove('Merge')
    elif fault == 'foreign-identity':
        plan['identities']['Merge'] = '/outside/Merge'
    elif fault == 'local-model':
        template['services'][0]['artifacts'] = [{'path': '/host/model.onnx'}]
    else:
        template['services'].append({'name': '/Other', 'roles': []})
    with pytest.raises(ValueError):
        configuration_for_run(template, plan)
