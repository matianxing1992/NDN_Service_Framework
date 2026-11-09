// Installed fault fixture: seed original-shaped, unauthorized-signer Data in real NFD CS.
#include <ndn-cxx/face.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/security/pib/identity.hpp>
#include <ndn-cxx/security/pib/key.hpp>
#include <ndn-cxx/security/pib/pib.hpp>
#include <ndn-cxx/security/signing-helpers.hpp>
#include <ndn-cxx/security/verification-helpers.hpp>
#include <ndn-cxx/transport/unix-transport.hpp>

#include <boost/asio/io_context.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
constexpr size_t MAX_SEGMENTS = 1024;
constexpr size_t MAX_WIRE_BYTES = 8 * 1024 * 1024;

void
require(bool condition, const std::string& reason)
{
  if (!condition) throw std::runtime_error(reason);
}

void
trace(Clock::time_point started, const std::string& phase,
      size_t completed = 0, size_t total = 0)
{
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
    Clock::now() - started).count();
  std::cerr << "SPEC219_CACHED_SIGNER_PROGRESS phase=" << phase
            << " elapsed_ms=" << elapsed;
  if (total != 0) {
    std::cerr << " completed=" << completed << " total=" << total;
  }
  std::cerr << std::endl;
}

std::string
digest(const ndn::Block& block)
{
  unsigned char result[SHA256_DIGEST_LENGTH];
  SHA256(block.data(), block.size(), result);
  std::ostringstream output;
  output << "sha256:" << std::hex << std::setfill('0');
  for (auto byte : result) output << std::setw(2) << static_cast<unsigned>(byte);
  return output.str();
}

void
archive(const std::filesystem::path& path, const ndn::Block& block)
{
  std::ofstream output(path, std::ios::binary);
  output.write(reinterpret_cast<const char*>(block.data()),
               static_cast<std::streamsize>(block.size()));
  require(static_cast<bool>(output), "packet archive write failed");
}

template<typename Predicate>
void
pump(boost::asio::io_context& io, Clock::time_point deadline, Predicate done)
{
  while (!done() && Clock::now() < deadline) {
    const auto remaining = deadline - Clock::now();
    const auto quantum = std::min(remaining,
      std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(5)));
    io.restart();
    io.run_for(quantum);
  }
  require(done(), "fixture operation deadline expired");
}

ndn::Data
fetch(ndn::Face& face, const ndn::Name& name, Clock::time_point deadline)
{
  std::optional<ndn::Data> result;
  std::string error;
  ndn::Interest interest(name);
  interest.setCanBePrefix(false).setMustBeFresh(true)
    .setInterestLifetime(ndn::time::milliseconds(1000));
  const auto pending = face.expressInterest(interest,
    [&] (const auto&, const ndn::Data& data) { result = data; },
    [&] (const auto&, const auto&) { error = "fixture exact Interest Nacked"; },
    [&] (const auto&) { error = "fixture exact Interest timed out"; });
  try {
    pump(face.getIoContext(), deadline, [&] { return result || !error.empty(); });
  }
  catch (...) {
    pending.cancel();
    throw;
  }
  pending.cancel();
  require(result.has_value(), error);
  require(result->getName() == name, "fixture received a different exact segment");
  return *result;
}

