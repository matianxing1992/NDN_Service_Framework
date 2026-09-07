// Bounded native ONNX assembly worker entry (spec182 T006-C / OA03).
//
// This binary is the fixed installed worker that runNativeOnnxAssemblyWorker
// spawns for every cold certified assembly: one request frame over stdin,
// one response frame over stdout, protocol-only stdout, bounded stderr, and
// no path-command surface.  main() only dispatches into the shared library
// entry -- there is no other command, option, or service mode here.

#include "NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.hpp"

int
main(int argc, char** argv)
{
  return ndnsf::di::runNativeOnnxAssemblyWorkerMain(argc, argv);
}
