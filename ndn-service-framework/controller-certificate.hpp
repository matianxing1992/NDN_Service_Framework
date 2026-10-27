#ifndef NDNSF_CONTROLLER_CERTIFICATE_HPP
#define NDNSF_CONTROLLER_CERTIFICATE_HPP

#include <ndn-cxx/name.hpp>
#include <ndn-cxx/security/certificate.hpp>
#include <ndn-cxx/security/key-chain.hpp>
#include <ndn-cxx/util/io.hpp>
#include <ndn-cxx/util/scope.hpp>

#include <cerrno>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace ndn_service_framework {

/** Select a preprovisioned public Controller certificate without creating its identity. */
inline ndn::security::Certificate
loadControllerCertificate(ndn::security::KeyChain& keyChain, const ndn::Name& controller,
                          const char* certificateFile)
{
  ndn::security::Certificate certificate;
  if (certificateFile != nullptr && *certificateFile != '\0') {
    constexpr std::uintmax_t MAX_CERTIFICATE_FILE_BYTES = 65536;
    const int descriptor = ::open(certificateFile, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (descriptor < 0)
      throw std::runtime_error("Controller certificate file is unavailable");
    const auto closeFile = ndn::make_scope_exit([descriptor] { ::close(descriptor); });
    struct stat status;
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode))
      throw std::runtime_error("Controller certificate must be a regular file");
    if (status.st_size <= 0 ||
        static_cast<std::uintmax_t>(status.st_size) > MAX_CERTIFICATE_FILE_BYTES)
      throw std::runtime_error("Controller certificate file size is invalid");
    // Read the checked descriptor once, with one extra byte to detect growth.
    const auto size = static_cast<std::size_t>(status.st_size);
    std::string bytes(size + 1, '\0');
    std::size_t length = 0;
    while (length < bytes.size()) {
      const auto count = ::read(descriptor, bytes.data() + length, bytes.size() - length);
      if (count < 0) {
        if (errno == EINTR)
          continue;
        throw std::runtime_error("Controller certificate read failed");
      }
      if (count == 0)
        break;
      length += static_cast<std::size_t>(count);
    }
    if (length != size)
      throw std::runtime_error("Controller certificate changed size while reading");
    bytes.resize(length);
    std::istringstream input(bytes);
    const auto loaded = ndn::io::load<ndn::security::Certificate>(input);
    if (loaded == nullptr)
      throw std::runtime_error("Controller certificate file is invalid");
    certificate = *loaded;
  }
  else {
    // PIB reads can fail, but must never be replaced by createIdentity().
    certificate = keyChain.getPib().getIdentity(controller)
                    .getDefaultKey().getDefaultCertificate();
  }
  if (!certificate.isValid() || certificate.getIdentity() != controller)
    throw std::runtime_error("Controller certificate identity or validity does not match");
  return certificate;
}

} // namespace ndn_service_framework

#endif // NDNSF_CONTROLLER_CERTIFICATE_HPP
