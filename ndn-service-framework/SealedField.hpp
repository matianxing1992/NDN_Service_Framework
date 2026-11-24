#ifndef NDNSF_SEALED_FIELD_HPP
#define NDNSF_SEALED_FIELD_HPP

#include <string>
#include <vector>

namespace ndn_service_framework {

/** A field sealed at rest with AES-256-GCM.
 *
 * The marker keeps a sealed field recognisable inside a layout that still stores strings, so a reader can
 * tell "sealed with this key" from "legacy plaintext" without a format version bump. The associated data
 * binds the field to its purpose, so a ciphertext lifted from one place does not open in another.
 */
constexpr char SEALED_FIELD_PREFIX[] = "ndnsf-sealed-1:";

/** Seal `plaintext` under `key` (32 bytes) and return the marked, base64-encoded field. */
std::string
sealField(const std::vector<uint8_t>& key, const std::string& plaintext,
          const std::string& associatedData);

/** Open a marked field; returns false for a marker this build does not know or a failed authentication. */
bool
openField(const std::vector<uint8_t>& key, const std::string& field,
          const std::string& associatedData, std::string& plaintext);

/** True when the field carries the seal marker. */
bool
isSealedField(const std::string& field);

} // namespace ndn_service_framework

#endif // NDNSF_SEALED_FIELD_HPP
