#!/usr/bin/env python3
"""Inject Google-Slides-visible speaker notes into the PDF-matched PPTX.

This utility adds the required PresentationML notes parts directly without
changing slide visuals. The maintained hybrid exporter supplies its current
notes explicitly; the historical NOTES list below is not its content source.
"""

from __future__ import annotations

import re
import tempfile
import zipfile
from pathlib import Path
from xml.sax.saxutils import escape
import xml.etree.ElementTree as ET


SLIDES_DIR = Path(__file__).resolve().parent
PPTX_PATH = SLIDES_DIR / "NDNSF_proposal_google_slides.pptx"
NOTES_TEX_PATH = SLIDES_DIR / "speaker_notes.tex"

REL_NS = "http://schemas.openxmlformats.org/package/2006/relationships"
CT_NS = "http://schemas.openxmlformats.org/package/2006/content-types"
NOTES_REL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesSlide"
SLIDE_REL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/slide"
NOTES_MASTER_REL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/notesMaster"
THEME_REL = "http://schemas.openxmlformats.org/officeDocument/2006/relationships/theme"


NOTES: list[str] = [
    """Good morning. My dissertation is about NDNSF: a data-centric service framework for secure, collaborative, and dynamic networked applications. The core contribution is the framework itself. UAV and distributed inference are not just two applications; they are deliberately chosen workloads that expose different requirements and help validate and refine the framework.""",
    """I will start with the idea of a service framework. In many distributed systems, a service framework is the application-layer set of protocols and libraries that turns distributed capabilities into reusable services. It defines how applications discover, authorize, invoke, and compose services. It also hides provider location and implementation details, and it manages concerns such as readiness, selection, retries, security, and data transfer. These needs appear in UAV control, edge AI inference, storage, sensing, robotics, and many other applications.""",
    """Existing IP-based service frameworks are useful, but they usually bind applications to hosts, processes, brokers, or channels. That creates problems in mobile and dynamic environments. Failover, provider selection, and data movement are often handled outside the framework. Security is also channel-centered: TLS protects a connection, but data may later be stored, forwarded, or cached. My work asks whether NDN can support a more data-centric service framework for these dynamic multi-provider applications.""",
    """This is the roadmap for the talk. I first introduce the NDN background needed for the proposal. Then I identify the missing service-layer semantics. After that I present NDNSF, including the completed foundation and the mechanisms still being refined. I then discuss two validation workloads: UAV network applications and distributed inference. Finally, I summarize the evaluation plan, remaining work, risks, and timeline.""",
    """Named Data Networking changes the narrow waist of the network. Instead of centering communication on host-addressed IP packets, NDN centers it on named, secured data chunks. Applications request data by name, and any valid producer, cache, or repository can satisfy the request. This matters for service frameworks because naming and data-centric security can reduce the dependence on fixed endpoints.""",
    """This slide shows the basic packet model. An Interest packet asks for named data. A Data packet carries a name, metadata, content, and a signature. The key point here is that NDN communication is pull-based and name-based: Interests retrieve Data by name, and the returned Data can be verified independently of where it came from. I will introduce NDNSF messages later, after the NDN background is clear.""",
    """Here is a concrete UAV example. A ground station can ask for a telemetry status name, and the drone returns signed Data containing position, battery, and health information. The same idea applies to a video frame or a segmented video object. The application names the desired data; a valid producer, cache, or repository can return signed Data with a matching name. This is the basic data-centric building block NDNSF uses.""",
    """NDN is pull-based, but distributed applications also need to learn when new names exist. Sync protocols fill that gap. State Vector Sync, or NDN-SVS, represents each participant's latest state with a compact state vector. Participants exchange these vectors to learn which names are new, and then they fetch the corresponding Data by name. In this proposal, Sync is important background because service messages are announced before they are fetched.""",
    """NDN security is attached to Data, not just to the channel that carried it. A consumer can verify producer authenticity and content integrity after retrieval. This still works through caches, repositories, and alternate paths. That is different from a purely channel-based security model. Access-control policy is a higher-layer question, and I will later explain how NDNSF uses name-based permissions and tokens on top of data-centric security.""",
    """NDN provides named, signed Data retrieval, but it does not define service-layer semantics by itself. It does not say who is authorized to call or provide a service, who is currently ready, how a user should select one or more providers, or how a large service input or output should be handled as part of an invocation. NDNSF studies this missing layer as a reusable data-centric service framework.""",
    """Related work in NDN service frameworks falls into several directions. Function-centric systems such as NFN focus on distributed computation names. Invocation-oriented systems such as RICE and NSC provide service-call semantics. Serverless and edge systems such as NFaaS and CFEC study dynamic placement. Application-specific systems such as MIA-NDN, DNMP, and SECaaS optimize for concrete domains. The gap is that no single direction provides a general-purpose, secure, collaborative NDN service framework.""",
    """The main lesson for NDNSF is that prior systems explore important pieces, but not a unified framework. Large data, long-running services, security, provider readiness, and provider selection are often treated separately. NDNSF's response is to make naming, invocation, authorization, readiness metadata, provider selection, large data input and output, and collaboration part of one framework abstraction.""",
    """This slide introduces NDNSF access control. NDNSF expresses service authorization over names rather than over fixed hosts or channels. The controller distributes encrypted permission responses to target user and provider identities. User permissions specify which named services an identity may access. Provider permissions specify which named services an identity may offer. NAC-ABE attributes then map service messages to service-level authorization namespaces.""",
    """NDNSF also uses invocation tokens. The controller-issued permissions bootstrap policy and key material, but per-request authorization is enforced during invocation. A Request carries a fresh user token. An ACK echoes that user token and returns a provider token. The Selection message must carry the selected provider token, and the Response echoes the user token. These one-time tokens separate packet validity from service authorization and help prevent replay.""",
    """This slide summarizes the dissertation components. NDNSF is the core dissertation contribution. UAV and distributed inference are application-driven workloads that stress different parts of the framework. UAV stresses mobility, control, video, and mission requirements. Distributed inference stresses artifact provisioning, generated execution plans, dependency dataflow, and large intermediate data. Feedback from both workloads validates and evolves the framework.""",
    """Here is the current status and timeline. Several pieces are already completed: the v0.1 workflow, Selective ACK, custom provider selection, Targeted invocation, local composition, large-data input and output, UAV requirements analysis, and DI planning and dataflow prototypes. The remaining work is to stabilize provider collaboration, finish UAV and DI validation, run the final evaluation matrix, and freeze the dissertation evidence from June through November 2026.""",
    """These are the research questions. RQ1 asks which host-centric service problems data-centric naming can reduce. RQ2 asks how NDNSF should support invocation, authorization, selection, collaboration, and large-object retrieval. RQ3 asks what reliability, security, and latency tradeoffs appear in multi-provider invocation. RQ4 focuses on UAV mobility and control requirements. RQ5 focuses on distributed inference as a validation workload for artifact-heavy collaboration and dependency dataflow.""",
    """NDNSF has three main entities. The controller is a startup permission authority. It authorizes users and providers, but it is not in the middle of every invocation. During invocation, a service user coordinates directly with one or more authorized providers through named Data messages. This design keeps the controller out of the hot path while still giving the user and providers the key material and permissions needed for secure service calls.""",
    """The normal invocation workflow has four logical messages: Request, ACK, Selection, and Response. The user publishes a Request. Providers that can serve it publish ACK messages. The user selects one or more providers. Selected providers return Response messages. These are carried as named, signed Data objects, and Sync announces that new messages exist. Interests then fetch each Data object by name.""",
    """A Request Message announces that a user wants a named service. It names the service, identifies the requester and request instance, carries application payload or large-data references, and includes a fresh user token for this invocation. The important point is that a service request is not just a socket call. It is a named, signed, fetchable Data object in the NDN namespace.""",
    """Selective ACK means providers do not have to ACK every request as executable. A provider checks local readiness before publishing an ACK. The ACK metadata can report load, wait time, location, model availability, safety state, or other application-specific readiness information. The user then selects among providers that are actually ready. This is the first step toward treating provider state as part of the service framework.""",
    """Provider selection is user-side, policy-driven, and performed after ACK collection. The user collects ACK candidates during an ACK window. Built-in choices include first responding, random, and all selected. Applications can also provide a custom policy using latency, load, capability, location, or safety metadata. The key point is that readiness reporting and final provider selection are separate framework steps.""",
    """This slide visualizes selection strategies. A single request can reach multiple providers, and each valid ACK becomes a candidate. First responding selects the earliest valid ACK. Random selects one valid provider. All selected invokes all valid providers. Custom selection uses metadata such as load, capability, or safety state. This is where NDNSF becomes a multi-provider coordination framework, not only a remote procedure call wrapper.""",
    """NDNSF v0.1 is already a completed foundation. It includes a dynamic C++ API, Python bindings, multi-provider ACK collection, provider selection strategies, a NAC-ABE security path, and MiniNDN evaluation. This is important for proposal defense: I am not proposing only an abstract design. The basic framework exists, and a paper describing this foundation has been submitted to ICNP.""",
    """The first v0.1 evaluation category studies network and provider failures. In a MiniNDN wired service topology, I compare NDNSF, NSC, and gRPC under one percent link loss. NDNSF and gRPC maintain full success, while NSC loses over twelve percent. In provider-failure tests, one-to-many invocation keeps success high even when providers reject or disappear with twenty to thirty percent probability. The observation is that named-data retrieval and provider diversity improve robustness.""",
    """The second v0.1 evaluation category studies mobility and availability. In wireless mobility, the provider moves in and out of a limited AP range. NDNSF reaches a high success rate because the user is not bound to one fixed endpoint. In the real-machine test, laptop and PC provider availability windows overlap for only part of the run. The result is close to the theoretical window where at least one provider is available, showing this is not just a simulator artifact.""",
    """The v0.1 results validate the basic framework idea. Multi-provider invocation improves reliability under loss, provider failure, and mobility. ACK metadata and selection improve overload handling and provider choice. The remaining question is whether these mechanisms are sufficient under real application pressure. That is why the dissertation uses UAV and distributed inference as requirement generators rather than as product demos.""",
    """The UAV workload asks whether NDNSF mechanisms can support a mobile, command-sensitive service workload. UAV systems have changing reachability, command authorization requirements, telemetry freshness requirements, video and recording objects, and multi-drone mission needs. The point is not to build a complete ground station product. The point is to use UAV requirements to discover, refine, and validate the NDNSF mechanisms needed by this class of application.""",
    """A UAV network application needs mobility support, command authorization, fresh state, large media transfer, and mission coordination. In this dissertation, the UAV workload is a requirements workload. It tells us which framework mechanisms matter: Targeted invocation for known-drone commands, local composition for same-process helpers, large-data references for video and recordings, lifecycle visibility for readiness, and collaboration status for mission recovery.""",
    """This table maps UAV needs to NDNSF features. Known drone commands motivate Targeted invocation after secure bootstrap. Same-process helpers motivate ServiceContainer and LocalServiceRegistry. Video and recordings motivate large-data references and segmented Data. Readiness and freshness motivate typed state and visible lifecycle. Mission recovery motivates collaboration status and compensation requests. These are framework features, not merely app features.""",
    """Now I turn to distributed inference. Distributed inference splits a model execution across providers. A model may be split vertically into stages, horizontally into shards, or both. The framework must plan which provider runs each segment, distribute model files and runtime artifacts, move intermediate tensors, and handle provider failures. This workload is useful because it combines computation, naming, large data, and dependency dataflow.""",
    """Distributed inference is challenging because the collaboration plan depends on several things at once: model files, runtime files, tensor dependencies, and provider capabilities. A planner must connect these into an execution plan. The framework then has to provision artifacts, express dependencies, and move large intermediate data. This is a stronger stress test than a simple request-response service because one user request creates a multi-provider dataflow.""",
    """This is the DI architecture. The user invokes a planner, and input data or model artifacts may be stored in the repository. The planner generates an execution plan that assigns model segments to providers and defines the tensor dependencies. Providers execute their assigned segments and exchange intermediate data using named references. The user receives the final result through the NDNSF service workflow. This is the full architecture, not just a simple stream.""",
    """Inference plan generation turns a model into an executable collaboration plan. For ONNX models, the planner analyzes the graph, estimates compute and activation costs, chooses candidate split points, exports model segments, and records tensor dependencies. The output is a native execution plan and service manifest. The dissertation studies how this planning can be made reusable across application workloads while still respecting model-specific constraints.""",
    """For YOLO, two split styles illustrate the tradeoff. In shared-backbone mode, one provider runs the backbone and sends larger activations to head providers. This saves compute but may transfer large tensors. In replicated-backbone mode, multiple providers run more of the model locally and exchange smaller outputs. This spends more compute but may reduce network cost. NDNSF-DI should choose based on compute cost, activation size, segment count, and RTT.""",
    """The planner decision is not just a graph cut. It should consider compute cost, activation bytes, planned segments, provider RTT, and provider capacity. The output should say which model segment runs where, which tensors cross provider boundaries, and how large those transfers are expected to be. This lets DI evaluate whether a split is actually useful instead of assuming that more distribution is always better.""",
    """DI validation asks whether NDNSF can support generated plans, repo-backed model files, dependency dataflow, and provider failures. The completed work already includes ONNX planning, repo-backed artifacts, deterministic dependency references, native execution path prototypes, and MiniNDN smoke tests. Remaining work includes planner scoring, larger-model evaluation, failure tests, and a clearer latency and data-volume analysis.""",
    """This evaluation matrix ties the research questions to evidence. NDNSF v0.1 covers the basic service framework and multi-provider invocation. UAV validation tests mobility, command, video, recording, and mission requirements. DI validation tests planning, artifact provisioning, dependency dataflow, and provider failures. The goal is to show that the framework is not only correct in isolation, but useful under two very different application pressures.""",
    """The main risks are scope, performance, and evaluation coverage. Scope risk comes from trying to build complete applications instead of framework validation workloads. I address that by keeping UAV and DI focused on requirements and evidence. Performance risk comes from large data and provider coordination overhead. I address that with MiniNDN measurements, native execution paths, and planner scoring. Evaluation risk is handled by an explicit matrix and timeline.""",
    """This backup slide gives more detail on UAV validation. The key checks are targeted command lifecycle, typed state freshness, large media objects, and mission recovery. These details support the main claim that UAV is a requirements workload. I will use this slide only if the committee asks for more detail on how the UAV application is used to validate the framework.""",
    """This backup slide gives more detail on runtime metadata evidence. Adaptive admission bounds the number of outstanding service requests before they enter the full Request-ACK-Selection-Response pipeline. Selective ACK lets overloaded providers opt out before selection. These results support the argument that ACK metadata is not just a payload field; it is a framework mechanism for runtime-aware provider choice.""",
    """This backup slide explains user-side admission control. NDNSF bounds outstanding requests locally and updates the admission window periodically. When latency is stable, the window can grow. Under rising latency, timeouts, or near-full queues, the window shrinks. The goal is not to maximize offered load blindly, but to protect the service pipeline from overload and keep tail latency under control.""",
    """This backup slide addresses NDNSF's distinct point relative to prior service work. The central difference is secure multi-provider service coordination. Prior work explores naming, function execution, remote invocation, and edge placement. NDNSF integrates provider readiness, provider selection and coordination, name-based authorization, and large data input and output into one NDN service framework.""",
    """This backup slide answers a likely security question: why not use only the NDN Data validator for authorization? Data validation proves that a packet is well formed, signed, and produced under a trusted key. Service authorization is a separate semantic question: is this user allowed to invoke this service now, and is this provider selected for this request? Tokens add replay protection and invocation-specific authorization without overloading the validator.""",
    """This backup slide answers why not just use gRPC or ROS. Those systems are useful and mature, but they are built around endpoint-oriented service calls and channel-centered security. NDNSF studies a different point in the design space: named services, data-centric security, provider diversity, in-network retrieval, and large-data workflows. The contribution is not that every application should abandon existing systems, but that NDN enables a different service framework abstraction.""",
    """These are the main references used in the talk. The most important background references are the NDN architecture paper, the NDN-SVS technical report, NAC-ABE, prior NDN service and computation systems, and related application-specific NDN systems. I keep the full reference list in backup so the main talk can focus on the dissertation argument rather than citation details.""",
]