int
run(int argc, char** argv)
{
  require(argc == 15, "expected seven explicit fixture options");
  std::map<std::string, std::string> options;
  for (int i = 1; i < argc; i += 2) {
    require(options.emplace(argv[i], argv[i + 1]).second, "duplicate fixture option");
  }
  for (const auto* key : {"--origin-socket", "--cache-socket", "--keychain", "--object",
                          "--producer", "--signer", "--output"}) {
    require(options.count(key) != 0, "missing required fixture option");
  }
  const ndn::Name producerName(options.at("--producer"));
  const ndn::Name signerName(options.at("--signer"));
  const ndn::Name objectName(options.at("--object"));
  const auto started = Clock::now();
  require(producerName == ndn::Name("/example/uav/drone/UAV3") &&
          signerName == ndn::Name("/example/uav/drone/UAV1"), "unexpected fixture actors");
  require(producerName.isPrefixOf(objectName) && objectName.size() == producerName.size() + 7 &&
          objectName.getSubName(producerName.size(), 3) == ndn::Name("/UAV/OBS/UAV3") &&
          objectName.at(-3) == ndn::name::Component("2") &&
          objectName.at(-2) == ndn::name::Component("FRAME") &&
          objectName.at(-1).isVersion() && objectName.at(-1).toVersion() == 1,
          "fixture requires UAV3 window-1 FRAME identity");
  const std::filesystem::path output(options.at("--output"));
  require(output.is_absolute() && std::filesystem::create_directory(output),
          "fixture output must be a new absolute directory");
  std::filesystem::permissions(output, std::filesystem::perms::owner_all);
  const std::filesystem::path keychainRoot(options.at("--keychain"));
  require(keychainRoot.is_absolute(), "fixture needs absolute private keychain");
  ndn::KeyChain keyChain("pib-sqlite3:" + (keychainRoot / "pib").string(),
                         "tpm-file:" + (keychainRoot / "tpm").string());
  const auto producerCertificate = keyChain.getPib().getIdentity(producerName)
    .getDefaultKey().getDefaultCertificate();
  const auto signerCertificate = keyChain.getPib().getIdentity(signerName)
    .getDefaultKey().getDefaultCertificate();
  archive(output / "producer.cert", producerCertificate.wireEncode());
  archive(output / "signer.cert", signerCertificate.wireEncode());

  // Faces are destroyed before every callback-captured packet/result variable.
  std::vector<ndn::Data> packets;
  bool registered = false;
  bool unregistered = false;
  std::string registrationError;
  size_t served = 0;
  boost::asio::io_context io;
  ndn::Face origin(std::make_shared<ndn::UnixTransport>(options.at("--origin-socket")),
                   io, keyChain);
  ndn::Face publisher(std::make_shared<ndn::UnixTransport>(options.at("--cache-socket")),
                      io, keyChain);
  ndn::Face primer(std::make_shared<ndn::UnixTransport>(options.at("--cache-socket")),
                   io, keyChain);
  const auto deadline = Clock::now() + std::chrono::seconds(20);
  auto firstName = objectName;
  firstName.appendSegment(0);
  auto first = fetch(origin, firstName, deadline);
  const auto finalBlock = first.getFinalBlock();
  require(finalBlock && finalBlock->isSegment() && finalBlock->toSegment() < MAX_SEGMENTS,
          "invalid original final block");
  const size_t count = static_cast<size_t>(finalBlock->toSegment()) + 1;
  trace(started, "original-segment-zero-fetched", 1, count);
  size_t retained = 0;
  size_t forgedRetained = 0;
  boost::property_tree::ptree manifest;
  boost::property_tree::ptree segments;
  manifest.put("schema", "spec219-cached-signer-fixture/v1");
  manifest.put("objectName", objectName.toUri());
  manifest.put("producer", producerName.toUri());
  manifest.put("signer", signerName.toUri());
  for (size_t i = 0; i < count; ++i) {
    auto name = objectName;
    name.appendSegment(i);
    const auto original = i == 0 ? first : fetch(origin, name, deadline);
    require(original.getFinalBlock() == finalBlock &&
            ndn::security::verifySignature(original, producerCertificate),
            "original shape or UAV3 signature invalid");
    ndn::Data forged(original);
    keyChain.sign(forged, ndn::security::signingByIdentity(signerName));
    require(forged.getName() == original.getName() &&
            forged.getMetaInfo().wireEncode() == original.getMetaInfo().wireEncode() &&
            forged.getContent() == original.getContent() &&
            ndn::security::verifySignature(forged, signerCertificate) &&
            !ndn::security::verifySignature(forged, producerCertificate),
            "wrong-signer packet invariant failed");
    require(original.wireEncode().size() <= MAX_WIRE_BYTES - retained,
            "original object exceeds fixture retention bound");
    require(forged.wireEncode().size() <= MAX_WIRE_BYTES - forgedRetained,
            "forged object exceeds fixture retention bound");
    retained += original.wireEncode().size();
    forgedRetained += forged.wireEncode().size();
    const auto originalFile = "original-" + std::to_string(i) + ".wire";
    const auto forgedFile = "forged-" + std::to_string(i) + ".wire";
    archive(output / originalFile, original.wireEncode());
    archive(output / forgedFile, forged.wireEncode());
    boost::property_tree::ptree segment;
    segment.put("name", name.toUri());
    segment.put("originalFile", originalFile);
    segment.put("forgedFile", forgedFile);
    segment.put("originalDigest", digest(original.wireEncode()));
    segment.put("forgedDigest", digest(forged.wireEncode()));
    segment.put("contentDigest", digest(original.getContent()));
    segments.push_back({"", segment});
    packets.push_back(std::move(forged));
  }
  trace(started, "original-packets-ready", count, count);
  trace(started, "prefix-registration-started");
  auto registration = publisher.setInterestFilter(objectName,
    [&] (const auto&, const ndn::Interest& interest) {
      for (const auto& packet : packets) {
        if (interest.matchesData(packet)) {
          require(served < 2 * MAX_SEGMENTS, "fixture publication count exceeded");
          publisher.put(packet);
          ++served;
          return;
        }
      }
    },
    [&] (const auto&) { registered = true; },
    [&] (const auto&, const std::string& reason) { registrationError = reason; },
    ndn::security::signingByIdentity(signerName),
    ndn::nfd::ROUTE_FLAG_CHILD_INHERIT | ndn::nfd::ROUTE_FLAG_CAPTURE);
  pump(io, deadline, [&] { return registered || !registrationError.empty(); });
  require(registered, "fixture prefix registration failed: " + registrationError);
  trace(started, "prefix-registration-completed");
  trace(started, "cache-seeding-started", 0, count);
  size_t seededCount = 0;
  for (const auto& packet : packets) {
    const auto seeded = fetch(primer, packet.getName(), deadline);
    require(seeded.wireEncode() == packet.wireEncode(), "cache seeding packet changed");
    ++seededCount;
    if (seededCount % 8 == 0 || seededCount == count) {
      trace(started, "cache-seeding-progress", seededCount, count);
    }
  }
  require(served == count, "cache seeding did not traverse the sole fixture publisher");
  trace(started, "cache-seeding-completed", count, count);
  trace(started, "prefix-unregistration-started");
  registration.unregister([&] { unregistered = true; },
    [&] (const std::string& reason) { registrationError = reason; });
  pump(io, deadline, [&] { return unregistered || !registrationError.empty(); });
  require(unregistered, "fixture prefix removal failed: " + registrationError);
  trace(started, "prefix-unregistration-completed");
  manifest.put("segmentsCount", count);
  manifest.put("retainedOriginalBytes", retained);
  manifest.put("solicitedPublications", served);
  manifest.put("allAssertionsPassed", true);
  manifest.put("prefixUnregistered", unregistered);
  manifest.add_child("segments", segments);
  boost::property_tree::write_json((output / "manifest.json").string(), manifest);
  primer.shutdown();
  publisher.shutdown();
  origin.shutdown();
  std::cout << "SPEC219_UNAUTHORIZED_CACHE_SEEDED segments=" << count
            << " original_bytes=" << retained << " prefix_unregistered=true" << std::endl;
  return 0;
}

} // namespace

int
main(int argc, char** argv)
{
  try {
    return run(argc, argv);
  }
  catch (const std::exception& error) {
    std::cerr << "SPEC219_UNAUTHORIZED_CACHE_FIXTURE_FAILED reason=" << error.what() << std::endl;
    return 1;
  }
}
