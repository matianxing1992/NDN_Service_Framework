import importlib.util
from pathlib import Path
import pytest

path = Path(__file__).resolve().parents[2] / 'tools/ndnsf-di/compare_generation_lengths.py'
spec = importlib.util.spec_from_file_location('generation_lengths', path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def test_exact_additive_partition_keeps_transition_directions_distinct():
    calls = [dict(role='/LLM/Pipeline/Stage/0', epoch=0, started_ns=10, finished_ns=20),
             dict(role='/LLM/Pipeline/Stage/1', epoch=0, started_ns=30, finished_ns=50),
             dict(role='/LLM/Pipeline/Stage/0', epoch=1, started_ns=70, finished_ns=80)]
    result = module.backend_chain_partition(0, 100, calls)
    assert result['status'] == 'ADDITIVE_SEQUENTIAL_CHAIN'
    assert result['backend_seconds'] == 40 / 1e9
    assert result['other_seconds'] == 60 / 1e9
    assert [row['kind'] for row in result['gaps']] == [
        'before_first_backend', 'stage0_to_stage1_same_epoch',
        'stage1_to_stage0_next_epoch', 'after_last_backend']


def test_overlapping_backends_do_not_claim_sequential_partition():
    calls = [dict(role='a', epoch=0, started_ns=10, finished_ns=50),
             dict(role='b', epoch=0, started_ns=30, finished_ns=70)]
    assert module.backend_chain_partition(0, 100, calls) == {
        'status': 'UNAVAILABLE_OVERLAPPING_BACKENDS'}


def test_unexpected_transition_is_explicit_instead_of_attributed_to_activation():
    calls = [dict(role='/LLM/Pipeline/Stage/0', epoch=0, started_ns=10, finished_ns=20),
             dict(role='/LLM/Pipeline/Stage/0', epoch=1, started_ns=30, finished_ns=50)]
    result = module.backend_chain_partition(0, 100, calls)
    assert result['gaps'][1]['kind'] == 'other_between_backends'


def context_turns():
    return [dict(turn=index, generated_tokens=32, roles=[dict(
        role='/LLM/Pipeline/Stage/0', cache_observations=[dict(
            representedPrefixTokenCount=25 if index == 0 else 81,
            prefixWorkAvoided=0 if index == 0 else 57,
            conversationStateHit=index == 1,
            actualNewInputExtent=25 if index == 0 else 24)])]) for index in (0, 1)]


def test_context_check_rejects_a_different_warm_start():
    turns = context_turns()
    turns[1]['roles'][0]['cache_observations'][0]['representedPrefixTokenCount'] = 82
    with pytest.raises(AssertionError):
        module.verify_context(turns, dict(prefixTokenCount=57))


def test_context_check_rejects_a_different_cold_checkpoint():
    with pytest.raises(AssertionError):
        module.verify_context(context_turns(), dict(prefixTokenCount=58))
