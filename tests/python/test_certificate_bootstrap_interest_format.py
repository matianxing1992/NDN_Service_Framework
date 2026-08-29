from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "ndn-service-framework/CertificateBootstrap.cpp"


def test_certificate_bootstrap_signed_interest_keeps_parameters_digest_last() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    start = source.index(
        "ndn::Interest interest(makeCertificateBootstrapName(controllerPrefix, bootstrapIdentity));"
    )
    end = source.index("bool done = false;", start)
    snippet = source[start:end]

    assert "interest.setApplicationParameters(encryptedRequest.wireEncode());" in snippet
    assert "signingInfo.setSignedInterestFormat(ndn::security::SignedInterestFormat::V03);" in snippet
    assert "keyChain.sign(interest, signingInfo);" in snippet
