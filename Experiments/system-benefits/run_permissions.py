#!/usr/bin/env python3
"""Production Controller grant/revoke and eight native User recovery barriers."""
import argparse
import base64
import fcntl
import hashlib
import json
import os
import re
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

HERE=Path(__file__).resolve().parent
REPO=HERE.parents[1]
sys.path.insert(0,str(HERE.parent/"targeted-invocation"))
from run_minindn import (seed_keys,wait_marker,stop,save,digest,ndnsec,
                         keychain_environment,issue_certificate,certificate_info,
                         prepare_trust_schema)


class BarrierTimeout(RuntimeError):
    def __init__(self,name,begin_ns,seconds,observed_ns):
        super().__init__("barrier timeout "+name)
        self.name=name
        self.record={"begin_ns":begin_ns,"deadline_ns":begin_ns+int(seconds*1e9),
                     "observed_ns":observed_ns,"budget_seconds":seconds}


def live(path):
    if not path.exists():
        return []
    text=path.read_text()
    lines=text.splitlines()
    if text and not text.endswith("\n"):
        lines=lines[:-1]
    return [json.loads(line) for line in lines]


def run(args,parser):
    from mininet.topo import Topo
    from minindn.minindn import Minindn
    from minindn.apps.app_manager import AppManager
    from minindn.apps.nfd import Nfd
    from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
    from minindn.helpers.nfdc import Nfdc
    from minindn.util import getPopen
    root=Path(args.workspace).resolve();root.mkdir(parents=True,exist_ok=True)
    lock=(root/".active.lock").open("a");fcntl.flock(lock,fcntl.LOCK_EX|fcntl.LOCK_NB)
    output=Path(args.output_dir).resolve();output.mkdir(parents=True,exist_ok=False)
    stage,trigger=root/"stage.json",root/"trigger"
    for path in (stage,trigger):path.unlink(missing_ok=True)
    seed=seed_keys(root,fixed_root=True)
    root_info=certificate_info(seed/"root.cert")
    users=["/example/hello/user/u"+str(i) for i in range(8)]
    actor_identities={"controller":"/example/hello/controller",
                      "provider":"/example/hello/provider/A",
                      **{"u"+str(i):identity for i,identity in enumerate(users)}}
    actor_certificates={}
    for label,identity in actor_identities.items():
        home=root/"actor-homes"/label;home.mkdir(mode=0o700,parents=True,exist_ok=True)
        certificate=home/"actor.cert"
        listing=ndnsec(home,["list"]).decode().split()
        if identity not in listing:
            if certificate.exists():raise RuntimeError("owned identity missing: "+label)
            if label in ("controller","provider"):
                source=seed/("gs" if label=="controller" else "drone1")
                bag=home/"own-key.safebag"
                try:
                    with bag.open("wb") as stream:
                        os.chmod(bag,0o600)
                        stream.write(ndnsec(source,["export","-P","spec204-local-bootstrap","-i",identity]))
                    ndnsec(home,["import","-P","spec204-local-bootstrap",str(bag)])
                finally:bag.unlink(missing_ok=True)
            else:
                ndnsec(home,["key-gen","-n","-t","r",identity])
        if not certificate.exists():
            if label in ("controller","provider"):
                certificate.write_bytes(ndnsec(home,["cert-dump","-i",identity]))
            else:issue_certificate(seed,home,identity,certificate)
        info=certificate_info(certificate)
        if (info["identity"]!=identity or info["signer"] not in
                (root_info["certificate"],root_info["certificate"].rsplit("/",2)[0])):
            raise RuntimeError("actor certificate is not issued by pinned root: "+label)
        if base64.b64decode(ndnsec(home,["cert-dump","-i",identity]))!=base64.b64decode(certificate.read_bytes()):
            raise RuntimeError("actor certificate differs from pin: "+label)
        actor_certificates[label]=certificate
    controller_cert=output/"controller.cert"
    shutil.copy2(actor_certificates["controller"],controller_cert)
    shutil.copy2(seed/"root.cert",output/"root.cert")
    for label,certificate in actor_certificates.items():
        shutil.copy2(certificate,output/(label+".cert"))
    def actor_home(label,identity,node):
        from ndn.encoding import Name,parse_data
        from ndn.security.keychain.keychain_sqlite3 import KeychainSqlite3
        from ndn.security.tpm.tpm_file import TpmFile
        home=root/"actor-homes"/label
        directory=home/".ndn/ndnsec-key-file/ndnsec-key-file"
        before={p.name for p in directory.glob("*.privkey")}
        own_name=parse_data(base64.b64decode(actor_certificates[label].read_bytes()))[0]
        expected={hashlib.sha256(Name.to_bytes(own_name[:-2])).hexdigest()+".privkey"}
        if before!=expected:raise RuntimeError("actor private key inventory mismatch: "+label)
        pib=KeychainSqlite3(str(home/".ndn/pib.db"),TpmFile(str(directory)))
        if label=="controller":
            # The Controller's production permission API looks up recipient
            # public certificates in its PIB; import no peer private material.
            try:
                for certificate in actor_certificates.values():
                    wire=base64.b64decode(certificate.read_bytes())
                    name,_,content,_=parse_data(wire);peer,key=name[:-4],name[:-2]
                    if peer not in pib:pib.new_identity(peer)
                    public_identity=pib[peer]
                    try:
                        if bytes(public_identity[key].key_bits)!=bytes(content):
                            raise RuntimeError("public key changed under existing name")
                    except KeyError:
                        pib.conn.execute("INSERT INTO keys (identity_id,key_name,key_bits) VALUES (?,?,?)",
                            (public_identity.row_id,Name.to_bytes(key),bytes(content)))
                        pib.conn.commit()
                    if name not in public_identity[key]:
                        pib.import_cert(Name.to_bytes(key),Name.to_bytes(name),wire)
            finally:pib.shutdown()
            expected_identities=set(actor_identities.values())
        else:pib.shutdown();expected_identities={identity}
        if before!={p.name for p in directory.glob("*.privkey")}:
            raise RuntimeError("public certificate import changed private keys: "+label)
        listing=ndnsec(home,["list"]).decode().split()
        identities=[word for word in listing if word.startswith("/")]
        if set(identities)!=expected_identities:raise RuntimeError("actor public identity inventory mismatch: "+label)
        for peer,public_identity in actor_identities.items():
            if public_identity not in expected_identities:continue
            if base64.b64decode(ndnsec(home,["cert-dump","-i",public_identity]))!=base64.b64decode(actor_certificates[peer].read_bytes()):
                raise RuntimeError("actor certificate differs from bootstrap pin: "+label)
        manifest.setdefault("actor_key_inventory",{})[label]={"private_keys":len(before),
            "owner":identity,"public_identities":sorted(expected_identities)}
        node_home=Path(node.params["params"]["homeDir"])
        shutil.copy2(node_home/".ndn/client.conf",home/".ndn/client.conf")
        return home
    services=["/HELLO"]+["/PERM/"+str(p) for p in range(1,args.permissions)]
    def policy_entry(kind,identity,allowed):
        return kind+" {\n for "+identity+"\n allow {\n"+"\n".join(allowed)+"\n }\n }\n"
    body="provider-policies {\n"+policy_entry("provider-policy","/example/hello/provider/A",services+["/EXTRA"])+"}\n"
    body+="user-policies {\n"+"".join(policy_entry("user-policy",identity,services) for identity in users)+"}\n"
    policy=output/"permissions.policies"
    policy.write_text("name /example/hello/controller/NDNSF/ControllerPolicy/spec204/"+hashlib.sha256(body.encode()).hexdigest()+"\n"+body)
    trust=output/"trust-schema.conf"
    prepare_trust_schema(seed,trust,"spec204-permissions",additional_leaves=users,
                         svs_participants=[actor_identities["provider"],*users])
    group="/example/hello/group/spec204-permissions/"+hashlib.sha256(str(time.time_ns()).encode()).hexdigest()[:16]
    topo=Topo()
    for name in ("gs","relay","drone1","drone2"):topo.addHost(name)
    for left,right in [("gs","relay"),("relay","drone1"),("relay","drone2")]:
        topo.addLink(left,right,delay="10ms",loss=0,bw=1000)
    ndn=Minindn(parser=parser,topo=topo,workDir=str(root/"nodes"))
    if ndn.args.resultDir:raise ValueError("--result-dir would move fixed node state")
    before,wall,after=time.monotonic_ns(),time.time_ns(),time.monotonic_ns()
    manifest={"schema":"ndnsf-permission204-v1","status":"PARTIAL","permissions":args.permissions,
        "seed":args.seed,"users":users,"group":group,"policy":digest(policy),"trust":digest(trust),
        "controller_certificate":digest(controller_cert),"key_scope":"each actor owns only its own private key; Controller also holds public recipient certificates; trusted harness holds root issuer",
        "ndn_trust_root":root_info,"ndn_certificates":{label:certificate_info(path) for label,path in actor_certificates.items()},
        "revalidation_ms":500,"status_validity_ms":86400000,"grace_ms":0,
        "scope":"production update/recovery only; no network baseline comparison",
        "clock_anchor":{"wall_ns":wall,"mono_ns":(before+after)//2,"uncertainty_ns":(after-before)//2},
        "children":[],"barriers":{},"capture":{}}
    sources=list(HERE.glob("permission-*.cpp"))+[HERE/"dynamic-common.hpp",HERE/"wscript",Path(__file__).resolve(),
        HERE.parent/"targeted-invocation/common.hpp",HERE.parent/"targeted-invocation/run_minindn.py"]
    manifest["sources"]={str(p):digest(p) for p in sources}
    binaries=["/usr/local/bin/ndnsf-permission-"+role for role in ("controller","provider","user")]
    manifest["binaries"]={p:digest(p) for p in binaries};manifest["libraries"]={}
    for binary in binaries:
        closure=subprocess.check_output(["ldd",binary],env=dict(os.environ,LD_LIBRARY_PATH="/usr/local/lib"),text=True)
        if "not found" in closure or str(REPO) in closure:raise RuntimeError("invalid installed closure")
        for word in closure.split():
            if word.startswith("/") and Path(word).is_file():manifest["libraries"][str(Path(word).resolve())]=digest(word)
    children=[];nfd_manager=None;controller=None;writer=None
    # Independent repetitions start with a fresh authority ledger. Reusable
    # identities and node/actor directories remain fixed; previous revocations
    # are durable evidence in their own result directory, never silently reset.
    generation=output/"controller-generation.state";generation_lock=Path(str(generation)+".lock")
    def launch(node,label,command,home=None):
        log=output/(label+".log");handle=log.open("wb")
        env={"NDN_LOG":"ndn_service_framework.*=INFO","LD_LIBRARY_PATH":"/usr/local/lib",
             "NDNSF_ENABLE_NDNSD":"0","NDNSF_POLICY_REVALIDATION_PERIOD_MS":"500"}
        if home:
            prepared=keychain_environment(home)
            env.update({key:prepared[key] for key in ("HOME","NDN_CLIENT_PIB","NDN_CLIENT_TPM")})
        if label=="controller":env["NDNSF_CONTROLLER_GENERATION_STATE"]=str(generation)
        manifest.setdefault("actor_environment",{})[label]=env
        proc=getPopen(node,command,envDict=env,stdout=handle,stderr=subprocess.STDOUT)
        children.append((label,proc,handle));return proc,log
    def barrier(name,predicate,seconds):
        start=time.monotonic_ns();deadline=time.monotonic()+seconds
        while time.monotonic()<deadline:
            for label,proc,_ in children:
                if proc.poll() is not None:raise RuntimeError("child exited at barrier "+name+": "+label)
            result=predicate()
            if result:
                manifest["barriers"][name]={"begin_ns":start,"observed_ns":time.monotonic_ns(),"evidence":result}
                save(output/"manifest.json",manifest);return result
            time.sleep(.02)
        raise BarrierTimeout(name,start,seconds,time.monotonic_ns())
    def commit(phase):
        trigger.write_text(phase+"\n")
        return barrier(phase+"-commit",lambda:next((r for r in live(output/"controller.jsonl")
            if r["event"]=="commit" and r["phase"]==phase),None),10)
    def checkpoint(phase):
        trigger.write_text("checkpoint-"+phase+"\n")
        barrier(phase+"-checkpoint",lambda:next((r for r in live(output/"controller.jsonl")
            if r["event"]=="recovery_checkpoint" and r["phase"]==phase),None),2)
    def observe_timeout(error,phase,outcome):
        manifest["outcome"]=outcome;manifest["recovery_timeout"]=error.record
        manifest["first_boundary"]=str(error)
        checkpoint(phase)
        for label,proc,_ in children:
            if label.startswith("user") and stop(proc)!=0:
                raise RuntimeError("User failed at boundary drain")
        manifest["drain_begin_ns"]=time.monotonic_ns();time.sleep(2)
        manifest["drain_end_ns"]=time.monotonic_ns()
        manifest["status"]="MEASUREMENT_COMPLETE"
    try:
        ndn.start()
        nfd_manager=AppManager(ndn,ndn.net.hosts,Nfd,logLevel="WARN")
        routing=NdnRoutingHelper(ndn.net,"udp","link-state")
        for owner,prefix in [("gs","/example/hello/controller"),("drone1","/example/hello/provider/A"),
                             ("drone2","/example/hello/user")]:
            routing.addOrigin([ndn.net[owner]],[prefix,group])
        routing.calculateRoutes()
        for node in ndn.net.hosts:
            Nfdc.setStrategy(node,"/example/hello",Nfdc.STRATEGY_BEST_ROUTE)
            Nfdc.setStrategy(node,group,Nfdc.STRATEGY_MULTICAST)
            listing=node.cmd("nfdc strategy list")
            (output/(node.name+"-routing.txt")).write_text(listing+node.cmd("nfdc fib list")+node.cmd("tc qdisc show"))
            for prefix,strategy in [("/example/hello","best-route"),(group,"multicast")]:
                matches=[line for line in listing.splitlines() if "prefix="+prefix+" " in line]
                if len(matches)!=1 or "/strategy/"+strategy+"/" not in matches[0]:raise RuntimeError("routing strategy")
        for peer in ("gs","drone1","drone2"):
            interface=ndn.net["relay"].connectionsTo(ndn.net[peer])[0][0].name
            capture,log=launch(ndn.net["relay"],"capture-"+peer,["tcpdump","-n","-U","-s","0","-i",interface,
                "-w",str(output/(peer+".pcap")),"ip"])
            wait_marker(capture,log,"listening on",5)
            manifest["capture"][peer]={"interface":interface,"pcap":peer+".pcap","started_ns":time.monotonic_ns(),"guard_ns":250000000}
        time.sleep(.25)
        controller,log=launch(ndn.net["gs"],"controller",[binaries[0],"--trust",str(trust),"--policy",str(policy),
            "--records",str(output/"controller.jsonl"),"--trigger",str(trigger),"--stage",str(stage)],
            actor_home("controller","/example/hello/controller",ndn.net["gs"]))
        wait_marker(controller,log,"SPEC204_PERMISSION_CONTROLLER_READY")
        if "NDNSF_CONTROLLER_VERSION generation=" not in log.read_text():raise RuntimeError("missing own generation marker")
        writer=generation_lock.read_bytes()
        provider,log=launch(ndn.net["drone1"],"provider",[binaries[1],"--group",group,"--trust",str(trust),
            "--controller-cert",str(controller_cert),"--records",str(output/"provider.jsonl")],
            actor_home("provider","/example/hello/provider/A",ndn.net["drone1"]))
        wait_marker(provider,log,"SPEC204_PERMISSION_PROVIDER_READY")
        for i,identity in enumerate(users):
            home=actor_home("u"+str(i),identity,ndn.net["drone2"])
            launch(ndn.net["drone2"],"user"+str(i),[binaries[2],"--group",group,"--trust",str(trust),
                "--controller-cert",str(controller_cert),
                "--identity",identity,"--permissions",str(args.permissions),"--stage",str(stage),
                "--records",str(output/("user"+str(i)+".jsonl"))],home)
        barrier("initial-eight",lambda:[i for i in range(8) if any(r["event"]=="terminal" and r["verified_success"]
            for r in live(output/("user"+str(i)+".jsonl")))] if all(any(r["event"]=="terminal" and r["verified_success"]
            for r in live(output/("user"+str(i)+".jsonl"))) for i in range(8)) else None,45)
        time.sleep(10)
        grant=commit("grant")
        try:
            barrier("grant-extra",lambda:next((r for r in live(output/"user0.jsonl") if r["event"]=="grant_extra_verified"
                and r["response_version"]==grant["global"]),None),10)
        except BarrierTimeout as error:
            if error.name!="grant-extra":raise
            observe_timeout(error,"grant","GRANT_TIMEOUT")
        else:
            checkpoint("grant");time.sleep(10)
        revoke=None if manifest.get("outcome")=="GRANT_TIMEOUT" else commit("revoke")
        def recovered():
            evidence={}
            for i in range(1,8):
                found=next((r for r in live(output/("user"+str(i)+".jsonl")) if r["event"]=="survivor_recovery_verified"
                    and r["begin_ns"]>=revoke["mono_ns"] and r["oracle"] and r["permission_matrix"]
                    and r.get("response_version")==revoke["hello"]),None)
                if not found:return None
                evidence[str(i)]=found
            denied=[r for r in live(output/"user0.jsonl") if r["event"]=="revoked_gate_denied" and r["global"]==revoke["global"]]
            if {r["service"] for r in denied}!={"/HELLO","/EXTRA"}:return None
            forbidden={r["command"] for r in denied}
            if any(r["event"]=="execution" and r["command"] in forbidden for r in live(output/"provider.jsonl")):
                raise RuntimeError("revoked denied command executed")
            if any(r["event"].startswith("unexpected_revoked_") for r in live(output/"user0.jsonl")):
                raise RuntimeError("revoked gate unexpectedly produced callback")
            evidence["revoked"]=denied;return evidence
        if revoke is not None:
            try:
                barrier("revoke-seven-and-denied",recovered,20)
            except BarrierTimeout as error:
                if error.name!="revoke-seven-and-denied":raise
                observe_timeout(error,"revoke","RECOVERY_TIMEOUT")
            else:
                manifest["outcome"]="RECOVERED";checkpoint("revoke");time.sleep(10)
                manifest["status"]="RUN_COMPLETE"
    except BaseException as error:
        manifest["first_failure"]=type(error).__name__+": "+str(error);raise
    finally:
        for label,proc,handle in reversed(children):
            try:
                code=stop(proc);manifest["children"].append({"label":label,"exit":code})
                if code:manifest["status"]="PARTIAL";manifest.setdefault("cleanup_errors",[]).append(label+" nonzero exit")
            except Exception as error:manifest["status"]="PARTIAL";manifest.setdefault("cleanup_errors",[]).append(str(error))
            handle.close()
        for peer in ("gs","drone1","drone2"):
            log=output/("capture-"+peer+".log")
            text=log.read_text() if log.exists() else ""
            found=re.search(r"(\d+) packets dropped by kernel",text)
            if not found or int(found.group(1)):
                manifest["status"]="PARTIAL";manifest.setdefault("cleanup_errors",[]).append("capture loss/unclosed: "+peer)
            else:manifest["capture"][peer]["dropped_packets"]=int(found.group(1))
        try:
            if nfd_manager:
                for app in nfd_manager:
                    if app.process:
                        try:
                            code=stop(app.process);manifest["children"].append({"label":app.node.name+"-nfd","exit":code})
                            if code:raise RuntimeError("NFD nonzero exit")
                        except Exception as error:manifest["status"]="PARTIAL";manifest.setdefault("cleanup_errors",[]).append(str(error))
            if controller and controller.poll() is not None and writer is not None and generation_lock.exists():
                if generation_lock.read_bytes()!=writer:raise RuntimeError("writer ownership changed")
                (output/"controller-writer-release.lock").write_bytes(writer);generation_lock.unlink()
            if generation.exists() and generation.parent!=output:shutil.copy2(generation,output/generation.name)
        finally:
            try:ndn.stop()
            except Exception as error:manifest["status"]="PARTIAL";manifest.setdefault("cleanup_errors",[]).append(str(error))
            finally:save(output/"manifest.json",manifest);fcntl.flock(lock,fcntl.LOCK_UN);lock.close()
    for path,expected in manifest["sources"].items():
        if digest(path)!=expected:manifest["status"]="PARTIAL";manifest.setdefault("cleanup_errors",[]).append("source drift: "+path)
    for path,expected in {**manifest["binaries"],**manifest["libraries"]}.items():
        if digest(path)!=expected:manifest["status"]="PARTIAL";manifest.setdefault("cleanup_errors",[]).append("installed candidate drift: "+path)
    for path,expected in [(policy,manifest["policy"]),(trust,manifest["trust"]),(controller_cert,manifest["controller_certificate"])]:
        if digest(path)!=expected:manifest["status"]="PARTIAL";manifest.setdefault("cleanup_errors",[]).append("configuration drift: "+str(path))
    for path,expected in [(seed/"root.cert",root_info["sha256"]),
                          *[(path,manifest["ndn_certificates"][label]["sha256"])
                            for label,path in actor_certificates.items()]]:
        if digest(path)!=expected:manifest["status"]="PARTIAL";manifest.setdefault("cleanup_errors",[]).append("certificate drift: "+str(path))
    save(output/"manifest.json",manifest)
    if manifest["status"] not in ("RUN_COMPLETE","MEASUREMENT_COMPLETE"):
        raise RuntimeError("permission measurement/cleanup incomplete")
    denied_commands={r["command"] for r in live(output/"user0.jsonl") if r["event"]=="revoked_gate_denied"}
    if any(r["event"].startswith("unexpected_revoked_") for r in live(output/"user0.jsonl")) \
       or any(r["event"]=="execution" and r["command"] in denied_commands for r in live(output/"provider.jsonl")):
        manifest["status"]="PARTIAL";manifest["first_failure"]="late revoked execution/callback"
        save(output/"manifest.json",manifest);raise RuntimeError(manifest["first_failure"])
    print("PERMISSION_CELL_COMPLETE",output,flush=True)


if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace",default="/var/tmp/ndnsf-spec204-permissions")
    parser.add_argument("--output-dir",required=True)
    parser.add_argument("--permissions",type=int,choices=(1,16),default=1)
    parser.add_argument("--seed",type=int,choices=range(20401,20406),default=20401)
    args,_=parser.parse_known_args()
    if os.geteuid()!=0:raise SystemExit("MiniNDN requires root: sudo -n python3")
    def interrupted(signum,frame):raise KeyboardInterrupt("supervisor signal "+str(signum))
    signal.signal(signal.SIGTERM,interrupted)
    run(args,parser)
