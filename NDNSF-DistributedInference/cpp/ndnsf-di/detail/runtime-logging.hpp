#ifndef NDNSF_DI_DETAIL_RUNTIME_LOGGING_HPP
#define NDNSF_DI_DETAIL_RUNTIME_LOGGING_HPP

#include <iosfwd>

namespace ndnsf::di::detail {

/** Configure the existing process logger for serialized synchronous output.
 *  Call before starting workers or network activity. The caller owns output
 *  until the destination is replaced or the process exits. Records retain
 *  their levels and default text format; writes apply ordinary I/O backpressure.
 */
void
configureSynchronousLogging(std::ostream& output);

} // namespace ndnsf::di::detail

#endif // NDNSF_DI_DETAIL_RUNTIME_LOGGING_HPP