def _next_rid(rels_xml: bytes) -> str:
    ids = [int(x) for x in re.findall(rb'Id="rId(\d+)"', rels_xml)]
    return f"rId{max(ids, default=0) + 1}"


def _ensure_relationship(rels_xml: bytes | None, rel_type: str, target: str) -> bytes:
    if rels_xml is None:
        rels_xml = (
            b'<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
            b'<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"/>'
        )
    ET.register_namespace("", REL_NS)
    root = ET.fromstring(rels_xml)
    for rel in root.findall(f"{{{REL_NS}}}Relationship"):
        if rel.get("Type") == rel_type and rel.get("Target") == target:
            return ET.tostring(root, encoding="utf-8", xml_declaration=True)
    rel = ET.SubElement(root, f"{{{REL_NS}}}Relationship")
    rel.set("Id", _next_rid(rels_xml))
    rel.set("Type", rel_type)
    rel.set("Target", target)
    return ET.tostring(root, encoding="utf-8", xml_declaration=True)


def _ensure_content_type(content_xml: bytes, part_name: str, content_type: str) -> bytes:
    ET.register_namespace("", CT_NS)
    root = ET.fromstring(content_xml)
    for override in root.findall(f"{{{CT_NS}}}Override"):
        if override.get("PartName") == part_name:
            override.set("ContentType", content_type)
            return ET.tostring(root, encoding="utf-8", xml_declaration=True)
    override = ET.SubElement(root, f"{{{CT_NS}}}Override")
    override.set("PartName", part_name)
    override.set("ContentType", content_type)
    return ET.tostring(root, encoding="utf-8", xml_declaration=True)


