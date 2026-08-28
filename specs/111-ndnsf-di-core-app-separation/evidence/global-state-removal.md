# Ambient Assignment State Removal

Date: 2026-07-14  
Verdict: **PASS**

`rg -n 'NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE|roleProviderPreferenceFromEnv' --glob '!specs/**' --glob '!results/**' .`
found only the forbidden-string assertion in
`tests/python/test_ndnsf_di_architecture_imports.py`. Production readers and
writers: **0**. Native and Python collaboration calls receive immutable
`AssignmentContext` mappings explicitly.
