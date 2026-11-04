"""Bounded, poll-driven observations of this run's applications and NFD processes."""
from __future__ import annotations

from pathlib import Path
import sys
import json
import time
from typing import Any

import psutil

SAMPLE_PERIOD_NS = 100000000
MAX_SAMPLES = 10000
MAX_IDENTITIES = 512
MAX_ACTIVE = 64
MAX_ARTIFACT_BYTES = 16 * 1024 * 1024


class PersonRunResources:
    """Owned by the existing runner; no background work or process control."""

    def __init__(self, run_id: str, requested_device: str):
        self.run_id = run_id
        self.requested_device = requested_device
        self.roots = {}
        self.processes = {}
        self.samples = []
        self.errors = []
        self._live = {}
        self._last_ns = None
        self._finished = False
        cpu_model = None
        try:
            with Path('/proc/cpuinfo').open('r') as stream:
                text = stream.read(262144)
            cpu_model = next((line.split(':', 1)[1].strip() for line in text.splitlines()
                              if line.startswith('model name') and ':' in line), None)
            affinity = psutil.Process().cpu_affinity()
        except (OSError, psutil.Error) as error:
            affinity = []
            self._error('host metadata: ' + type(error).__name__)
        if len(affinity) > 1024:
            self._error('host CPU affinity exceeds bound')
        try:
            logical = psutil.cpu_count(logical=True)
            physical = psutil.cpu_count(logical=False)
        except OSError as error:
            logical = physical = None
            self._error('CPU counts: ' + type(error).__name__)
        self.host = {'cpuModel': cpu_model, 'logicalCpus': logical,
                     'physicalCores': physical,
                     'launcherAffinity': affinity[:1024], 'python': sys.executable,
                     'pythonVersion': sys.version, 'psutilVersion': psutil.__version__,
                     'psutilPath': psutil.__file__}

    def _error(self, message):
        if len(self.errors) < 64:
            self.errors.append(message[:512])

    def register(self, label: str, pid: int):
        if self._finished:
            return
        if label in self.roots or len(self.roots) >= MAX_ACTIVE or type(pid) is not int or pid <= 0:
            self._error('invalid or duplicate resource root: ' + label)
            return
        try:
            process = psutil.Process(pid)
            self.roots[label] = process
            process.create_time()  # Pin psutil's PID identity before observing descendants.
        except psutil.Error as error:
            self._error(label + ': registration ' + type(error).__name__)

    def sample(self, *, force=False):
        if self._finished:
            return
        now = time.monotonic_ns()
        if self._last_ns is not None and not force and now - self._last_ns < SAMPLE_PERIOD_NS:
            return
        self._last_ns = now
        if len(self.samples) >= MAX_SAMPLES:
            self._error('resource sample bound exceeded')
            return
        candidates = dict(self._live)
        for label, root in self.roots.items():
            try:
                if not root.is_running():
                    continue
                candidates[root.pid] = (root, label)
                for child in root.children(recursive=True):
                    if len(candidates) >= MAX_ACTIVE and child.pid not in candidates:
                        self._error('concurrent observed process bound exceeded')
                        break
                    prior = candidates.get(child.pid)
                    if prior is None or prior[0].create_time() != child.create_time():
                        candidates[child.pid] = child, label + '/descendant'
            except psutil.NoSuchProcess:
                pass
            except psutil.Error as error:
                self._error(label + ': discovery ' + type(error).__name__)
        if len(candidates) > MAX_ACTIVE:
            self._error('concurrent observed process bound exceeded')
        rss, threads, active, identities = 0, 0, {}, []
        for pid, (process, label) in list(candidates.items())[:MAX_ACTIVE]:
            try:
                if not process.is_running() or process.status() == psutil.STATUS_ZOMBIE:
                    continue
                with process.oneshot():
                    created = process.create_time()
                    memory = process.memory_info().rss
                    thread_count = process.num_threads()
                identity = pid, created
                if identity not in self.processes:
                    if len(self.processes) >= MAX_IDENTITIES:
                        self._error('observed process identity bound exceeded')
                        continue
                    affinity = process.cpu_affinity()
                    if len(affinity) > 1024:
                        self._error('process affinity exceeds bound')
                    self.processes[identity] = {'identityId': len(self.processes) + 1,
                        'pid': pid, 'creationTimeUnixSeconds': created,
                        'owner': label, 'firstSampleNs': now, 'lastSampleNs': now,
                        'maxObservedRssBytes': 0, 'maxObservedThreads': 0,
                        'cpuAffinity': affinity[:1024]}
                record = self.processes[identity]
                record['lastSampleNs'] = now
                record['maxObservedRssBytes'] = max(record['maxObservedRssBytes'], memory)
                record['maxObservedThreads'] = max(record['maxObservedThreads'], thread_count)
                rss += memory
                threads += thread_count
                active[pid] = process, label
                identities.append(record['identityId'])
            except psutil.NoSuchProcess:
                pass
            except psutil.Error as error:
                self._error(label + ': sample ' + type(error).__name__)
        self._live = active
        self.samples.append({'monotonicNs': now, 'observedRssBytes': rss,
                             'observedThreads': threads, 'processIdentityIds': sorted(identities)})

    def finish(self) -> dict[str, Any]:
        if not self._finished:
            self.sample(force=True)
            self._finished = True
        gaps = [b['monotonicNs'] - a['monotonicNs'] for a, b in zip(self.samples, self.samples[1:])]
        result = {'schema': 'uav-person-resources-v1', 'runId': self.run_id, 'missionId': self.run_id,
                'clockDomain': 'CLOCK_MONOTONIC_NS', 'requestedDevice': self.requested_device,
                'targetSamplePeriodNs': SAMPLE_PERIOD_NS,
                'scope': 'registered application/NFD roots and observed descendants',
                'measurement': 'sampled sum of RSS; shared pages may be counted by several processes',
                'registeredRoots': {name: p.pid for name, p in self.roots.items()},
                'samples': self.samples, 'processes': list(self.processes.values()),
                'sampleCount': len(self.samples), 'largestObservedGapNs': max(gaps) if gaps else None,
                'sampledAggregatePeakRssBytes': max((x['observedRssBytes'] for x in self.samples), default=None),
                'maxObservedThreads': max((x['observedThreads'] for x in self.samples), default=None),
                'gpuMeasurement': 'not-applicable-cpu' if self.requested_device == 'cpu' else 'unavailable',
                'host': self.host, 'errors': self.errors,
                'limitations': 'polling may miss transient peaks and short-lived helper processes'}

        if len((json.dumps(result, indent=2, sort_keys=True) + '\n').encode()) > MAX_ARTIFACT_BYTES:
            self._error('resource artifact byte bound exceeded')
            result.update(samples=[], processes=[], sampleCount=0, artifactTruncated=True,
                          sampledAggregatePeakRssBytes=None, maxObservedThreads=None)
        return result
