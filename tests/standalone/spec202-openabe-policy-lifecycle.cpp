#include <openabe/openabe.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

void
require(bool condition, const char* message)
{
  if (!condition) {
    throw std::runtime_error(message);
  }
}

class CountedObject final : public oabe::ZObject {
public:
  explicit CountedObject(unsigned& destructions) : m_destructions(destructions) {}
  ~CountedObject() override { ++m_destructions; }

private:
  unsigned& m_destructions;
};

void
checkRelease(unsigned additionalReferences)
{
  unsigned destructions = 0;
  auto* object = new CountedObject(destructions);
  require(object->getRefCount() == 1, "initial reference count is not one");
  for (unsigned index = 0; index < additionalReferences; ++index) {
    object->addRef();
  }
  require(object->getRefCount() == additionalReferences + 1, "addRef lost a reference");
  for (unsigned remaining = additionalReferences; remaining > 0; --remaining) {
    object->deRef();
    require(destructions == 0, "object destroyed before its final release");
    require(object->getRefCount() == remaining, "release count is incorrect");
  }
  object->deRef();
  // The old library leaves a live object with count zero. Clean up that RED
  // path, but never dereference or delete the object after a real destructor.
  if (destructions == 0) {
    delete object;
    throw std::runtime_error("final owned reference did not destroy the object");
  }
  require(destructions == 1, "final release must destroy exactly once");
}

void
checkLeaf(oabe::OpenABETreeNode* node, const char* label)
{
  require(node && node->getNodeType() == oabe::GATE_TYPE_LEAF &&
          node->getNumSubnodes() == 0 && node->getLabel() == label,
          "policy leaf differs from literal expected label");
}

void
checkGate(oabe::OpenABETreeNode* node, oabe::zGateType type)
{
  require(node && node->getNodeType() == type && node->getNumSubnodes() == 2,
          "policy gate differs from literal expected structure");
}

void
checkPolicy(oabe::OpenABEPolicy& policy, const std::string& input)
{
  auto* root = policy.getRootNode();
  if (input == "A") {
    checkLeaf(root, "A");
  }
  else if (input == "A and B" || input == "A or B") {
    checkGate(root, input == "A and B" ? oabe::GATE_TYPE_AND : oabe::GATE_TYPE_OR);
    checkLeaf(root->getSubnode(0), "A");
    checkLeaf(root->getSubnode(1), "B");
  }
  else {
    checkGate(root, oabe::GATE_TYPE_OR);
    checkGate(root->getSubnode(0), oabe::GATE_TYPE_AND);
    checkGate(root->getSubnode(1), oabe::GATE_TYPE_AND);
    checkLeaf(root->getSubnode(0)->getSubnode(0), "A");
    checkLeaf(root->getSubnode(0)->getSubnode(1), "B");
    checkLeaf(root->getSubnode(1)->getSubnode(0), "C");
    checkLeaf(root->getSubnode(1)->getSubnode(1), "D");
  }
}

} // namespace

int
main()
{
  checkRelease(0);
  checkRelease(2);
  for (const auto* input : {"A", "A and B", "A or B", "(A and B) or (C and D)"}) {
    auto policy = oabe::createPolicyTree(input);
    if (!policy || !policy->getRootNode() || policy->toCompactString() != input) {
      throw std::runtime_error("policy parse changed the nominal input");
    }
    std::unique_ptr<oabe::OpenABEPolicy> cloned(policy->clone());
    if (!cloned || cloned->toCompactString() != policy->toCompactString()) {
      throw std::runtime_error("policy clone lost its identity");
    }
    checkPolicy(*policy, input);
    checkPolicy(*cloned, input);
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
