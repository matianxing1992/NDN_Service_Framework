#include <ndn-service-framework/ProducerDataNaming.hpp>
#include <iostream>

int main()
{
  using namespace ndn_service_framework;
  const auto digest = computeNamingDigest("ndnsf-naming/installed", {{1, 4, {0, 255}}});
  const ndn::Name producer("/installed/NDNSF/MODEL/producer");
  const auto name = makeProducerDataPrefix(producer, "MODEL")
    .append(makeNamingDigestComponent(digest)).append("MANIFEST");
  const auto parsed = parseProducerDataName(name, producer);
  if (!parsed || parsed->producer != producer || parsed->category != "MODEL") return 1;
  checkNamingNameSize(name);
  const auto semantic = makeProducerDataPrefix(producer, "NDNSF-DI", "model")
    .append(makeNamingLabelComponent("qwen2.5-0.5b"))
    .append(makeNamingDigestComponent(digest));
  const auto semanticParsed = parseProducerDataName(semantic, producer);
  if (!semanticParsed || semanticParsed->nameSpace != "NDNSF-DI" ||
      semanticParsed->category != "model" ||
      !matchesNamingLabelComponent(semanticParsed->suffix[0], "qwen2.5-0.5b")) return 2;
  std::cout << "SPEC201_INSTALLED_HEADER_ABI_PASS " << digest << std::endl;
  return 0;
}
