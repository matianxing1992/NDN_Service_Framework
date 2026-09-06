"""Compile the actual Core digest helpers and compare their wire identities."""
import hashlib
from pathlib import Path
import shlex
import subprocess

import pytest

ROOT = Path(__file__).resolve().parents[2]


def digest_function(path):
    text = path.read_text()
    start = text.index("sha256DigestString(const ndn::Buffer& payload)")
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return "std::string " + text[start:end]


@pytest.fixture(scope="module")
def digest_probe(tmp_path_factory):
    output = tmp_path_factory.mktemp("core-digest-probe")
    source = output / "probe.cpp"
    source.write_text(
        '#include <ndn-cxx/util/sha256.hpp>\n'
        '#include <ndn-cxx/encoding/buffer.hpp>\n'
        '#include <algorithm>\n#include <cctype>\n#include <iostream>\n#include <iterator>\n'
        + "namespace requester {\n" + digest_function(ROOT / "ndn-service-framework/ServiceUser.cpp") + "\n}\n"
        + "namespace provider {\n" + digest_function(ROOT / "ndn-service-framework/ServiceProvider.cpp") + "\n}\n"
        + 'int main() { std::string input((std::istreambuf_iterator<char>(std::cin)), {});\n'
          'ndn::Buffer payload(input.begin(), input.end());\n'
          'std::cout << requester::sha256DigestString(payload) << "\\n"\n'
          '          << provider::sha256DigestString(payload) << "\\n"; }\n')
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "libndn-cxx"], text=True))
    binary = output / "probe"
    build = subprocess.run(["/usr/bin/g++", "-B/usr/bin", "-std=c++17", str(source),
                            "-o", str(binary), *flags], capture_output=True,
                           text=True, timeout=60)
    assert build.returncode == 0, build.stderr
    return binary


@pytest.mark.parametrize("payload", [b"", b"abc", bytes(range(256)), b"assignment" * 3072],
                         ids=["empty", "ascii", "binary", "external-assignment"])
def test_requester_and_provider_emit_identical_canonical_digest(digest_probe, payload):
    result = subprocess.run([str(digest_probe)], input=payload, capture_output=True,
                            check=True, timeout=10)
    expected = "sha256:" + hashlib.sha256(payload).hexdigest()
    assert result.stdout.decode().splitlines() == [expected, expected]
