#include <openabe/openabe.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

int
main()
{
  for (const auto* input : {"A", "A and B", "(A and B) or (C and D)"}) {
    auto policy = oabe::createPolicyTree(input);
    if (!policy || !policy->getRootNode() || policy->toCompactString() != input) {
      throw std::runtime_error("policy parse changed the nominal input");
    }
    std::unique_ptr<oabe::OpenABEPolicy> cloned(policy->clone());
    if (!cloned || cloned->toCompactString() != policy->toCompactString()) {
      throw std::runtime_error("policy clone lost its identity");
    }
    auto* root = policy->getRootNode();
    for (uint32_t index = 0; index < root->getNumSubnodes(); ++index) {
      std::cout << "policy=" << input << " child=" << index
                << " references=" << root->getSubnode(index)->getRefCount() << '\n';
    }
  }
  if (oabe::createPolicyTree("")) {
    throw std::runtime_error("empty policy unexpectedly parsed");
  }
  std::cout << "SPEC202_OPENABE_POLICY_LIFECYCLE_ASSERTIONS_PASS" << std::endl;
  return 0;
}
