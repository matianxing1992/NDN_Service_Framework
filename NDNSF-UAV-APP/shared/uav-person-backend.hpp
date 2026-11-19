#ifndef NDNSF_UAV_PERSON_BACKEND_HPP
#define NDNSF_UAV_PERSON_BACKEND_HPP

#include "uav-person-tracking-profile.hpp"

#include <ndn-cxx/encoding/buffer.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace ndnsf::examples::uav {

struct UavPersonWindowContext;

/**
 * Native detector/ByteTrack/ReID backend for one three-camera window.
 *
 * The backend owns model sessions and bounded per-window tracker state only.
 * It returns the exact `uav-person-backend-v1` JSON consumed by the existing
 * engine parser; global identity and commit decisions stay outside.
 */
class UavPersonModelBackend
{
public:
  UavPersonModelBackend(const UavPersonTrackingProfile& profile,
                        const std::filesystem::path& detectorModel,
                        const std::filesystem::path& reidModel,
                        size_t reidBatchSize = 32);
  ~UavPersonModelBackend();

  UavPersonModelBackend(const UavPersonModelBackend&) = delete;
  UavPersonModelBackend& operator=(const UavPersonModelBackend&) = delete;

  std::string
  processWindow(const UavPersonWindowContext& context,
                const std::array<ndn::Buffer, 3>& admittedJpegs,
                const std::string& committedBackendStateJson,
                const std::function<bool()>& stillAuthorized);

  const std::string&
  detectorDigest() const noexcept;
  const std::string&
  reidDigest() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_UAV_PERSON_BACKEND_HPP
