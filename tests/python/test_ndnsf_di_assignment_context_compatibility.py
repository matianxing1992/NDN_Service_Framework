from __future__ import annotations

import unittest

from ndnsf_distributed_inference.core import AssignmentContext
from ndnsf_distributed_inference.deployment import (
    deployment_assignment_context, deployment_role_provider_preference,
    request_collaboration_with_deployment,
)


class User:
    def __init__(self): self.kwargs = None
    def get_ndnsd_services(self):
        return [{"serviceMetaInfo": {"deployments": '[{"deploymentId":"d","status":"ACTIVE","fragmentMap":{"prefill":[{"provider":"/a"}]}}]'}}]
    def request_collaboration(self, service, payload, **kwargs):
        self.kwargs = kwargs; return "ok"


class AssignmentContextCompatibilityTest(unittest.TestCase):
    def test_legacy_string_and_new_context_share_same_deployment_record(self):
        user = User()
        self.assertEqual(deployment_role_provider_preference(user, "d"), "prefill=>/a;")
        context = deployment_assignment_context(user, "d")
        self.assertIsInstance(context, AssignmentContext)
        self.assertEqual(context.providers_by_role(), {"prefill": "/a"})

    def test_legacy_helper_delegates_without_environment_bridge(self):
        user = User()
        self.assertEqual(request_collaboration_with_deployment(
            user, "/service", b"x", deployment_id="d"), "ok")
        self.assertEqual(user.kwargs["assignment_context"].providers_by_role(), {"prefill": "/a"})

    def test_missing_deployment_preserves_unassigned_legacy_call(self):
        user = User()
        request_collaboration_with_deployment(user, "/service", b"x", deployment_id="missing")
        self.assertNotIn("assignment_context", user.kwargs)


if __name__ == "__main__": unittest.main()
