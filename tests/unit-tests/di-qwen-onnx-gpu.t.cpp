#include "NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp"

#include <boost/test/unit_test.hpp>

#include <functional>
#include <numeric>

namespace ndnsf::di::tests {

BOOST_AUTO_TEST_SUITE(DiQwenOnnxGpu)

BOOST_AUTO_TEST_CASE(Float16HiddenAndKvTensorContractsRoundTripWithoutConversion)
{
  NamedTensor hidden{"hidden_states", TensorElementType::Float16, {1, 2, 4},
                     std::vector<std::uint8_t>(16, 0x2a)};
  NamedTensor key{"past_key_values.0.key", TensorElementType::Float16,
                  {1, 2, 2, 4}, std::vector<std::uint8_t>(32, 0x17)};
  NamedTensor value{"past_key_values.0.value", TensorElementType::Float16,
                    {1, 2, 2, 4}, std::vector<std::uint8_t>(32, 0x23)};
  const auto encoded = encodeTensorBundle({hidden, key, value});
  const auto decoded = decodeTensorBundle(encoded);
  BOOST_REQUIRE_EQUAL(decoded.size(), 3);
  for (const auto& tensor : decoded) {
    BOOST_CHECK(tensor.elementType == TensorElementType::Float16);
    BOOST_CHECK_EQUAL(tensor.payload.size(),
                      tensorElementByteSize(tensor.elementType) *
                        static_cast<std::size_t>(std::accumulate(
                          tensor.shape.begin(), tensor.shape.end(), std::int64_t{1},
                          std::multiplies<std::int64_t>())));
  }
  const auto kv = selectTensors(decoded,
                                {"past_key_values.0.key", "past_key_values.0.value"});
  BOOST_REQUIRE_EQUAL(kv.size(), 2);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace ndnsf::di::tests