def _paragraph_xml(paragraph: str) -> str:
    runs = []
    for line_no, line in enumerate(paragraph.splitlines() or [""]):
        if line_no:
            runs.append("<a:br/>")
        runs.append(
            '<a:r><a:rPr lang="en-US" sz="1200"/><a:t>'
            + escape(line)
            + "</a:t></a:r>"
        )
    return "<a:p>" + "".join(runs) + '<a:endParaRPr lang="en-US" sz="1200"/></a:p>'


def _notes_slide_xml(note: str) -> bytes:
    paragraphs = [p.strip() for p in note.strip().split("\n\n") if p.strip()]
    body = "\n".join(_paragraph_xml(p) for p in paragraphs)
    xml = f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:notes xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
         xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
         xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld>
    <p:spTree>
      <p:nvGrpSpPr>
        <p:cNvPr id="1" name=""/>
        <p:cNvGrpSpPr/>
        <p:nvPr/>
      </p:nvGrpSpPr>
      <p:grpSpPr>
        <a:xfrm>
          <a:off x="0" y="0"/>
          <a:ext cx="0" cy="0"/>
          <a:chOff x="0" y="0"/>
          <a:chExt cx="0" cy="0"/>
        </a:xfrm>
      </p:grpSpPr>
      <p:sp>
        <p:nvSpPr>
          <p:cNvPr id="2" name="Notes Placeholder 1"/>
          <p:cNvSpPr><a:spLocks noGrp="1"/></p:cNvSpPr>
          <p:nvPr><p:ph type="body" idx="1"/></p:nvPr>
        </p:nvSpPr>
        <p:spPr>
          <a:xfrm>
            <a:off x="685800" y="914400"/>
            <a:ext cx="7772400" cy="5486400"/>
          </a:xfrm>
          <a:prstGeom prst="rect"><a:avLst/></a:prstGeom>
        </p:spPr>
        <p:txBody>
          <a:bodyPr wrap="square"/>
          <a:lstStyle/>
          {body}
        </p:txBody>
      </p:sp>
    </p:spTree>
  </p:cSld>
  <p:clrMapOvr><a:masterClrMapping/></p:clrMapOvr>
