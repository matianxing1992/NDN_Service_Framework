#ifndef NDNSF_UAV_PERSON_ONNX_HPP
#define NDNSF_UAV_PERSON_ONNX_HPP

#include <ndn-cxx/encoding/buffer.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <filesystem>
#include <array>
#include <memory>
#include <string>
#include <vector>

namespace ndnsf::examples::uav {

/** Identity of the loaded ONNX Runtime used by native person sessions. */
std::string
personOnnxRuntimeId();

/**
 * Bounded native OSNet ReID session.
 *
 * The session owns its ONNX Runtime environment and buffers.  It returns one
 * 512-d L2-normalized embedding per crop and never allocates global identity
 * state.  Callers serialize access and check authorization around each run.
 */
class UavPersonReidSession
{
public:
  static constexpr size_t FEATURE_DIMENSION = 512;
  static constexpr size_t INPUT_HEIGHT = 256;
  static constexpr size_t INPUT_WIDTH = 128;

  explicit UavPersonReidSession(const std::filesystem::path& model,
                                size_t maximumBatch = 32);
  ~UavPersonReidSession();

  UavPersonReidSession(const UavPersonReidSession&) = delete;
  UavPersonReidSession& operator=(const UavPersonReidSession&) = delete;

  /** Run one batch of CHW RGB crops; input size must be count*3*256*128. */
  std::vector<float>
  embed(const std::vector<float>& input, size_t count,
        const std::function<bool()>& stillAuthorized = {});

  size_t
  maximumBatch() const noexcept
  {
    return m_maximumBatch;
  }

  /** Frozen model bytes identity for computation manifests. */
  const std::string&
  modelDigest() const noexcept
  {
    return m_modelDigest;
  }

private:
  struct Impl;
  std::unique_ptr<Impl> m_impl;
  size_t m_maximumBatch;
  std::string m_modelDigest;
};

struct UavRgbImage
{
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgb;
};

/** Decode one admitted JPEG into owned RGB bytes with explicit bounds. */
UavRgbImage
decodePersonJpeg(const ndn::Buffer& jpeg);

struct UavLetterboxedImage
{
  std::vector<float> tensor; // CHW, 3*640*640, RGB / 255 with 114/255 padding
  double scale = 0;
  int padX = 0;
  int padY = 0;
  uint32_t sourceWidth = 0;
  uint32_t sourceHeight = 0;
};

/** Reproduce the frozen detector letterbox: bilinear resize, centered 114 pad. */
UavLetterboxedImage
letterboxPersonImage(const UavRgbImage& image);

struct UavRectLetterboxedImage
{
  UavLetterboxedImage image;
  int tensorWidth = 0;
  int tensorHeight = 0;
};

/** Resize within 640 pixels and pad each axis only to the next YOLO stride of 32. */
UavRectLetterboxedImage
letterboxRectangularImage(const UavRgbImage& image);

/** One person detection in detector input coordinates (letterboxed pixels). */
struct UavPersonBoxDetection
{
  std::array<float, 4> box{}; // x1,y1,x2,y2
  float confidence = 0;
};

/** One raw-head detection in letterboxed pixel coordinates. */
struct UavVehicleBoxDetection
{
  std::array<float, 4> box{};
  float confidence = 0;
  size_t classId = 0;
};

/**
 * Bounded native YOLO person detector over a preprocessed letterboxed tensor.
 * JPEG decoding and letterboxing stay outside so the caller can prove the
 * exact snapshot-to-tensor boundary.
 */
class UavPersonDetectorSession
{
public:
  static constexpr size_t INPUT_SIZE = 640;
  static constexpr size_t PERSON_CLASS = 0;
  static constexpr size_t MAX_DETECTIONS = 300;

  enum class ModelFormat { PERSON_RAW, PERSON_END_TO_END, VEHICLE_RAW };

  explicit UavPersonDetectorSession(const std::filesystem::path& model,
                                    float confidenceThreshold = 0.25F,
                                    float nmsIou = 0.7F,
                                    size_t maximumDetections = MAX_DETECTIONS);
  UavPersonDetectorSession(const std::filesystem::path& model, float confidenceThreshold,
                           float nmsIou, size_t maximumDetections, ModelFormat format);
  ~UavPersonDetectorSession();

  UavPersonDetectorSession(const UavPersonDetectorSession&) = delete;
  UavPersonDetectorSession& operator=(const UavPersonDetectorSession&) = delete;

  /** Input must be one CHW RGB float tensor of 3*640*640 values. */
  std::vector<UavPersonBoxDetection>
  detect(const std::vector<float>& input,
         const std::function<bool()>& stillAuthorized = {});

  std::vector<UavPersonBoxDetection>
  detect(const UavRectLetterboxedImage& input,
         const std::function<bool()>& stillAuthorized = {});

  const std::string&
  modelDigest() const noexcept
  {
    return m_modelDigest;
  }

private:
  friend class UavVehicleDetectorSession;
  std::vector<UavVehicleBoxDetection>
  detectBoxes(const std::vector<float>& input, size_t width, size_t height,
              const std::function<bool()>& stillAuthorized);

  std::vector<UavPersonBoxDetection>
  detectPersons(const std::vector<float>& input, size_t width, size_t height,
                const std::function<bool()>& stillAuthorized);

  struct Impl;
  std::unique_ptr<Impl> m_impl;
  float m_confidenceThreshold;
  float m_nmsIou;
  size_t m_maximumDetections;
  std::string m_modelDigest;
};

/** Native raw-head YOLO detector for rectangular vehicle tracking inputs. */
class UavVehicleDetectorSession
{
public:
  explicit UavVehicleDetectorSession(const std::filesystem::path& model,
                                     float confidenceThreshold);

  /** Decode the raw head, taking the best class per anchor and applying class-aware NMS. */
  std::vector<UavVehicleBoxDetection>
  detect(const UavRectLetterboxedImage& input,
         const std::function<bool()>& stillAuthorized = {});

  const std::vector<std::string>& modelClassNames() const noexcept;

private:
  UavPersonDetectorSession m_detector;
};

} // namespace ndnsf::examples::uav

#endif // NDNSF_UAV_PERSON_ONNX_HPP
