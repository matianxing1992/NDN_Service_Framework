"""Allowlisted evidence from actual V3 Selection wire; never persist that wire.

This projection is not authenticated execution evidence. The caller must bind
it to the sealed User request and separately verify native observations.
"""


def public_assignment_projection(wire, *, request_id, attempt, plan_digest, provider):
    """Decode the production contract and expose names, not grants or keys.

    Scope rules mirror NativeExecutionPlanJson::roleSpecFromSelectionProjectionV3;
    the session rule mirrors ExecutionAttemptKey::scopedSessionId. Input edges
    include APPLICATION_INPUT (empty producer), which must not be treated as a
    Provider-to-Provider pair by the collector.
    """
    from ndnsf_distributed_inference.sdk.placement import ProviderSelectionProjectionV3

    if not isinstance(wire, bytes) or type(attempt) is not int or not 0 < attempt < 2**64:
        raise ValueError('PUBLIC_ASSIGNMENT_INPUT')
    projection = ProviderSelectionProjectionV3.from_bytes(wire)
    if (projection.request_id != request_id or projection.attempt != attempt
            or projection.plan_digest != plan_digest or projection.provider != provider):
        raise ValueError('PUBLIC_ASSIGNMENT_BINDING')
    if not request_id.strip('/') or any(c.isspace() for c in request_id):
        raise ValueError('PUBLIC_ASSIGNMENT_SESSION')
    flow = projection.dataflow
    groups = {}
    for endpoint in flow.may_publish + flow.must_fetch:
        groups.setdefault(endpoint.group_id, set()).add((endpoint.producer_role, endpoint.tensor_id))
    redistribution_groups = {
        dependency['key_scope'] for dependency in projection.dependencies
        if dependency.get('redistributions')
    }

    def edge(endpoint):
        if endpoint.security_profile != 'NDNSF_DATA_V1':
            raise ValueError('PUBLIC_ASSIGNMENT_TRANSPORT')
        scope = endpoint.group_id
        if len(groups[scope]) > 1:
            scope += '/from/' + endpoint.producer_role.lstrip('/')
            if endpoint.group_id not in redistribution_groups:
                scope += '/tensor/' + endpoint.tensor_id
        # Explicit construction is intentional: do not use asdict/projection
        # serialization or copy arbitrary future fields into a public record.
        return dict(scope=scope, producer=endpoint.producer_role,
                    consumer=endpoint.consumer_role, planned_name=endpoint.name_prefix)

    return dict(schema='tiger-yolo-public-assignment-v1', requestId=request_id,
        attempt=attempt, planDigest=plan_digest,
        sessionId=request_id.strip('/') + '/attempt/' + str(attempt),
        provider=provider, role=flow.role,
        inputs=[edge(endpoint) for endpoint in flow.must_fetch],
        outputs=[edge(endpoint) for endpoint in flow.may_publish])