</p:notes>
'''
    return xml.encode("utf-8")


def _notes_rels_xml(slide_number: int) -> bytes:
    return f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="{SLIDE_REL}" Target="../slides/slide{slide_number}.xml"/>
  <Relationship Id="rId2" Type="{NOTES_MASTER_REL}" Target="../notesMasters/notesMaster1.xml"/>
</Relationships>
'''.encode("utf-8")


def _notes_master_xml() -> bytes:
    return b'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<p:notesMaster xmlns:a="http://schemas.openxmlformats.org/drawingml/2006/main"
               xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships"
               xmlns:p="http://schemas.openxmlformats.org/presentationml/2006/main">
  <p:cSld>
    <p:bg>
      <p:bgPr>
        <a:solidFill><a:schemeClr val="bg1"/></a:solidFill>
        <a:effectLst/>
      </p:bgPr>
    </p:bg>
    <p:spTree>
      <p:nvGrpSpPr><p:cNvPr id="1" name=""/><p:cNvGrpSpPr/><p:nvPr/></p:nvGrpSpPr>
      <p:grpSpPr>
        <a:xfrm><a:off x="0" y="0"/><a:ext cx="0" cy="0"/><a:chOff x="0" y="0"/><a:chExt cx="0" cy="0"/></a:xfrm>
      </p:grpSpPr>
    </p:spTree>
  </p:cSld>
  <p:clrMap bg1="lt1" tx1="dk1" bg2="lt2" tx2="dk2" accent1="accent1" accent2="accent2" accent3="accent3" accent4="accent4" accent5="accent5" accent6="accent6" hlink="hlink" folHlink="folHlink"/>
  <p:hf/>
  <p:notesStyle>
    <a:lvl1pPr algn="l"><a:defRPr sz="1200" kern="1200"/></a:lvl1pPr>
  </p:notesStyle>
