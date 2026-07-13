from __future__ import annotations
import copy,unittest
from _support import FIXTURES,load_json,load_tool

model=load_tool("spec109_model_registry")
class ModelRegistryTest(unittest.TestCase):
    def setUp(self): self.valid=load_json(FIXTURES/"model-registry/valid.json")
    def test_seals_immutable_revision_tokenizer_license_files_and_sizes(self):
        sealed=model.seal_registry_entry(self.valid)
        self.assertEqual(sealed["state"],"SEALED"); self.assertTrue(sealed["registryDigest"].startswith("sha256:"))
    def test_rejects_floating_revision_lfs_pointer_duplicate_and_size_mismatch(self):
        cases=[]
        cases.append(load_json(FIXTURES/"model-registry/invalid-floating.json"))
        lfs=copy.deepcopy(self.valid); lfs["files"][0]["lfsPointer"]=True; cases.append(lfs)
        dup=copy.deepcopy(self.valid); dup["files"].append(copy.deepcopy(dup["files"][0])); dup["sourceBytes"]+=100; cases.append(dup)
        size=copy.deepcopy(self.valid); size["sourceBytes"]+=1; cases.append(size)
        for value in cases:
            with self.subTest(value=value.get("revision")),self.assertRaises(model.ModelRegistryError): model.validate_registry_entry(value)
if __name__ == "__main__": unittest.main()
