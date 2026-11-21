"""The maintained OpenABE recipe must generate public headers before compilation."""
from pathlib import Path
import os
import subprocess

ROOT=Path(__file__).resolve().parents[2]

def test_scanner_header_exists_before_parallel_source_build(tmp_path):
    source=tmp_path/'openabe'
    (source/'src').mkdir(parents=True)
    (source/'env').write_text('export ZROOT="$PWD"\n')
    (source/'src/zparser.yy').write_text('%language "c++"\n%defines\n%token NAME\n%%\nstart: NAME ;\n')
    (source/'src/Makefile').write_text('header:\n\tmkdir -p ../root/include/openabe\n')
    (source/'Makefile').write_text('''.PHONY: clean deps src examples
clean:
\trm -f root/include/openabe/zparser.tab.hh src/zparser.tab.hh
deps:
\t@true
src:
\ttest -s root/include/openabe/zparser.tab.hh
\t@echo SCANNER_HEADER_READY
examples:
\t@true
''')
    installer=(ROOT/'install_ndnsf_stack.sh').read_text()
    recipe=installer.split("<<'OPENABE_BUILD'\n",1)[1].split('\nOPENABE_BUILD',1)[0]
    result=subprocess.run(['bash','-e','-s','--',str(source),'/usr/bin:/bin','28'],input=recipe,
                          text=True,capture_output=True,env={**os.environ,'TMPDIR':str(tmp_path)})
    assert result.returncode==0,result.stdout+result.stderr
    assert 'SCANNER_HEADER_READY' in result.stdout