</p:notesMaster>
'''


def _notes_master_rels_xml() -> bytes:
    return f'''<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="{THEME_REL}" Target="../theme/theme1.xml"/>
</Relationships>
'''.encode("utf-8")


def _plain_text_from_latex(text: str) -> str:
    text = re.sub(r"(?<!\\)%.*", "", text)
    for cmd in ["textit", "textbf", "emph", "texttt", "path"]:
        text = re.sub(rf"\\{cmd}\{{([^{{}}]*)\}}", r"\1", text)
    text = text.replace(r"\&", "&").replace(r"\%", "%").replace(r"\_", "_")
    text = text.replace("``", '"').replace("''", '"')
    text = re.sub(r"\\[a-zA-Z]+(?:\[[^\]]*\])?", "", text)
    text = text.replace("{", "").replace("}", "")
    text = re.sub(r"[ \t]+", " ", text)
    text = re.sub(r"\n{3,}", "\n\n", text)
    return text.strip()


def load_notes_from_tex(path: Path = NOTES_TEX_PATH) -> list[str]:
    if not path.exists():
        raise FileNotFoundError(path)
    source = path.read_text(encoding="utf-8")
    pattern = re.compile(
        r"\\slideentry\{[^}]+\}\{[^}]+\}\{(?P<body>.*?)\n\}",
        re.DOTALL,
    )
    notes = [_plain_text_from_latex(m.group("body")) for m in pattern.finditer(source)]
    if not notes:
        raise RuntimeError(f"No speaker notes found in {path}")
    return notes


def inject_notes(pptx_path: Path = PPTX_PATH, notes: list[str] | None = None) -> None:
    if not pptx_path.exists():
        raise FileNotFoundError(pptx_path)
    if notes is None:
        notes = load_notes_from_tex()

    with zipfile.ZipFile(pptx_path, "r") as zin:
        slide_paths = sorted(
            [
                name
                for name in zin.namelist()
                if re.fullmatch(r"ppt/slides/slide\d+\.xml", name)
            ],
            key=lambda name: int(re.search(r"slide(\d+)\.xml", name).group(1)),
        )
        if len(slide_paths) != len(notes):
            raise RuntimeError(
                f"Speaker note count ({len(notes)}) does not match slide count ({len(slide_paths)})"
            )

        modified: dict[str, bytes] = {}
        content = zin.read("[Content_Types].xml")
        content = _ensure_content_type(
            content,
            "/ppt/notesMasters/notesMaster1.xml",
            "application/vnd.openxmlformats-officedocument.presentationml.notesMaster+xml",
        )
        for i in range(1, len(notes) + 1):
            content = _ensure_content_type(
                content,
                f"/ppt/notesSlides/notesSlide{i}.xml",
                "application/vnd.openxmlformats-officedocument.presentationml.notesSlide+xml",
            )
        modified["[Content_Types].xml"] = content

        pres_rels_path = "ppt/_rels/presentation.xml.rels"
        modified[pres_rels_path] = _ensure_relationship(
            zin.read(pres_rels_path), NOTES_MASTER_REL, "notesMasters/notesMaster1.xml"
        )

        modified["ppt/notesMasters/notesMaster1.xml"] = _notes_master_xml()
        modified["ppt/notesMasters/_rels/notesMaster1.xml.rels"] = _notes_master_rels_xml()

        for i, note in enumerate(notes, start=1):
            slide_rels_path = f"ppt/slides/_rels/slide{i}.xml.rels"
            rel_xml = zin.read(slide_rels_path) if slide_rels_path in zin.namelist() else None
            modified[slide_rels_path] = _ensure_relationship(
                rel_xml, NOTES_REL, f"../notesSlides/notesSlide{i}.xml"
            )
            modified[f"ppt/notesSlides/notesSlide{i}.xml"] = _notes_slide_xml(note)
            modified[f"ppt/notesSlides/_rels/notesSlide{i}.xml.rels"] = _notes_rels_xml(i)

        with tempfile.NamedTemporaryFile(suffix=".pptx", delete=False) as tmp_file:
            tmp_path = Path(tmp_file.name)

        with zipfile.ZipFile(tmp_path, "w", zipfile.ZIP_DEFLATED) as zout:
            written = set()
            for item in zin.infolist():
                if item.filename in modified:
                    continue
                zout.writestr(item, zin.read(item.filename))
                written.add(item.filename)
            for name, data in modified.items():
                zout.writestr(name, data)
                written.add(name)

    tmp_path.replace(pptx_path)
    print(f"Injected speaker notes into {pptx_path} ({len(notes)} slides)")


def main() -> None:
    inject_notes()


if __name__ == "__main__":
    main()
