from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "ndn-service-framework/CertificateBootstrap.cpp"


def test_certificate_bootstrap_signed_interest_keeps_parameters_digest_last() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index(
        'ndn::Interest interest(makeControlRpcName(controllerPrefix, "CERTBOOTSTRAP", parameters,'
    )
    end = source.index("bool done = false;", start)
    snippet = source[start:end]

    parameters = snippet.index("interest.setApplicationParameters(parameters);")
    version = snippet.index(
        "signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);"
    )
    assert parameters < version < snippet.index("keyChain.sign(interest, signingInfo);")
