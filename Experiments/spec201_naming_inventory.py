"""Source-backed publication inventory used by the Spec201 admission gate.

Embedded/local/library rows retain their existing carrier. This inventory does
not add a publisher or replace each family's production parser.
"""
from pathlib import Path


def naming_inventory(root: Path):
    core = "ndn-service-framework/"
    native = "NDNSF-DistributedInference/cpp/ndnsf-di/"
    repo = "NDNSF-DistributedRepo/"
    result = {f"C{i:02d}": [] for i in range(2, 11)}
    result["C12"] = []

    def add(family, rows, writer, consumer, validator, owner, target, files,
            publication="independent", reason=""):
        for row in rows:
            result[family].append(dict(row=row, writer=writer, consumer=consumer,
                validator=validator, referenceOwner=owner, target=target,
                sourceFiles=[str((root / path).resolve(strict=True)) for path in files],
                publication=publication, reason=reason))

    content_files = [native + "NativeCanonicalArtifactPublisher.cpp",
                     native + "NativeCanonicalOnnxAssembler.cpp",
                     core + "ServiceUser.cpp", core + "ServiceProvider.cpp",
                     core + "PublishedContentName.cpp", core + "ProtectedBlobManifest.cpp",
                     core + "ProducerSignerPolicy.cpp"]
    add("C02", ["Model root manifest", "Layer / pre-split / Qwen stage / derived artifact manifest",
                "Material manifest", "Publish / materialize / assembly receipt",
                "完整模型文件", "层、预切片、Qwen stage、派生模型文件", "weights / initializer / tokenizer",
                "material bundle", "输入文件或数据", "确实没有更具体业务类别的通用大对象"],
        "NativeCanonicalArtifactPublisher::publicationContentName/publicationManifestSuffix; ServiceUser::publishEncryptedLargeDataImpl",
        "NativeCanonicalOnnxAssembler; ServiceProvider::fetchAndDecryptLargeDataUntil",
        "PublishedContentName; ProtectedBlobManifest::validate; ProducerSignerPolicy",
        "CanonicalPreparationReceipt/publication lease/original packet owner",
        "spec201-original-packet-publication", content_files)
    add("C02", ["Fixed assignment logical prefix A"],
        "NativeInferenceClient; nativeFixedRoleSourceContentName", "NativeExecutionPlanJson fixed-role parser",
        "PublishedContentName/ProtectedBlobManifest", "NativeFixedRoleAssignmentCache",
        "spec181-native-plan-closure", [native + name for name in
        ("NativeInferenceClient.cpp", "NativeExecutionPlanJson.cpp", "NativeExecutionPlanJson.hpp")])
    add("C02", ["Root/artifact/material manifest page, only if independently published"],
        "network_artifact_backend::_PreparedArtifactSource._prepare_manifest/start; FileSegmentedObjectProducer",
        "_NetworkPublishDriver/source_page_name/artifact manifest page parser",
        "artifact_manifest_page_from_dict/canonical_manifest_page_bytes/original packet validation",
        "original page source/ImmutableFilePublication",
        "spec201-transparent-repo", [repo + "pythonWrapper/py_repoclient/network_artifact_backend.py",
        "pythonWrapper/ndnsf/service.py", "pythonWrapper/src/ndnsf/_ndnsf.cpp",
        core + "ImmutableFilePublication.hpp", core + "PublishedContentName.cpp"],
        "independent", "Existing raw semantic material/artifact-page representation and standard segmented transport.")
    add("C02", ["Model alias snapshot",
                "Independently published plan logical prefix A"],
        "N/A: no independent writer", "existing metadata/preparation consumer",
        "existing descriptor admission", "canonical preparation metadata",
        "spec201-static-publication", content_files, "local",
        "Current aliases/plans are metadata or embedded; no separate Data publisher.")
    messages = [core + name for name in ("ServiceUser.cpp", "ServiceProvider.cpp", "utils.cpp")]
    add("C03", ["Input descriptor", "Current invocation context", "REQUEST", "ACK / offer", "SELECTION", "RESPONSE"],
        "ServiceUser/ServiceProvider paper message publishers", "pending invocation handlers",
        "contextual make/parse NameV2 and authenticated request projection",
        "PreparedRequest/selection/current invocation", "spec201-invocation-names", messages)
    add("C03", ["Independent readiness report", "Execution-activate decision record", "Selection/execution status snapshot"],
        "Provider-ready/activation/status signed RPC replies", "User/Provider control callbacks",
        "utils contextual RPC parser/current authorization", "prepared control Interest",
        "spec201-control-authorization", messages, "embedded", "Existing signed RPC/control reply carrier.")
    tensor = [native + name for name in ("NativeExecutionPlan.cpp", "NdnsfCollaborationDependencyIo.cpp",
                                         "ProviderGroupCoordinator.cpp")]
    add("C04", ["Tensor manifest / segment", "Collective member contribution / result", "External result / large COLLAB payload"],
        "tensorObjectNamePrefix/ManifestName/SegmentName; publishOwnedExactData", "dependency prefetch/fetch",
        "sealed endpoint/context/verifyTensorObjectManifest/acceptAndOpenSegment",
        "packet-group/prefetch/collective endpoint owner", "spec201-collective-names", tensor)
    add("C04", ["Collective coordination manifest, only if independently published", "Final result descriptor"],
        "N/A independent writer; existing collective/result carrier", "coordinator/requester result consumer",
        "collective capability/result binding", "sealed collective/result descriptor",
        "spec201-collective-names", tensor, "embedded", "Descriptors are embedded in existing operations/results.")
    add("C05", ["Core invocation application / progress / terminal event"],
        "InvocationStream::makeInvocationEventName/event publisher", "invocation stream reader",
        "contextual event parser/StreamBinding", "InvocationStream lifecycle",
        "spec201-event-conversation", [core + "InvocationStream.cpp"])
    conversation = [native + "NativeInferenceClient.cpp", native + "NativeProviderHandler.hpp"]
    add("C05", ["Conversation ready", "State descriptor, only if independently published", "Receipt",
                "Commit decision", "Commit ACK", "Control / lease record"],
        "publishConversationControls/existing encrypted COLLAB", "native conversation control handler",
        "conversation binding/current control authorization", "conversation transaction/local journal",
        "spec201-event-conversation", conversation, "embedded", "No independent conversation record publication.")
    probes = [core + "PeerProbeNaming.cpp", core + "ServiceProvider.cpp", core + "ServiceUser.cpp"]
    add("C06", ["Existing signed RTT / readiness request-reply", "Existing signed dedicated goodput request-reply"],
        "Provider peer-probe handlers/makePeerProbeRpcName", "requester probe callbacks",
        "matchesPeerProbeRpcName/signature/target/scope", "native probe run/cache",
        "spec201-probe-names", probes)
    add("C06", ["Direct goodput manifest, only for an already-established run-serving API",
                "Direct dedicated goodput packet, same conditional run-serving API",
                "Measurement snapshot, only if independently published"],
        "N/A independent writer; signed RPC packet train", "native probe consumer",
        "signed probe parameter/scope validation", "probe run/cache",
        "spec201-probe-names", probes, "embedded", "No separate run-serving publication API.")
    stream = [core + "Stream.cpp", core + "StreamFacade.cpp", core + "utils.cpp"]
    add("C07", ["Stream descriptor", "Map block", "Generic stream transport packet",
                "Existing predictive reservation content packet", "Existing ordinary semantic sample packet",
                "Predictive FEC group commit descriptor", "Predictive repair packet", "Produced/committed frontier snapshot",
                "COLLAB / SVS business inner / outer / mapping"],
        "LiveStreamPublisher; makeStreamNameMapBlockName/makePredictiveDataName/makePredictiveGroupManifestName/makePredictiveRepairName/makeFrontierSnapshotName; collaboration publishers",
        "stream/predictive/COLLAB readers", "map/group/FEC/frontier/context and inner signer validation",
        "stream sessionEpoch/map/packet/FEC owner", "spec201-stream-transport", stream + messages)
    add("C07", ["Logical mapping root M, not a Data packet", "Independently published immutable map descriptor, only if already used"],
        "N/A independent writer", "existing map reference consumer", "map/session contract",
        "stream mapping owner", "spec201-stream-transport", stream, "local", "Logical root; no additional descriptor publisher.")
    control = messages + [core + "ServiceController.cpp", core + "HybridMessageCrypto.cpp"]
    add("C08", ["Recipient grant / role/group capability when independently published", "Hybrid key envelope",
                "Policy status", "User / Provider permissions", "Service access / provision authorization record"],
        "NativeGrantVerifier::canonicalNativeGrantName; hybrid key publishers; Controller RPC handlers",
        "NativeGrantClient/ProtectedRuntime/User/Provider control consumer",
        "grant binding/current authorization/contextual RPC/hybrid crypto",
        "request/provider/model/epoch scoped key and Controller permission state",
        "spec201-control-authorization", control + [native + "NativeGrantVerifier.cpp", native + "ProtectedRuntime.cpp"])
    deployment = "NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/deployment.py"
    add("C08", ["App deployment definition", "App activation record", "App revocation record"],
        "deployment.publish_definition/publish_activation/revoke_activation; publishSignedAppData",
        "deployment exact-name/envelope consumer", "makeDeploymentRecordName/revision binding",
        "deployment revision owner", "spec201-control-authorization", [deployment, core + "utils.cpp", core + "ServiceUser.cpp"])
    add("C08", ["Policy manifest"],
        "N/A standalone writer; Controller configured state/revocation RPC",
        "User/Provider policy/revocation consumer", "Controller generation/epoch authorization",
        "Controller policy state", "spec201-control-authorization", control + [deployment],
        "embedded", "Policy manifest remains configured state or the existing signed RPC carrier.")
    repository = [repo + "src/RepoCore.cpp", repo + "src/RepoNode.cpp", repo + "src/RepoClient.cpp",
                  repo + "src/RepoPacketServer.cpp", repo + "src/backends/FilesystemRepoStoreBackend.cpp"]
    add("C09", ["Original stored Data", "Inventory / capability / cache status / operation status", "Catalog snapshot", "Catalog delta"],
        "original Producer packet; RepoCore::putDataPacket/RepoNode::makeResponse",
        "RepoClient/original packet server", "original wire/name/index and paper RESPONSE validation",
        "Repo packet index/backend/catalog operation state", "spec201-transparent-repo", repository,
        "original-and-embedded", "Stored wire unchanged; management payloads use Repo-owned paper RESPONSE.")
    add("C10", ["Certificates / NAC", "Local IDs/cache keys", "ndn-svs Sync/StateVector"],
        "CertificatePublisher/CertificateBootstrap/Controller; standard library; N/A local IDs",
        "standard credential/protocol consumer", "issuer/subject delegated namespace authorization",
        "original certificate/NAC/library owner", "spec201-control-authorization",
        [core + "CertificatePublisher.cpp", core + "CertificateBootstrap.cpp", core + "ServiceController.cpp"],
        "standard-or-local", "Preserve library grammar; no blanket namespace exemption.")
    add("C12", ["Sync multicast / producer Data best-route"],
        "both actual model launcher prefix-role wiring", "NFD strategy/FIB readback",
        "prefix_role_plan/configure_prefix_roles longest match and shadow checks",
        "frozen actors/groups/data roots/selected SVS protocol", "spec201-candidate-closure",
        ["Experiments/native_prefix_roles.py", "Experiments/NDNSF_DI_Yolo2x2_Minindn.py",
         "Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py"])
    return result
