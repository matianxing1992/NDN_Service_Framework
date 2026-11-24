#ifndef NDNSF_DI_DETAIL_NATIVE_JOURNAL_FILE_HPP
#define NDNSF_DI_DETAIL_NATIVE_JOURNAL_FILE_HPP

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ndnsf::di::detail {

struct JournalRecord
{
  std::string kindJson;
  std::string payloadJson;
  std::uint64_t timestampMs;
};

struct JournalFrame
{
  std::string wire;
  std::vector<std::string> checksums;
};

/** Frame one or more logical records, preserving the caller's canonical payload bytes. */
JournalFrame
frameJournalRecords(const std::vector<JournalRecord>& records, std::uint64_t timestampMs);

enum class JournalRecordValidation
{
  VALID,
  CHECKSUM_MISMATCH,
  SCHEMA_MISMATCH,
};

/** Validate an unsigned canonical record, including the encoded schema value. */
JournalRecordValidation
validateJournalRecord(const std::string& body, std::string_view checksum,
                      std::string_view schemaJson);

class NativeJournalRollbackError : public std::runtime_error
{
public:
  using std::runtime_error::runtime_error;
};

/** Append one already framed transaction to an owned journal descriptor.
 * The caller holds the writer lock and supplies the last accepted file size.
 * A failed write is rolled back before the caller can advance its state.
 */
void
appendJournalWire(int fd, std::uint64_t offset, std::string_view wire);

/** Discard a parser-rejected tail while holding the journal writer lock. */
void
truncateJournalTail(int fd, std::uint64_t observedSize, std::uint64_t validSize);

} // namespace ndnsf::di::detail

#endif
