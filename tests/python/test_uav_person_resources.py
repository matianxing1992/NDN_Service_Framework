"""Resource observer arithmetic/ownership fixtures, not native tracking or accuracy tests."""
from contextlib import nullcontext
import copy
import subprocess
import sys
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import psutil
from Experiments.UAV import person_run_resources as resources
from Experiments.UAV.person_run_receipt import validate_person_resources


class Process:
    def __init__(self, pid, rss, created=1):
        self.pid, self.rss, self.created = pid, rss, created
        self.children_list = []
        self.alive = True
        self.denied = False
    def create_time(self): return self.created
    def is_running(self): return self.alive
    def status(self): return psutil.STATUS_RUNNING
    def children(self, recursive=True): return self.children_list
    def oneshot(self): return nullcontext()
    def num_threads(self): return 2
    def cpu_affinity(self): return [0, 1]
    def memory_info(self):
        if self.denied: raise psutil.AccessDenied(self.pid)
        return SimpleNamespace(rss=self.rss)


class ResourceObserverTests(unittest.TestCase):
    def setUp(self):
        self.root = Process(10, 100)
        self.child = Process(20, 200)
        self.root.children_list = [self.child]
        lookup = {10: self.root, 20: self.child, None: Process(99, 50)}
        self.patch = patch.object(resources.psutil, 'Process', side_effect=lambda pid=None: lookup[pid])
        self.patch.start()
        self.addCleanup(self.patch.stop)
        self.clock_patch = patch.object(resources.time, 'monotonic_ns', return_value=1000000000)
        self.clock = self.clock_patch.start()
        self.addCleanup(self.clock_patch.stop)
        self.sampler = resources.PersonRunResources('run', 'cpu')
        self.sampler.register('compute', 10)

    def test_observed_sum_deduplicates_and_does_not_sum_separate_peaks(self):
        self.sampler.register('extra', 20)
        self.sampler.sample()
        self.root.rss, self.child.rss = 200, 50
        self.clock.return_value = 1100000000
        self.sampler.sample()
        self.clock.return_value = 1200000000
        result = self.sampler.finish()
        self.assertEqual(result['sampledAggregatePeakRssBytes'], 300)
        self.assertEqual(sum(p['maxObservedRssBytes'] for p in result['processes']), 400)
        self.assertEqual(len(result['samples'][0]['processIdentityIds']), 2)
        self.assertEqual(result['gpuMeasurement'], 'not-applicable-cpu')
        count = len(result['samples'])
        self.sampler.sample(force=True)
        self.assertEqual(len(result['samples']), count)

    def test_poll_throttle_and_access_failure(self):
        self.sampler.sample()
        self.clock.return_value = 1050000000
        self.sampler.sample()
        self.assertEqual(len(self.sampler.samples), 1)
        self.child.denied = True
        self.clock.return_value = 1100000000
        result = self.sampler.finish()
        self.assertTrue(any('AccessDenied' in e for e in result['errors']))
        self.assertEqual(result['samples'][-1]['observedRssBytes'], 100)

    def test_pid_identity_reuse_stays_separate(self):
        self.sampler.sample()
        self.child.alive = False
        replacement = Process(20, 300, created=2)
        self.root.children_list = [replacement]
        self.clock.return_value = 1100000000
        result = self.sampler.finish()
        versions = [p for p in result['processes'] if p['pid'] == 20]
        self.assertEqual(len(versions), 2)
        self.assertNotEqual(versions[0]['identityId'], versions[1]['identityId'])
        self.assertNotEqual(result['samples'][0]['processIdentityIds'],
                            result['samples'][1]['processIdentityIds'])

    def test_each_resource_history_bound_is_visible(self):
        for constant, expected in [('MAX_ACTIVE', 'concurrent'), ('MAX_IDENTITIES', 'identity'),
                                   ('MAX_SAMPLES', 'sample bound')]:
            with self.subTest(bound=constant), patch.object(resources, constant, 1):
                sampler = resources.PersonRunResources('run', 'cpu')
                sampler.register('compute', 10)
                sampler.sample(force=True)
                sampler.sample(force=True)
                result = sampler.finish()
                self.assertTrue(any(expected in e for e in result['errors']), result['errors'])
                self.assertLessEqual(len(result['samples']), 3)
                self.assertLessEqual(len(result['processes']), 2)

    def test_artifact_bound_removes_samples_with_explicit_error(self):
        with patch.object(resources, 'MAX_ARTIFACT_BYTES', 4096):
            for i in range(40):
                self.clock.return_value = 1000000000 + i * 100000000
                self.sampler.sample()
            self.clock.return_value += 100000000
            result = self.sampler.finish()
        self.assertTrue(result['artifactTruncated'])
        self.assertEqual(result['samples'], [])
        self.assertTrue(any('artifact byte bound' in e for e in result['errors']))


