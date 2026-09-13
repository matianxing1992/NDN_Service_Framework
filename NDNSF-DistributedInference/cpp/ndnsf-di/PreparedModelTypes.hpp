#ifndef NDNSF_DI_PREPARED_MODEL_TYPES_HPP
#define NDNSF_DI_PREPARED_MODEL_TYPES_HPP

#include <string>
#include <vector>

namespace ndnsf::di {

/** Read-only capabilities derived from the verified adapter and task. */
struct ModelCapabilities
{
  std::string inputSchemaJson;
  std::string outputSchemaJson;
  std::vector<std::string> inputKinds;
  std::vector<std::string> outputModes;
  bool streaming = false;
  bool conversations = false;
};

/** Stable identity view of a verified package. */
struct ModelManifest
{
  std::string modelName;
  std::string modelRevision;
  std::string modelDigest;
  std::string taskName;
  std::string canonicalGraphDigest;
  std::string planningGraphDigest;
  std::string catalogConfigurationDigest;
  std::string taskContractDigest;
  std::string preparationKeyDigest;
};

} // namespace ndnsf::di

#endif // NDNSF_DI_PREPARED_MODEL_TYPES_HPP
