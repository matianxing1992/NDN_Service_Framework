#!/usr/bin/env python3
"""Run the python-ndn stream test suite with an explicit result exit.

The native Face/keychain stack can start teardown threads during interpreter
finalization.  Exiting after unittest has flushed its result keeps the
MiniNDN launcher status tied to the assertions, while the test itself still
executes the real C++ StreamPublisher.push path.
"""

from __future__ import annotations

import os
import sys
import unittest

import test_ndnsf_python_ndn_stream_push as test_module


if __name__ == "__main__":
    suite = unittest.defaultTestLoader.loadTestsFromModule(test_module)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    sys.stdout.flush()
    sys.stderr.flush()
    os._exit(0 if result.wasSuccessful() else 1)
