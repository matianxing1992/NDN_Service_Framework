// Exercise the same runner regression cases against installed production libraries.
#define BOOST_TEST_DYN_LINK
#define BOOST_TEST_MODULE ndnsf_installed_onnx_session
#include "tests/unit-tests/di-onnxruntime-gpu-evidence.t.cpp"

BOOST_AUTO_TEST_CASE(CudaMemoryBindingOnly, *boost::unit_test::disabled())
{
  const auto* enabled = std::getenv("NDNSF_TEST_FIRST_CUDA_SESSION");
  BOOST_REQUIRE(enabled && std::string(enabled) == "1");
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    ndnsf::di::CudaDeviceMemoryQuery binding(0);
    binding.prepare();
    const auto observed = binding.query(1, attempt + 1, 0);
    BOOST_CHECK_GT(observed.totalMemoryMb, 0);
  }
}