class ResourceAdmissionTests(unittest.TestCase):
    def setUp(self):
        self.timing = {'runId': 'run', 'missionId': 'run', 'windows': [{
            'windowId': 'window-0', 'workerPid': 20, 'modelInitStartNs': 100,
            'modelInitEndNs': 140, 'inferenceEndNs': 200}]}
        self.resource = {'schema': 'uav-person-resources-v1', 'runId': 'run', 'missionId': 'run',
            'clockDomain': 'CLOCK_MONOTONIC_NS', 'requestedDevice': 'cpu', 'registeredRoots': {'compute': 10},
            'samples': [{'monotonicNs': 150, 'observedRssBytes': 300, 'observedThreads': 4,
                         'processIdentityIds': [1, 2]}],
            'processes': [{'identityId': 1, 'pid': 10, 'creationTimeUnixSeconds': 1,
                          'owner': 'compute', 'firstSampleNs': 150, 'lastSampleNs': 150,
                          'maxObservedRssBytes': 100, 'maxObservedThreads': 2, 'cpuAffinity': [0]},
                         {'identityId': 2, 'pid': 20, 'creationTimeUnixSeconds': 2,
                          'owner': 'compute/descendant', 'firstSampleNs': 150, 'lastSampleNs': 150,
                          'maxObservedRssBytes': 200, 'maxObservedThreads': 2, 'cpuAffinity': [0]}],
            'sampledAggregatePeakRssBytes': 300, 'maxObservedThreads': 4,
            'targetSamplePeriodNs': 100000000, 'largestObservedGapNs': None,
            'gpuMeasurement': 'not-applicable-cpu', 'host': {'cpuModel': 'literalCPU',
            'logicalCpus': 2, 'launcherAffinity': [0, 1], 'pythonVersion': 'fixture', 'psutilVersion': 'fixture'},
            'errors': [], 'scope': 'fixture', 'measurement': 'sampled RSS', 'limitations': 'sampling'}

    def check(self):
        return validate_person_resources(self.resource, self.timing, {'compute'})

    def test_loaded_model_process_is_required(self):
        self.assertTrue(self.check()['complete'])
        self.resource['samples'][0]['monotonicNs'] = 110
        for process in self.resource['processes']:
            process['firstSampleNs'] = process['lastSampleNs'] = 110
        result = self.check()
        self.assertFalse(result['complete'])
        self.assertIn('no loaded-model', ' '.join(result['errors']))

    def test_worker_boundary_samples_cover_short_jobs_without_adding_aggregate_rss(self):
        first = self.timing['windows'][0]
        first.update(inferenceStartNs=160, modelResourceSample={
            'monotonicNs': 140, 'pid': 20, 'creationTimeUnixSeconds': 2,
            'rssBytes': 900, 'threads': 3})
        second = copy.deepcopy(first)
        second.update(windowId='window-1', modelInitEndNs=300,
                      inferenceStartNs=301, inferenceEndNs=302)
        second['modelResourceSample'].update(monotonicNs=301, rssBytes=1000)
        self.timing['windows'].append(second)
        # Both aggregate polls lie outside the model intervals and bracket the worker lifetime.
        self.resource['samples'][0]['monotonicNs'] = 110
        self.resource['samples'].append({**self.resource['samples'][0], 'monotonicNs': 400})
        for process in self.resource['processes']:
            process['firstSampleNs'], process['lastSampleNs'] = 110, 400
        original = copy.deepcopy(self.resource)
        result = self.check()
        self.assertTrue(result['complete'], result['errors'])
        coverage = result['summary']['modelCoverage']
        self.assertEqual([p['observationKind'] for p in coverage], ['worker-boundary'] * 2)
        self.assertEqual([p['workerSample']['rssBytes'] for p in coverage], [900, 1000])
        self.assertEqual(result['summary']['sampledAggregatePeakRssBytes'], 300)
        self.assertEqual(result['summary']['maxObservedThreads'], 4)
        self.assertEqual(self.resource, original)

    def test_advertised_worker_sample_rejects_bad_identity_time_and_measurements(self):
        window = self.timing['windows'][0]
        window['inferenceStartNs'] = 160
        valid = {'monotonicNs': 150, 'pid': 20, 'creationTimeUnixSeconds': 2,
                 'rssBytes': 200, 'threads': 2}
        cases = [None, {}, {'error': 'AccessDenied'}, {**valid, 'extra': 1}]
        for field, value in [('pid', 10), ('pid', 21), ('pid', True),
                             ('creationTimeUnixSeconds', 3), ('creationTimeUnixSeconds', True),
                             ('creationTimeUnixSeconds', float('inf')),
                             ('monotonicNs', 139), ('monotonicNs', 161), ('monotonicNs', True),
                             ('rssBytes', 0), ('rssBytes', -1), ('rssBytes', True),
                             ('threads', 0), ('threads', '2'), ('threads', True)]:
            cases.append({**valid, field: value})
        for field in valid:
            cases.append({k: v for k, v in valid.items() if k != field})
        for sample in cases:
            with self.subTest(sample=sample):
                window['modelResourceSample'] = sample
                result = self.check()
                # Even a valid in-interval aggregate poll cannot hide malformed advertised data.
                self.assertFalse(result['complete'])
                self.assertIn('invalid model resource sample', ' '.join(result['errors']))
        window['modelResourceSample'] = valid
        self.resource['processes'][1]['owner'] = 'different/descendant'
        self.assertFalse(self.check()['complete'])

    def test_creation_identity_selects_one_observed_pid_generation(self):
        window = self.timing['windows'][0]
        window.update(inferenceStartNs=160, modelResourceSample={
            'monotonicNs': 150, 'pid': 20, 'creationTimeUnixSeconds': 2,
            'rssBytes': 200, 'threads': 2})
        replacement = {**self.resource['processes'][1], 'identityId': 3,
                       'creationTimeUnixSeconds': 3, 'firstSampleNs': 250, 'lastSampleNs': 250}
        self.resource['processes'].append(replacement)
        self.resource['samples'].append({'monotonicNs': 250, 'observedRssBytes': 300,
                                        'observedThreads': 4, 'processIdentityIds': [1, 3]})
        self.resource['processes'][0]['lastSampleNs'] = 250
        self.assertTrue(self.check()['complete'])
        window['modelResourceSample']['creationTimeUnixSeconds'] = 3
        self.assertFalse(self.check()['complete'])  # The replacement was only observed later.
        window['modelResourceSample']['creationTimeUnixSeconds'] = 4
        self.assertFalse(self.check()['complete'])

    def test_scope_missing_root_worker_and_gpu_fail_closed(self):
        original = copy.deepcopy(self.resource)
        for field, value in [('runId', 'wrong'), ('gpuMeasurement', 'unavailable'),
                             ('errors', ['access denied'])]:
            with self.subTest(field=field):
                self.resource = {**copy.deepcopy(original), field: value}
                try:
                    result = self.check()
                    self.assertFalse(result['complete'])
                except ValueError:
                    pass
        self.resource = original
        self.timing['windows'][0]['workerPid'] = None
        self.assertFalse(self.check()['complete'])
        self.timing['windows'][0]['workerPid'] = 10
        self.assertFalse(self.check()['complete'])
        self.timing['windows'][0]['workerPid'] = 20
        self.assertFalse(validate_person_resources(self.resource, self.timing, {'compute', 'nfd-uav1'})['complete'])

    def test_malformed_sample_and_duplicate_identity_reject(self):
        self.resource['samples'][0]['observedRssBytes'] = True
        with self.assertRaises(ValueError): self.check()
        self.resource['samples'][0]['observedRssBytes'] = 300
        self.resource['processes'][1]['identityId'] = 1
        with self.assertRaises(ValueError): self.check()


class RealResourceProcessTests(unittest.TestCase):
    def test_small_real_child_is_observed_and_reaped(self):
        child = subprocess.Popen([sys.executable, '-c',
            "import time; a=bytearray(8*1024*1024); print('READY',flush=True); time.sleep(2)"],
            stdout=subprocess.PIPE, text=True)
        try:
            self.assertEqual(child.stdout.readline().strip(), 'READY')
            sampler = resources.PersonRunResources('bounded-child-fixture', 'cpu')
            sampler.register('fixture', child.pid)
            sampler.sample()
            time.sleep(.11)
            result = sampler.finish()
            record = next(p for p in result['processes'] if p['pid'] == child.pid)
            self.assertGreater(record['maxObservedRssBytes'], 8*1024*1024)
            self.assertGreaterEqual(result['sampleCount'], 2)
            self.assertEqual(result['errors'], [])
        finally:
            if child.poll() is None: child.terminate()
            try: child.wait(timeout=2)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait(timeout=2)
            child.stdout.close()
        self.assertIsNotNone(child.returncode)


if __name__ == '__main__':
    unittest.main()
