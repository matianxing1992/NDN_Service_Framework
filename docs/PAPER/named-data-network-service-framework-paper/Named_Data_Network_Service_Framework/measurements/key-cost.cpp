// Controlled provisioning microbenchmark; not an NDNSF network experiment.
#include <openabe/openabe.h>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using oabe::OpenABECryptoContext;
using Clock = std::chrono::steady_clock;

static double ms(Clock::time_point start) {
  return std::chrono::duration<double, std::milli>(Clock::now()-start).count();
}
static std::string policy(int count) {
  std::string value = "S1";
  for (int i=2; i<=count; ++i) value = "("+value+" or S"+std::to_string(i)+")";
  return value;
}
static void check(OpenABECryptoContext& ctx, const std::string& key,
                  int service, bool expected) {
  std::string ciphertext, recovered;
  const std::string plaintext(32, 'k');
  ctx.encrypt("|S"+std::to_string(service), plaintext, ciphertext);
  bool accepted = ctx.decrypt(key, ciphertext, recovered);
  if (accepted != expected || (accepted && recovered != plaintext))
    throw std::runtime_error("Permission-equivalence check failed");
}

int main(int argc, char** argv) {
  if (argc != 2) { std::cerr << "Usage: key-cost OUTPUT.csv\n"; return 2; }
  oabe::InitializeOpenABE();
  try {
    std::ofstream out(argv[1]);
    if (!out) throw std::runtime_error("Cannot open output");
    out << "permissions,repetition,design,issue_export_ms,key_bytes,grant_export_ms,grant_bytes,access_verified\n";
    out << std::fixed << std::setprecision(6);
    for (int p : {1,5,20}) {
      // Repetition zero is warm-up and is excluded from retained measurements.
      for (int rep=0; rep<=10; ++rep) {
        OpenABECryptoContext ctx("KP-ABE");
        ctx.generateParams();
        for (int turn=0; turn<2; ++turn) {
          const bool aggregate = ((turn+rep)%2 == 0);
          const std::string prefix = aggregate ? "aggregate" : "separate";
          std::vector<std::string> keys;
          size_t bytes=0;
          const auto begin=Clock::now();
          for (int i=1; i<=(aggregate ? 1 : p); ++i) {
            const auto key=prefix+std::to_string(i);
            ctx.keygen(aggregate ? policy(p) : "S"+std::to_string(i), key);
            std::string blob;
            ctx.exportUserKey(key, blob);
            bytes+=blob.size(); keys.push_back(key);
          }
          const double issueMs=ms(begin);
          for (int i=1; i<=p; ++i) check(ctx, aggregate ? keys[0] : keys[i-1], i, true);
          for (const auto& key : keys) check(ctx, key, p+1, false);
          if (!aggregate && p>1) check(ctx, keys[0], 2, false);
          const auto grantBegin=Clock::now();
          const std::string grantKey=prefix+"grant";
          ctx.keygen(aggregate ? policy(p+1) : "S"+std::to_string(p+1), grantKey);
          std::string granted;
          ctx.exportUserKey(grantKey, granted);
          const double grantMs=ms(grantBegin);
          check(ctx, grantKey, p+1, true);
          if (aggregate) check(ctx, grantKey, 1, true);
          else check(ctx, grantKey, 1, false);
          if (rep>0) out << p << ',' << rep << ',' << prefix << ',' << issueMs << ','
             << bytes << ',' << grantMs << ',' << granted.size() << ",true\n";
          for (const auto& key : keys) ctx.deleteKey(key);
          ctx.deleteKey(grantKey);
        }
      }
    }
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n'; oabe::ShutdownOpenABE(); return 1;
  }
  oabe::ShutdownOpenABE();
  return 0;
}
