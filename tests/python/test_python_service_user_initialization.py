from pathlib import Path


def test_python_service_user_initializes_before_permission_fetch():
    source = (
        Path(__file__).resolve().parents[2]
        / "pythonWrapper/src/ndnsf/_ndnsf.cpp"
    ).read_text(encoding="utf-8")
    init_pos = source.index("m_user->init();", source.index("class NativeServiceUser"))
    fetch_pos = source.index(
        "m_user->fetchPermissionsFromController(m_controller);",
        source.index("class NativeServiceUser"),
    )
    assert init_pos < fetch_pos
