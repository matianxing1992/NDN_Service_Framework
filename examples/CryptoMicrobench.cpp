#include <ndn-cxx/data.hpp>
#include <ndn-cxx/encoding/buffer-stream.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/transform/base64-decode.hpp>
#include <ndn-cxx/security/transform/block-cipher.hpp>
#include <ndn-cxx/security/transform/buffer-source.hpp>
#include <ndn-cxx/security/transform/public-key.hpp>
#include <ndn-cxx/security/transform/stream-sink.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/util/random.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct Stats
{
  size_t count = 0;
  double minUs = 0.0;
  double p50Us = 0.0;
  double p95Us = 0.0;
  double meanUs = 0.0;
  double maxUs = 0.0;
};

Stats
computeStats(std::vector<double> values)
{
  if (values.empty()) {
    return {};
  }
  std::sort(values.begin(), values.end());
  const auto percentile = [&] (double p) {
    const double pos = (values.size() - 1) * p;
    const auto lo = static_cast<size_t>(pos);
    const auto hi = std::min(lo + 1, values.size() - 1);
    const double frac = pos - lo;
    return values[lo] * (1.0 - frac) + values[hi] * frac;
  };
  const double total = std::accumulate(values.begin(), values.end(), 0.0);
  return Stats{
    values.size(),
    values.front(),
    percentile(0.50),
    percentile(0.95),
    total / values.size(),
    values.back()
  };
}

template<typename F>
Stats
measure(size_t iterations, F&& fn)
{
  std::vector<double> values;
  values.reserve(iterations);
  for (size_t i = 0; i < iterations; ++i) {
    const auto start = Clock::now();
    fn(i);
    const auto stop = Clock::now();
    values.push_back(std::chrono::duration_cast<std::chrono::duration<double, std::micro>>(stop - start).count());
  }
  return computeStats(std::move(values));
}

void
printStats(const std::string& name, const Stats& stats)
{
  std::cout << "CRYPTO_MICROBENCH "
            << "name=" << name
            << " count=" << stats.count
            << std::fixed << std::setprecision(2)
            << " min_us=" << stats.minUs
            << " p50_us=" << stats.p50Us
            << " p95_us=" << stats.p95Us
            << " mean_us=" << stats.meanUs
            << " max_us=" << stats.maxUs
            << "\n";
}

ndn::Buffer
runAesCbc(ndn::span<const uint8_t> input,
          ndn::span<const uint8_t> key,
          ndn::span<const uint8_t> iv,
          ndn::CipherOperator op)
{
  ndn::OBufferStream output;
  ndn::security::transform::bufferSource(input) >>
    ndn::security::transform::blockCipher(ndn::BlockCipherAlgorithm::AES_CBC, op, key, iv) >>
    ndn::security::transform::streamSink(output);
  return *output.buf();
}

ndn::Data
makeData(const ndn::Name& name, const std::vector<uint8_t>& payload)
{
  ndn::Data data(name);
  data.setFreshnessPeriod(ndn::time::seconds(10));
  data.setContent(ndn::make_span(payload));
  return data;
}

ndn::security::Certificate
makeIdentity(ndn::KeyChain& keyChain, const ndn::Name& identity, const ndn::KeyParams& params)
{
  try {
    return keyChain.getPib().getIdentity(identity).getDefaultKey().getDefaultCertificate();
  }
  catch (const std::exception&) {
    return keyChain.createIdentity(identity, params).getDefaultKey().getDefaultCertificate();
  }
}

size_t
readSizeArg(int argc, char** argv, const std::string& name, size_t defaultValue)
{
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] == name) {
      return static_cast<size_t>(std::stoul(argv[i + 1]));
    }
  }
  return defaultValue;
}

} // namespace

