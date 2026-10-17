#!/usr/bin/env python3
"""Freeze and supervise ten production update/recovery repetitions."""
import argparse
import json
import os
from pathlib import Path
import random
import signal
import subprocess
import sys
import time
from dynamic_campaign import candidate as dynamic_candidate, sha, save

HERE=Path(__file__).resolve().parent


def candidate():
    frozen=dynamic_candidate()
    paths=[*HERE.glob("permission-*.cpp"),HERE/"run_permissions.py",HERE/"analyze_permissions.py",
           Path(__file__).resolve(),HERE/"dynamic_campaign.py",HERE/"analyze_dynamic.py",
           HERE/"summarize_permissions.py",HERE/"analyze_credentials.py"]
    binaries=[Path("/usr/local/bin/ndnsf-permission-"+role) for role in ("controller","provider","user")]
    for binary in binaries:
        closure=subprocess.check_output(["ldd","-r",str(binary)],env=dict(os.environ,LD_LIBRARY_PATH="/usr/local/lib"),text=True,timeout=30)
        if "not found" in closure or "undefined symbol:" in closure:
            raise RuntimeError("incompatible installed permission actor: "+str(binary))
        paths.append(binary)
        paths.extend(Path(word).resolve() for word in closure.split() if word.startswith("/") and Path(word).is_file())
    frozen.update({str(path):sha(path) for path in paths});return frozen


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir",type=Path,required=True)
    parser.add_argument("--workspace",default="/var/tmp/ndnsf-spec204-permissions")
    args=parser.parse_args()
    if os.geteuid()!=0:raise SystemExit("MiniNDN campaign requires root")
    output=args.output_dir.resolve();output.mkdir(parents=True,exist_ok=False)
    cells=[{"permissions":p,"seed":s} for p in (1,16) for s in range(20401,20406)]
    random.Random(204).shuffle(cells);frozen=candidate();started=time.monotonic()
    manifest={"schema":"ndnsf-permission-campaign204-v1","status":"PARTIAL","candidate":frozen,
              "scope":"production update/recovery; no network baseline comparison","matrix":cells,"cells":[]}
    save(output/"manifest.json",manifest)
    def interrupted(signum,frame):raise KeyboardInterrupt("supervisor signal "+str(signum))
    signal.signal(signal.SIGTERM,interrupted)
    try:
        for index,cell in enumerate(cells):
            if candidate()!=frozen:raise RuntimeError("candidate changed before cell")
            label=f"c{index:02d}-p{cell['permissions']}-b{cell['seed']}";directory=output/label
            manifest["active_cell"]=label;save(output/"manifest.json",manifest)
            command=[sys.executable,str(HERE/"run_permissions.py"),"--output-dir",str(directory),
                     "--workspace",args.workspace,"--permissions",str(cell["permissions"]),"--seed",str(cell["seed"])]
            print("CELL_START",label,flush=True)
            with (output/(label+".log")).open("w") as log:
                process=subprocess.Popen(command,stdout=log,stderr=subprocess.STDOUT)
                try:code=process.wait(timeout=300)
                except BaseException:
                    process.send_signal(signal.SIGINT)
                    try:process.wait(timeout=45)
                    except subprocess.TimeoutExpired:process.kill();process.wait()
                    raise
            if code:raise RuntimeError("native cell failed: "+label)
            analysis=subprocess.run([sys.executable,str(HERE/"analyze_permissions.py"),str(directory)],
                capture_output=True,text=True,timeout=30)
            if analysis.returncode:raise RuntimeError("qualification failed: "+label+": "+analysis.stderr[-1000:])
            result=json.loads(analysis.stdout)
            if not result["measurement_complete"]:raise RuntimeError("incomplete measurement")
            if candidate()!=frozen:raise RuntimeError("candidate changed during cell")
            save(output/(label+"-summary.json"),result)
            manifest["cells"].append({**cell,"id":label,"measurement_complete":True,
                "production_qualified":result["production_qualified"],"outcome":result["outcome"],"summary":label+"-summary.json"})
            manifest["elapsed_seconds"]=time.monotonic()-started;save(output/"manifest.json",manifest)
            print("CELL_COMPLETE",len(manifest["cells"]),"/",len(cells),label,flush=True)
        manifest["status"]="CAMPAIGN_COMPLETE"
    except BaseException as error:
        manifest["first_failure"]=type(error).__name__+": "+str(error);raise
    finally:
        manifest["elapsed_seconds"]=time.monotonic()-started;save(output/"manifest.json",manifest)
    print("PERMISSION_CAMPAIGN_COMPLETE",output,flush=True)
