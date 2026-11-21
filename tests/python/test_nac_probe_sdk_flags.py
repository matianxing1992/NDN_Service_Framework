"""The configure-time NAC probe must honor the installed SDK's public flags."""
from pathlib import Path
from types import SimpleNamespace
import subprocess
import textwrap

ROOT=Path(__file__).resolve().parents[2]

def test_nac_probe_accepts_cmake_sdk_without_waf_private_config_header(tmp_path):
    header=tmp_path/'include/nac-abe/consumer.hpp'
    header.parent.mkdir(parents=True)
    header.write_text('''#ifndef NAC_ABE_CMAKE_BUILD
#include "nac-abe-config.hpp"
#endif
namespace ndn { namespace nacabe { struct Consumer { void consumeWithCkAuthorization(); }; } }
''')
    class Env(dict):
        def __setattr__(self,key,value):self[key]=value
        def __getattr__(self,key):return self.get(key,[])
    env=Env({'DEFINES_NAC-ABE':['NAC_ABE_CMAKE_BUILD']})
    code=(ROOT/'wscript').read_text()
    block=code.split('    conf.env.INCLUDES_NDNSF_NAC = ',1)[1].split('    nac_consumer_header = ',1)[0]
    block='    conf.env.INCLUDES_NDNSF_NAC = '+block
    exec(textwrap.dedent(block),{'conf':SimpleNamespace(env=env),'os':__import__('os'),'nac_abe_prefix':str(tmp_path)})
    probe=tmp_path/'probe.cpp'
    probe.write_text('#include <nac-abe/consumer.hpp>\nusing RequiredApi = decltype(&ndn::nacabe::Consumer::consumeWithCkAuthorization);\n')
    command=['g++-10','-std=c++17','-c',str(probe),'-o',str(tmp_path/'probe.o')]
    command+=['-I'+value for value in env.INCLUDES_NDNSF_NAC]
    command+=['-D'+value for value in env.DEFINES_NDNSF_NAC]
    result=subprocess.run(command,text=True,capture_output=True)
    assert result.returncode==0,result.stderr