int
main(int argc, char** argv)
{
  const size_t iterations = readSizeArg(argc, argv, "--iterations", 1000);
  const size_t payloadSize = readSizeArg(argc, argv, "--payload-size", 1024);

  std::vector<uint8_t> payload(payloadSize);
  ndn::random::generateSecureBytes(ndn::make_span(payload));

  ndn::KeyChain rsaKeyChain("pib-memory:crypto-microbench-rsa",
                            "tpm-memory:crypto-microbench-rsa");
  ndn::KeyChain ecKeyChain("pib-memory:crypto-microbench-ec",
                           "tpm-memory:crypto-microbench-ec");

  auto rsaCert = makeIdentity(rsaKeyChain,
                              ndn::Name("/crypto-microbench/rsa"),
                              ndn::RsaKeyParams(2048));
  auto ecCert = makeIdentity(ecKeyChain,
                             ndn::Name("/crypto-microbench/ec"),
                             ndn::EcKeyParams());

  auto rsaSignedData = makeData("/crypto-microbench/rsa/data", payload);
  rsaKeyChain.sign(rsaSignedData, ndn::security::signingByCertificate(rsaCert));
  auto ecSignedData = makeData("/crypto-microbench/ec/data", payload);
  ecKeyChain.sign(ecSignedData, ndn::security::signingByCertificate(ecCert));

  ndn::security::transform::PublicKey rsaPublicKey;
  rsaPublicKey.loadPkcs8(rsaCert.getPublicKey());
  ndn::security::transform::PublicKey ecPublicKey;
  ecPublicKey.loadPkcs8(ecCert.getPublicKey());

  ndn::Buffer aesKey(32);
  ndn::Buffer iv(16);
  ndn::random::generateSecureBytes(ndn::make_span(aesKey));
  ndn::random::generateSecureBytes(ndn::make_span(iv));
  auto cipherText = runAesCbc(ndn::make_span(payload), ndn::make_span(aesKey), ndn::make_span(iv),
                              ndn::CipherOperator::ENCRYPT);
  auto encryptedAesKey = rsaPublicKey.encrypt(ndn::make_span(aesKey));

  // One warmup pass keeps first-use allocation and OpenSSL lazy initialization out of the measured loop.
  for (size_t i = 0; i < std::min<size_t>(100, iterations); ++i) {
    auto data = makeData(ndn::Name("/crypto-microbench/warmup/rsa").appendNumber(i), payload);
    rsaKeyChain.sign(data, ndn::security::signingByCertificate(rsaCert));
    (void)ndn::security::verifySignature(data, rsaPublicKey);
    (void)runAesCbc(ndn::make_span(payload), ndn::make_span(aesKey), ndn::make_span(iv),
                    ndn::CipherOperator::ENCRYPT);
  }

  printStats("rsa2048_keychain_sign_data", measure(iterations, [&] (size_t i) {
    auto data = makeData(ndn::Name("/crypto-microbench/rsa/sign").appendNumber(i), payload);
    rsaKeyChain.sign(data, ndn::security::signingByCertificate(rsaCert));
  }));

  printStats("rsa2048_verify_signature_helper", measure(iterations, [&] (size_t) {
    if (!ndn::security::verifySignature(rsaSignedData, rsaPublicKey)) {
      throw std::runtime_error("RSA verify failed");
    }
  }));

  printStats("ecdsa_keychain_sign_data", measure(iterations, [&] (size_t i) {
    auto data = makeData(ndn::Name("/crypto-microbench/ec/sign").appendNumber(i), payload);
    ecKeyChain.sign(data, ndn::security::signingByCertificate(ecCert));
  }));

  printStats("ecdsa_verify_signature_helper", measure(iterations, [&] (size_t) {
    if (!ndn::security::verifySignature(ecSignedData, ecPublicKey)) {
      throw std::runtime_error("ECDSA verify failed");
    }
  }));

  printStats("aes256_cbc_encrypt", measure(iterations, [&] (size_t) {
    (void)runAesCbc(ndn::make_span(payload), ndn::make_span(aesKey), ndn::make_span(iv),
                    ndn::CipherOperator::ENCRYPT);
  }));

  printStats("aes256_cbc_decrypt", measure(iterations, [&] (size_t) {
    (void)runAesCbc(ndn::make_span(cipherText), ndn::make_span(aesKey), ndn::make_span(iv),
                    ndn::CipherOperator::DECRYPT);
  }));

  printStats("rsa2048_unwrap_aes_key", measure(iterations, [&] (size_t) {
    auto unwrapped = rsaKeyChain.getTpm().decrypt(ndn::make_span(*encryptedAesKey), rsaCert.getKeyName());
    if (unwrapped == nullptr || unwrapped->size() != aesKey.size()) {
      throw std::runtime_error("RSA unwrap failed");
    }
  }));

  return 0;
}
