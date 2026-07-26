"""Qwen adapter and lazy candidate registration."""

def register_candidates(**kwargs):
    from ...llm_stub_planner import qwen_model_candidates
    return qwen_model_candidates(**kwargs)

def load_pilot():
    from . import pilot
    return pilot

__all__ = ["load_pilot", "register_candidates"]
