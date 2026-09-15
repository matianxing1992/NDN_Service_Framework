# Python 原生绑定映射

从 pybind11 绑定声明静态提取 Python 名称、C++ 目标或 lambda 参数、py::arg 默认值与调用策略。lambda 实现体省略；构造器 py::init 与动态导出须另查源文件。此表不导入扩展，不声明 ABI 或运行通过。

## pythonWrapper/src/ndnsf/_ndnsf.cpp

SHA-256：`3753d48efeb3256310d563d512e6ec4fc3ebd84b327e8e521a7edf804c776cd9`。

### root · "schema"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L4345)

```cpp
value("schema",
std::string{})
```

### see-source-owner · "decode"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6212)

```cpp
def_static("decode",
[] (const py::bytes& wire) { implementation omitted },
py::arg("wire"))
```

### see-source-owner · "wire_encode"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6209)

```cpp
def("wire_encode",
[] (const T& value) { implementation omitted })
```

### see-source-owner · "digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6208)

```cpp
def("digest",
&T::computeDigest)
```

### see-source-owner · "fields"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6205)

```cpp
def_property_readonly("fields",
[] (const T& value) { implementation omitted })
```

### see-source-owner · "get_field"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6203)

```cpp
def("get_field",
&T::getField,
py::arg("name"),
py::return_value_policy::copy)
```

### see-source-owner · "has_field"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6202)

```cpp
def("has_field",
&T::hasField,
py::arg("name"))
```

### see-source-owner · "set_field"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6201)

```cpp
def("set_field",
&T::setField,
py::arg("name"),
py::arg("value"))
```

### see-source-owner · "version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6200)

```cpp
def_property("version",
&T::getVersion,
&T::setVersion)
```

### m · "make_opaque_control_handle"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6245)

```cpp
def("make_opaque_control_handle",
&nsf::makeOpaqueControlHandle,
py::arg("bytes") = 24)
```

### m · "is_valid_opaque_control_handle"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6247)

```cpp
def("is_valid_opaque_control_handle",
&nsf::isValidOpaqueControlHandle,
py::arg("handle"))
```

### NativeStreamFecInfo · "enabled"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6261)

```cpp
def_property_readonly("enabled",
&nsf::StreamFecInfo::enabled)
```

### NativeStreamFecInfo · "metadata"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6260)

```cpp
def_readwrite("metadata",
&nsf::StreamFecInfo::metadata)
```

### NativeStreamFecInfo · "repair_symbol"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6259)

```cpp
def_readwrite("repair_symbol",
&nsf::StreamFecInfo::repairSymbol)
```

### NativeStreamFecInfo · "source_block_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6258)

```cpp
def_readwrite("source_block_id",
&nsf::StreamFecInfo::sourceBlockId)
```

### NativeStreamFecInfo · "data_lengths"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6257)

```cpp
def_readwrite("data_lengths",
&nsf::StreamFecInfo::dataLengths)
```

### NativeStreamFecInfo · "symbol_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6256)

```cpp
def_readwrite("symbol_count",
&nsf::StreamFecInfo::symbolCount)
```

### NativeStreamFecInfo · "symbol_index"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6255)

```cpp
def_readwrite("symbol_index",
&nsf::StreamFecInfo::symbolIndex)
```

### NativeStreamFecInfo · "parity_shards"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6254)

```cpp
def_readwrite("parity_shards",
&nsf::StreamFecInfo::parityShards)
```

### NativeStreamFecInfo · "data_shards"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6253)

```cpp
def_readwrite("data_shards",
&nsf::StreamFecInfo::dataShards)
```

### NativeStreamFecInfo · "scheme"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6252)

```cpp
def_readwrite("scheme",
&nsf::StreamFecInfo::scheme)
```

### NativeStreamChunk · "metadata"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6288)

```cpp
def_readwrite("metadata",
&nsf::StreamChunk::metadata)
```

### NativeStreamChunk · "fec"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6287)

```cpp
def_readwrite("fec",
&nsf::StreamChunk::fec)
```

### NativeStreamChunk · "segment_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6286)

```cpp
def_readwrite("segment_count",
&nsf::StreamChunk::segmentCount)
```

### NativeStreamChunk · "segment_index"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6285)

```cpp
def_readwrite("segment_index",
&nsf::StreamChunk::segmentIndex)
```

### NativeStreamChunk · "frame_last_seq"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6284)

```cpp
def_readwrite("frame_last_seq",
&nsf::StreamChunk::frameLastSeq)
```

### NativeStreamChunk · "frame_first_seq"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6283)

```cpp
def_readwrite("frame_first_seq",
&nsf::StreamChunk::frameFirstSeq)
```

### NativeStreamChunk · "frame_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6282)

```cpp
def_readwrite("frame_id",
&nsf::StreamChunk::frameId)
```

### NativeStreamChunk · "key_chunk"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6281)

```cpp
def_readwrite("key_chunk",
&nsf::StreamChunk::keyChunk)
```

### NativeStreamChunk · "deadline_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6280)

```cpp
def_readwrite("deadline_ms",
&nsf::StreamChunk::deadlineMs)
```

### NativeStreamChunk · "arrival_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6279)

```cpp
def_readwrite("arrival_ms",
&nsf::StreamChunk::arrivalMs)
```

### NativeStreamChunk · "capture_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6278)

```cpp
def_readwrite("capture_ms",
&nsf::StreamChunk::captureMs)
```

### NativeStreamChunk · "content_type"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6277)

```cpp
def_readwrite("content_type",
&nsf::StreamChunk::contentType)
```

### NativeStreamChunk · "payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6268)

```cpp
def_property("payload",
[] (const nsf::StreamChunk& chunk) { implementation omitted },
[] (nsf::StreamChunk& chunk, const py::bytes& value) { implementation omitted })
```

### NativeStreamChunk · "seq"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6267)

```cpp
def_readwrite("seq",
&nsf::StreamChunk::seq)
```

### NativeStreamChunk · "session_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6266)

```cpp
def_readwrite("session_epoch",
&nsf::StreamChunk::sessionEpoch)
```

### NativeStreamChunk · "stream_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6265)

```cpp
def_readwrite("stream_id",
&nsf::StreamChunk::streamId)
```

### NativeStreamNameMapEntry · "predicted_group_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6325)

```cpp
def_property_readonly("predicted_group_items",
&nsf::StreamNameMapEntry::predictedGroupItems)
```

### NativeStreamNameMapEntry · "has_group_binding"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6323)

```cpp
def_property_readonly("has_group_binding",
&nsf::StreamNameMapEntry::hasGroupBinding)
```

### NativeStreamNameMapEntry · "is_tombstone"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6322)

```cpp
def("is_tombstone",
&nsf::StreamNameMapEntry::isTombstone)
```

### NativeStreamNameMapEntry · "make_tombstone"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6321)

```cpp
def_static("make_tombstone",
&nsf::StreamNameMapEntry::makeTombstone)
```

### NativeStreamNameMapEntry · "from_grouped_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6311)

```cpp
def_static("from_grouped_name",
[] (const std::string& name, std::string groupId, std::string sampleClass,
          uint64_t groupItemIndex, uint64_t predictedSourceItems,
          uint64_t predictedRepairItems) { implementation omitted },
py::arg("name"),
py::arg("group_id"),
py::arg("sample_class"),
py::arg("group_item_index"),
py::arg("predicted_source_items"),
py::arg("predicted_repair_items"))
```

### NativeStreamNameMapEntry · "from_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6307)

```cpp
def_static("from_name",
[] (const std::string& name) { implementation omitted },
py::arg("name"))
```

### NativeStreamNameMapEntry · "predicted_repair_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6305)

```cpp
def_readwrite("predicted_repair_items",
&nsf::StreamNameMapEntry::predictedRepairItems)
```

### NativeStreamNameMapEntry · "predicted_source_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6303)

```cpp
def_readwrite("predicted_source_items",
&nsf::StreamNameMapEntry::predictedSourceItems)
```

### NativeStreamNameMapEntry · "group_item_index"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6302)

```cpp
def_readwrite("group_item_index",
&nsf::StreamNameMapEntry::groupItemIndex)
```

### NativeStreamNameMapEntry · "sample_class"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6301)

```cpp
def_readwrite("sample_class",
&nsf::StreamNameMapEntry::sampleClass)
```

### NativeStreamNameMapEntry · "group_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6300)

```cpp
def_readwrite("group_id",
&nsf::StreamNameMapEntry::groupId)
```

### NativeStreamNameMapEntry · "tombstone"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6299)

```cpp
def_readwrite("tombstone",
&nsf::StreamNameMapEntry::tombstone)
```

### NativeStreamNameMapEntry · "original_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6292)

```cpp
def_property("original_name",
[] (const nsf::StreamNameMapEntry& entry) { implementation omitted },
[] (nsf::StreamNameMapEntry& entry, const std::string& value) { implementation omitted })
```

### NativeStreamNameMapBlock · "last_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6376)

```cpp
def("last_cursor",
&nsf::StreamNameMapBlock::lastCursor)
```

### NativeStreamNameMapBlock · "fits_signed_wire_budget"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6374)

```cpp
def("fits_signed_wire_budget",
&nsf::StreamNameMapBlock::fitsSignedWireBudget,
py::arg("signed_envelope_overhead"),
py::arg("configured_wire_cap"))
```

### NativeStreamNameMapBlock · "content_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6371)

```cpp
def("content_digest",
[] (const nsf::StreamNameMapBlock& block) { implementation omitted })
```

### NativeStreamNameMapBlock · "canonical_content"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6368)

```cpp
def("canonical_content",
[] (const nsf::StreamNameMapBlock& block) { implementation omitted })
```

### NativeStreamNameMapBlock · "decode"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6360)

```cpp
def_static("decode",
[] (const py::bytes& wire) { implementation omitted },
py::arg("wire"))
```

### NativeStreamNameMapBlock · "wire_encode"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6357)

```cpp
def("wire_encode",
[] (const nsf::StreamNameMapBlock& block) { implementation omitted })
```

### NativeStreamNameMapBlock · "validate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6353)

```cpp
def("validate",
[] (const nsf::StreamNameMapBlock& block) -> py::object { implementation omitted })
```

### NativeStreamNameMapBlock · "entries"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6352)

```cpp
def_readwrite("entries",
&nsf::StreamNameMapBlock::entries)
```

### NativeStreamNameMapBlock · "previous_content_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6337)

```cpp
def_property("previous_content_digest",
[] (const nsf::StreamNameMapBlock& block) -> py::object { implementation omitted },
[] (nsf::StreamNameMapBlock& block, const py::object& value) { implementation omitted })
```

### NativeStreamNameMapBlock · "first_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6336)

```cpp
def_readwrite("first_cursor",
&nsf::StreamNameMapBlock::firstCursor)
```

### NativeStreamNameMapBlock · "block_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6335)

```cpp
def_readwrite("block_capacity",
&nsf::StreamNameMapBlock::blockCapacity)
```

### NativeStreamNameMapBlock · "block_number"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6334)

```cpp
def_readwrite("block_number",
&nsf::StreamNameMapBlock::blockNumber)
```

### NativeStreamNameMapBlock · "mapping_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6333)

```cpp
def_readwrite("mapping_version",
&nsf::StreamNameMapBlock::mappingVersion)
```

### NativeStreamNameMapBlock · "session_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6332)

```cpp
def_readwrite("session_epoch",
&nsf::StreamNameMapBlock::sessionEpoch)
```

### NativeStreamNameMapBlock · "stream_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6331)

```cpp
def_readwrite("stream_id",
&nsf::StreamNameMapBlock::streamId)
```

### NativeStreamNameMapBlock · "contract_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6330)

```cpp
def_readwrite("contract_version",
&nsf::StreamNameMapBlock::contractVersion)
```

### m · "make_stream_name_map_root"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6378)

```cpp
def("make_stream_name_map_root",
[] (const std::string& provider, const std::string& streamId) { implementation omitted },
py::arg("provider"),
py::arg("stream_id"))
```

### m · "make_stream_name_map_block_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6383)

```cpp
def("make_stream_name_map_block_name",
[] (const std::string& mappingRoot, uint64_t mappingVersion,
        uint64_t blockNumber) { implementation omitted },
py::arg("mapping_root"),
py::arg("mapping_version"),
py::arg("block_number"))
```

### NativeStreamCursorFrontiers · "validate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6400)

```cpp
def("validate",
[] (const nsf::StreamCursorFrontiers& frontiers,
                         uint64_t blockCapacity,
                         uint64_t checkpointBlock) -> py::object { implementation omitted },
py::arg("block_capacity"),
py::arg("checkpoint_block"))
```

### NativeStreamCursorFrontiers · "next_reserved"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6399)

```cpp
def_readwrite("next_reserved",
&nsf::StreamCursorFrontiers::nextReserved)
```

### NativeStreamCursorFrontiers · "mapping_committed_through"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6397)

```cpp
def_readwrite("mapping_committed_through",
&nsf::StreamCursorFrontiers::mappingCommittedThrough)
```

### NativeStreamCursorFrontiers · "latest_produced"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6396)

```cpp
def_readwrite("latest_produced",
&nsf::StreamCursorFrontiers::latestProduced)
```

### NativeStreamCursorFrontiers · "latest_join"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6395)

```cpp
def_readwrite("latest_join",
&nsf::StreamCursorFrontiers::latestJoin)
```

### NativeStreamCursorFrontiers · "oldest_retained"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6394)

```cpp
def_readwrite("oldest_retained",
&nsf::StreamCursorFrontiers::oldestRetained)
```

### NativeStreamNameMapCheckpoint · "content_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6411)

```cpp
def_property("content_digest",
[] (const nsf::StreamNameMapCheckpoint& checkpoint) { implementation omitted },
[] (nsf::StreamNameMapCheckpoint& checkpoint, const py::bytes& value) { implementation omitted })
```

### NativeStreamNameMapCheckpoint · "block_number"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6410)

```cpp
def_readwrite("block_number",
&nsf::StreamNameMapCheckpoint::blockNumber)
```

### NativeStreamNameMapCheckpoint · "frontiers"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6409)

```cpp
def_readwrite("frontiers",
&nsf::StreamNameMapCheckpoint::frontiers)
```

### NativeStreamNameMapResolverConfig · "max_original_name_wire_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6454)

```cpp
def_readwrite("max_original_name_wire_bytes",
&nsf::StreamNameMapResolverConfig::maxOriginalNameWireBytes)
```

### NativeStreamNameMapResolverConfig · "max_reverse_entries"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6452)

```cpp
def_readwrite("max_reverse_entries",
&nsf::StreamNameMapResolverConfig::maxReverseEntries)
```

### NativeStreamNameMapResolverConfig · "max_quarantine_blocks"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6450)

```cpp
def_readwrite("max_quarantine_blocks",
&nsf::StreamNameMapResolverConfig::maxQuarantineBlocks)
```

### NativeStreamNameMapResolverConfig · "max_verified_blocks"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6448)

```cpp
def_readwrite("max_verified_blocks",
&nsf::StreamNameMapResolverConfig::maxVerifiedBlocks)
```

### NativeStreamNameMapResolverConfig · "signed_wire_cap"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6447)

```cpp
def_readwrite("signed_wire_cap",
&nsf::StreamNameMapResolverConfig::signedWireCap)
```

### NativeStreamNameMapResolverConfig · "payload_prefix"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6440)

```cpp
def_property("payload_prefix",
[] (const nsf::StreamNameMapResolverConfig& config) { implementation omitted },
[] (nsf::StreamNameMapResolverConfig& config, const std::string& value) { implementation omitted })
```

### NativeStreamNameMapResolverConfig · "mapping_root"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6433)

```cpp
def_property("mapping_root",
[] (const nsf::StreamNameMapResolverConfig& config) { implementation omitted },
[] (nsf::StreamNameMapResolverConfig& config, const std::string& value) { implementation omitted })
```

### NativeStreamNameMapResolverConfig · "expected_provider"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6426)

```cpp
def_property("expected_provider",
[] (const nsf::StreamNameMapResolverConfig& config) { implementation omitted },
[] (nsf::StreamNameMapResolverConfig& config, const std::string& value) { implementation omitted })
```

### NativeStreamNameMapResolverConfig · "block_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6425)

```cpp
def_readwrite("block_capacity",
&nsf::StreamNameMapResolverConfig::blockCapacity)
```

### NativeStreamNameMapResolverConfig · "mapping_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6424)

```cpp
def_readwrite("mapping_version",
&nsf::StreamNameMapResolverConfig::mappingVersion)
```

### NativeStreamNameMapResolverConfig · "session_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6423)

```cpp
def_readwrite("session_epoch",
&nsf::StreamNameMapResolverConfig::sessionEpoch)
```

### NativeStreamNameMapResolverConfig · "stream_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6422)

```cpp
def_readwrite("stream_id",
&nsf::StreamNameMapResolverConfig::streamId)
```

### NativeStreamNameMapResolverConfig · "contract_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6421)

```cpp
def_readwrite("contract_version",
&nsf::StreamNameMapResolverConfig::contractVersion)
```

### NativeVerifiedStreamNameMapData · "required_before_monotonic_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6485)

```cpp
def_readwrite("required_before_monotonic_ms",
&nsf::VerifiedStreamNameMapData::requiredBeforeMonotonicMs)
```

### NativeVerifiedStreamNameMapData · "received_monotonic_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6483)

```cpp
def_readwrite("received_monotonic_ms",
&nsf::VerifiedStreamNameMapData::receivedMonotonicMs)
```

### NativeVerifiedStreamNameMapData · "content"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6476)

```cpp
def_property("content",
[] (const nsf::VerifiedStreamNameMapData& input) { implementation omitted },
[] (nsf::VerifiedStreamNameMapData& input, const py::bytes& value) { implementation omitted })
```

### NativeVerifiedStreamNameMapData · "signed_wire_size"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6475)

```cpp
def_readwrite("signed_wire_size",
&nsf::VerifiedStreamNameMapData::signedWireSize)
```

### NativeVerifiedStreamNameMapData · "has_final_block"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6474)

```cpp
def_readwrite("has_final_block",
&nsf::VerifiedStreamNameMapData::hasFinalBlock)
```

### NativeVerifiedStreamNameMapData · "content_type"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6473)

```cpp
def_readwrite("content_type",
&nsf::VerifiedStreamNameMapData::contentType)
```

### NativeVerifiedStreamNameMapData · "verified_provider"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6466)

```cpp
def_property("verified_provider",
[] (const nsf::VerifiedStreamNameMapData& input) { implementation omitted },
[] (nsf::VerifiedStreamNameMapData& input, const std::string& value) { implementation omitted })
```

### NativeVerifiedStreamNameMapData · "data_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6459)

```cpp
def_property("data_name",
[] (const nsf::VerifiedStreamNameMapData& input) { implementation omitted },
[] (nsf::VerifiedStreamNameMapData& input, const std::string& value) { implementation omitted })
```

### NativeStreamNameMapAdmissionDisposition · "FATAL_SESSION"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6494)

```cpp
value("FATAL_SESSION",
nsf::StreamNameMapAdmissionDisposition::FatalSession)
```

### NativeStreamNameMapAdmissionDisposition · "REJECTED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6493)

```cpp
value("REJECTED",
nsf::StreamNameMapAdmissionDisposition::Rejected)
```

### NativeStreamNameMapAdmissionDisposition · "QUARANTINED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6492)

```cpp
value("QUARANTINED",
nsf::StreamNameMapAdmissionDisposition::Quarantined)
```

### NativeStreamNameMapAdmissionDisposition · "DUPLICATE"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6491)

```cpp
value("DUPLICATE",
nsf::StreamNameMapAdmissionDisposition::Duplicate)
```

### NativeStreamNameMapAdmissionDisposition · "ADMITTED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6490)

```cpp
value("ADMITTED",
nsf::StreamNameMapAdmissionDisposition::Admitted)
```

### NativeStreamNameMapTiming · "LATE"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6499)

```cpp
value("LATE",
nsf::StreamNameMapTiming::Late)
```

### NativeStreamNameMapTiming · "AHEAD"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6498)

```cpp
value("AHEAD",
nsf::StreamNameMapTiming::Ahead)
```

### NativeStreamNameMapTiming · "UNCLASSIFIED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6497)

```cpp
value("UNCLASSIFIED",
nsf::StreamNameMapTiming::Unclassified)
```

### NativeStreamNameMapAdmissionResult · "fatal"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6516)

```cpp
def_property_readonly("fatal",
&nsf::StreamNameMapAdmissionResult::fatal)
```

### NativeStreamNameMapAdmissionResult · "accepted"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6515)

```cpp
def_property_readonly("accepted",
&nsf::StreamNameMapAdmissionResult::accepted)
```

### NativeStreamNameMapAdmissionResult · "mapping_committed_through"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6513)

```cpp
def_readonly("mapping_committed_through",
&nsf::StreamNameMapAdmissionResult::mappingCommittedThrough)
```

### NativeStreamNameMapAdmissionResult · "state_changed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6512)

```cpp
def_readonly("state_changed",
&nsf::StreamNameMapAdmissionResult::stateChanged)
```

### NativeStreamNameMapAdmissionResult · "reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6511)

```cpp
def_readonly("reason",
&nsf::StreamNameMapAdmissionResult::reason)
```

### NativeStreamNameMapAdmissionResult · "timing_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6507)

```cpp
def_property_readonly("timing_name",
[] (const nsf::StreamNameMapAdmissionResult& result) { implementation omitted })
```

### NativeStreamNameMapAdmissionResult · "disposition_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6503)

```cpp
def_property_readonly("disposition_name",
[] (const nsf::StreamNameMapAdmissionResult& result) { implementation omitted })
```

### NativeStreamNameMapResolution · "predicted_group_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6542)

```cpp
def_property_readonly("predicted_group_items",
&nsf::StreamNameMapResolution::predictedGroupItems)
```

### NativeStreamNameMapResolution · "has_group_binding"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6540)

```cpp
def_property_readonly("has_group_binding",
&nsf::StreamNameMapResolution::hasGroupBinding)
```

### NativeStreamNameMapResolution · "schedulable"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6539)

```cpp
def_property_readonly("schedulable",
&nsf::StreamNameMapResolution::schedulable)
```

### NativeStreamNameMapResolution · "timing_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6535)

```cpp
def_property_readonly("timing_name",
[] (const nsf::StreamNameMapResolution& resolution) { implementation omitted })
```

### NativeStreamNameMapResolution · "predicted_repair_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6533)

```cpp
def_readonly("predicted_repair_items",
&nsf::StreamNameMapResolution::predictedRepairItems)
```

### NativeStreamNameMapResolution · "predicted_source_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6531)

```cpp
def_readonly("predicted_source_items",
&nsf::StreamNameMapResolution::predictedSourceItems)
```

### NativeStreamNameMapResolution · "group_item_index"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6530)

```cpp
def_readonly("group_item_index",
&nsf::StreamNameMapResolution::groupItemIndex)
```

### NativeStreamNameMapResolution · "sample_class"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6529)

```cpp
def_readonly("sample_class",
&nsf::StreamNameMapResolution::sampleClass)
```

### NativeStreamNameMapResolution · "group_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6528)

```cpp
def_readonly("group_id",
&nsf::StreamNameMapResolution::groupId)
```

### NativeStreamNameMapResolution · "terminal_unproduced"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6526)

```cpp
def_readonly("terminal_unproduced",
&nsf::StreamNameMapResolution::terminalUnproduced)
```

### NativeStreamNameMapResolution · "tombstone"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6525)

```cpp
def_readonly("tombstone",
&nsf::StreamNameMapResolution::tombstone)
```

### NativeStreamNameMapResolution · "original_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6520)

```cpp
def_property_readonly("original_name",
[] (const nsf::StreamNameMapResolution& resolution) { implementation omitted })
```

### NativeStreamNameMapResolution · "cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6519)

```cpp
def_readonly("cursor",
&nsf::StreamNameMapResolution::cursor)
```

### NativeStreamNameResolverState · "diagnostics"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6591)

```cpp
def("diagnostics",
&nsf::StreamNameResolverState::diagnostics)
```

### NativeStreamNameResolverState · "binding_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6590)

```cpp
def("binding_count",
&nsf::StreamNameResolverState::bindingCount)
```

### NativeStreamNameResolverState · "quarantined_block_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6588)

```cpp
def("quarantined_block_count",
&nsf::StreamNameResolverState::quarantinedBlockCount)
```

### NativeStreamNameResolverState · "verified_block_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6587)

```cpp
def("verified_block_count",
&nsf::StreamNameResolverState::verifiedBlockCount)
```

### NativeStreamNameResolverState · "faulted"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6586)

```cpp
def("faulted",
&nsf::StreamNameResolverState::faulted)
```

### NativeStreamNameResolverState · "checkpoint"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6585)

```cpp
def("checkpoint",
&nsf::StreamNameResolverState::checkpoint)
```

### NativeStreamNameResolverState · "frontiers"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6584)

```cpp
def("frontiers",
&nsf::StreamNameResolverState::frontiers)
```

### NativeStreamNameResolverState · "evict_local_block"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6582)

```cpp
def("evict_local_block",
&nsf::StreamNameResolverState::evictLocalBlock,
py::arg("block_number"))
```

### NativeStreamNameResolverState · "mark_terminal_unproduced"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6579)

```cpp
def("mark_terminal_unproduced",
&nsf::StreamNameResolverState::markTerminalUnproduced,
py::arg("cursor"))
```

### NativeStreamNameResolverState · "reverse_resolve"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6574)

```cpp
def("reverse_resolve",
[] (const nsf::StreamNameResolverState& resolver,
                                const std::string& originalName) -> py::object { implementation omitted },
py::arg("original_name"))
```

### NativeStreamNameResolverState · "resolve"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6569)

```cpp
def("resolve",
[] (const nsf::StreamNameResolverState& resolver,
                        nsf::StreamCursor cursor) -> py::object { implementation omitted },
py::arg("cursor"))
```

### NativeStreamNameResolverState · "lookup"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6567)

```cpp
def("lookup",
&nsf::StreamNameResolverState::lookup,
py::arg("cursor"))
```

### NativeStreamNameResolverState · "refresh_checkpoint"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6565)

```cpp
def("refresh_checkpoint",
&nsf::StreamNameResolverState::refreshCheckpoint,
py::arg("checkpoint"))
```

### NativeStreamNameResolverState · "admit_verified_wire"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6551)

```cpp
def("admit_verified_wire",
[] (nsf::StreamNameResolverState& resolver,
          nsf::VerifiedStreamNameMapData input,
          const py::bytes& contentWire) { implementation omitted },
py::arg("input"),
py::arg("content_wire"))
```

### NativeStreamNameResolverState · "admit_verified_block"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6549)

```cpp
def("admit_verified_block",
&nsf::StreamNameResolverState::admitVerifiedBlock,
py::arg("input"))
```

### NativeStreamNameResolverState · "reset"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6547)

```cpp
def("reset",
&nsf::StreamNameResolverState::reset,
py::arg("config"),
py::arg("checkpoint"))
```

### NativeStreamMetrics · "bytes_received"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6607)

```cpp
def_readwrite("bytes_received",
&nsf::StreamMetrics::bytesReceived)
```

### NativeStreamMetrics · "bytes_produced"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6606)

```cpp
def_readwrite("bytes_produced",
&nsf::StreamMetrics::bytesProduced)
```

### NativeStreamMetrics · "max_pending"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6605)

```cpp
def_readwrite("max_pending",
&nsf::StreamMetrics::maxPending)
```

### NativeStreamMetrics · "overflows"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6604)

```cpp
def_readwrite("overflows",
&nsf::StreamMetrics::overflows)
```

### NativeStreamMetrics · "nacks"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6603)

```cpp
def_readwrite("nacks",
&nsf::StreamMetrics::nacks)
```

### NativeStreamMetrics · "timeouts"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6602)

```cpp
def_readwrite("timeouts",
&nsf::StreamMetrics::timeouts)
```

### NativeStreamMetrics · "gaps"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6601)

```cpp
def_readwrite("gaps",
&nsf::StreamMetrics::gaps)
```

### NativeStreamMetrics · "stale"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6600)

```cpp
def_readwrite("stale",
&nsf::StreamMetrics::stale)
```

### NativeStreamMetrics · "duplicates"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6599)

```cpp
def_readwrite("duplicates",
&nsf::StreamMetrics::duplicates)
```

### NativeStreamMetrics · "emitted"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6598)

```cpp
def_readwrite("emitted",
&nsf::StreamMetrics::emitted)
```

### NativeStreamMetrics · "received"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6597)

```cpp
def_readwrite("received",
&nsf::StreamMetrics::received)
```

### NativeStreamMetrics · "evicted"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6596)

```cpp
def_readwrite("evicted",
&nsf::StreamMetrics::evicted)
```

### NativeStreamMetrics · "produced"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6595)

```cpp
def_readwrite("produced",
&nsf::StreamMetrics::produced)
```

### NativeStreamProducerBuffer · "metrics"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6615)

```cpp
def_property_readonly("metrics",
&nsf::StreamProducerBuffer::metrics)
```

### NativeStreamProducerBuffer · "size"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6614)

```cpp
def("size",
&nsf::StreamProducerBuffer::size)
```

### NativeStreamProducerBuffer · "sequences"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6613)

```cpp
def("sequences",
&nsf::StreamProducerBuffer::sequences)
```

### NativeStreamProducerBuffer · "get"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6612)

```cpp
def("get",
&nsf::StreamProducerBuffer::get)
```

### NativeStreamProducerBuffer · "put"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6611)

```cpp
def("put",
&nsf::StreamProducerBuffer::put)
```

### NativeStreamConsumerReorderBuffer · "metrics"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6634)

```cpp
def_property_readonly("metrics",
&nsf::StreamConsumerReorderBuffer::metrics)
```

### NativeStreamConsumerReorderBuffer · "pending_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6633)

```cpp
def_property_readonly("pending_bytes",
&nsf::StreamConsumerReorderBuffer::pendingBytes)
```

### NativeStreamConsumerReorderBuffer · "pending_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6632)

```cpp
def_property_readonly("pending_count",
&nsf::StreamConsumerReorderBuffer::pendingCount)
```

### NativeStreamConsumerReorderBuffer · "next_seq"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6631)

```cpp
def_property_readonly("next_seq",
&nsf::StreamConsumerReorderBuffer::nextSeq)
```

### NativeStreamConsumerReorderBuffer · "skip_to"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6630)

```cpp
def("skip_to",
&nsf::StreamConsumerReorderBuffer::skipTo)
```

### NativeStreamConsumerReorderBuffer · "drain_ready"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6629)

```cpp
def("drain_ready",
&nsf::StreamConsumerReorderBuffer::drainReady)
```

### NativeStreamConsumerReorderBuffer · "pending_sequences"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6627)

```cpp
def("pending_sequences",
&nsf::StreamConsumerReorderBuffer::pendingSequences,
py::arg("limit") = 0)
```

### NativeStreamConsumerReorderBuffer · "missing_sequences"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6625)

```cpp
def("missing_sequences",
&nsf::StreamConsumerReorderBuffer::missingSequences,
py::arg("limit") = 32)
```

### NativeStreamConsumerReorderBuffer · "push"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6624)

```cpp
def("push",
&nsf::StreamConsumerReorderBuffer::push)
```

### NativeStreamConsumerReorderBuffer · "reset"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6622)

```cpp
def("reset",
&nsf::StreamConsumerReorderBuffer::reset,
py::arg("stream_id"),
py::arg("session_epoch"),
py::arg("next_seq") = 0)
```

### NativeStreamPrefetchPhase · "STOPPED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6642)

```cpp
value("STOPPED",
nsf::StreamPrefetchPhase::Stopped)
```

### NativeStreamPrefetchPhase · "RECOVERING"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6641)

```cpp
value("RECOVERING",
nsf::StreamPrefetchPhase::Recovering)
```

### NativeStreamPrefetchPhase · "FETCHING"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6640)

```cpp
value("FETCHING",
nsf::StreamPrefetchPhase::Fetching)
```

### NativeStreamPrefetchPhase · "ADJUSTING"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6639)

```cpp
value("ADJUSTING",
nsf::StreamPrefetchPhase::Adjusting)
```

### NativeStreamPrefetchPhase · "CHASING"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6638)

```cpp
value("CHASING",
nsf::StreamPrefetchPhase::Chasing)
```

### NativeStreamPrefetchPhase · "INACTIVE"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6637)

```cpp
value("INACTIVE",
nsf::StreamPrefetchPhase::Inactive)
```

### NativeStreamFetchDecision · "reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6682)

```cpp
def_readwrite("reason",
&nsf::StreamFetchDecision::reason)
```

### NativeStreamFetchDecision · "capacity_reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6681)

```cpp
def_readwrite("capacity_reason",
&nsf::StreamFetchDecision::capacityReason)
```

### NativeStreamFetchDecision · "mapping_wait_reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6680)

```cpp
def_readwrite("mapping_wait_reason",
&nsf::StreamFetchDecision::mappingWaitReason)
```

### NativeStreamFetchDecision · "detector_profile"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6679)

```cpp
def_readwrite("detector_profile",
&nsf::StreamFetchDecision::detectorProfile)
```

### NativeStreamFetchDecision · "policy_mode"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6678)

```cpp
def_readwrite("policy_mode",
&nsf::StreamFetchDecision::policyMode)
```

### NativeStreamFetchDecision · "phase_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6675)

```cpp
def_property_readonly("phase_name",
[] (const nsf::StreamFetchDecision& value) { implementation omitted })
```

### NativeStreamFetchDecision · "phase"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6674)

```cpp
def_readwrite("phase",
&nsf::StreamFetchDecision::phase)
```

### NativeStreamFetchDecision · "retransmission_eligible"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6673)

```cpp
def_readwrite("retransmission_eligible",
&nsf::StreamFetchDecision::retransmissionEligible)
```

### NativeStreamFetchDecision · "congestion_hold"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6672)

```cpp
def_readwrite("congestion_hold",
&nsf::StreamFetchDecision::congestionHold)
```

### NativeStreamFetchDecision · "future_wait"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6671)

```cpp
def_readwrite("future_wait",
&nsf::StreamFetchDecision::futureWait)
```

### NativeStreamFetchDecision · "mapping_ready"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6670)

```cpp
def_readwrite("mapping_ready",
&nsf::StreamFetchDecision::mappingReady)
```

### NativeStreamFetchDecision · "live_edge_confidence"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6669)

```cpp
def_readwrite("live_edge_confidence",
&nsf::StreamFetchDecision::liveEdgeConfidence)
```

### NativeStreamFetchDecision · "pressure"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6668)

```cpp
def_readwrite("pressure",
&nsf::StreamFetchDecision::pressure)
```

### NativeStreamFetchDecision · "atomic_deferrals"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6667)

```cpp
def_readwrite("atomic_deferrals",
&nsf::StreamFetchDecision::atomicDeferrals)
```

### NativeStreamFetchDecision · "atomic_expansions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6666)

```cpp
def_readwrite("atomic_expansions",
&nsf::StreamFetchDecision::atomicExpansions)
```

### NativeStreamFetchDecision · "later_cursor_advice"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6665)

```cpp
def_readwrite("later_cursor_advice",
&nsf::StreamFetchDecision::laterCursorAdvice)
```

### NativeStreamFetchDecision · "terminal_unproduced_advice"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6664)

```cpp
def_readwrite("terminal_unproduced_advice",
&nsf::StreamFetchDecision::terminalUnproducedAdvice)
```

### NativeStreamFetchDecision · "future_wait_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6663)

```cpp
def_readwrite("future_wait_count",
&nsf::StreamFetchDecision::futureWaitCount)
```

### NativeStreamFetchDecision · "retransmission_budget"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6662)

```cpp
def_readwrite("retransmission_budget",
&nsf::StreamFetchDecision::retransmissionBudget)
```

### NativeStreamFetchDecision · "payload_budget"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6661)

```cpp
def_readwrite("payload_budget",
&nsf::StreamFetchDecision::payloadBudget)
```

### NativeStreamFetchDecision · "mapping_budget"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6660)

```cpp
def_readwrite("mapping_budget",
&nsf::StreamFetchDecision::mappingBudget)
```

### NativeStreamFetchDecision · "aggregate_in_flight_limit"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6659)

```cpp
def_readwrite("aggregate_in_flight_limit",
&nsf::StreamFetchDecision::aggregateInFlightLimit)
```

### NativeStreamFetchDecision · "payload_end_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6658)

```cpp
def_readwrite("payload_end_cursor",
&nsf::StreamFetchDecision::payloadEndCursor)
```

### NativeStreamFetchDecision · "payload_begin_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6657)

```cpp
def_readwrite("payload_begin_cursor",
&nsf::StreamFetchDecision::payloadBeginCursor)
```

### NativeStreamFetchDecision · "mapping_end_block"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6656)

```cpp
def_readwrite("mapping_end_block",
&nsf::StreamFetchDecision::mappingEndBlock)
```

### NativeStreamFetchDecision · "mapping_begin_block"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6655)

```cpp
def_readwrite("mapping_begin_block",
&nsf::StreamFetchDecision::mappingBeginBlock)
```

### NativeStreamFetchDecision · "remaining_recovery_budget_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6654)

```cpp
def_readwrite("remaining_recovery_budget_ms",
&nsf::StreamFetchDecision::remainingRecoveryBudgetMs)
```

### NativeStreamFetchDecision · "recovery_checkpoint_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6653)

```cpp
def_readwrite("recovery_checkpoint_ms",
&nsf::StreamFetchDecision::recoveryCheckpointMs)
```

### NativeStreamFetchDecision · "hold_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6652)

```cpp
def_readwrite("hold_ms",
&nsf::StreamFetchDecision::holdMs)
```

### NativeStreamFetchDecision · "packet_demand"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6651)

```cpp
def_readwrite("packet_demand",
&nsf::StreamFetchDecision::packetDemand)
```

### NativeStreamFetchDecision · "sample_demand"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6650)

```cpp
def_readwrite("sample_demand",
&nsf::StreamFetchDecision::sampleDemand)
```

### NativeStreamFetchDecision · "missing_timeout_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6649)

```cpp
def_readwrite("missing_timeout_ms",
&nsf::StreamFetchDecision::missingTimeoutMs)
```

### NativeStreamFetchDecision · "interest_lifetime_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6648)

```cpp
def_readwrite("interest_lifetime_ms",
&nsf::StreamFetchDecision::interestLifetimeMs)
```

### NativeStreamFetchDecision · "lookahead"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6647)

```cpp
def_readwrite("lookahead",
&nsf::StreamFetchDecision::lookahead)
```

### NativeStreamFetchDecision · "window"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6646)

```cpp
def_readwrite("window",
&nsf::StreamFetchDecision::window)
```

### NativeStreamAdaptiveFetcherState · "decide"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6770)

```cpp
def("decide",
&nsf::StreamAdaptiveFetcherState::decide,
py::arg("now_ms") = 0,
py::arg("playout_deadline_ms") = 0)
```

### NativeStreamAdaptiveFetcherState · "invalid_observations"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6769)

```cpp
def_property_readonly("invalid_observations",
&nsf::StreamAdaptiveFetcherState::invalidObservations)
```

### NativeStreamAdaptiveFetcherState · "phase_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6766)

```cpp
def_property_readonly("phase_name",
[] (const nsf::StreamAdaptiveFetcherState& value) { implementation omitted })
```

### NativeStreamAdaptiveFetcherState · "stop_live"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6765)

```cpp
def("stop_live",
&nsf::StreamAdaptiveFetcherState::stopLive)
```

### NativeStreamAdaptiveFetcherState · "record_invalid_observation"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6764)

```cpp
def("record_invalid_observation",
&nsf::StreamAdaptiveFetcherState::recordInvalidObservation)
```

### NativeStreamAdaptiveFetcherState · "record_recovery"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6762)

```cpp
def("record_recovery",
&nsf::StreamAdaptiveFetcherState::recordRecovery,
py::arg("completed"))
```

### NativeStreamAdaptiveFetcherState · "begin_recovery"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6760)

```cpp
def("begin_recovery",
&nsf::StreamAdaptiveFetcherState::beginRecovery,
py::arg("now_ms"),
py::arg("playout_deadline_ms"))
```

### NativeStreamAdaptiveFetcherState · "observe_sample_extent"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6758)

```cpp
def("observe_sample_extent",
&nsf::StreamAdaptiveFetcherState::observeSampleExtent,
py::arg("predicted_count"),
py::arg("actual_count"))
```

### NativeStreamAdaptiveFetcherState · "observe_accepted_sample"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6754)

```cpp
def("observe_accepted_sample",
&nsf::StreamAdaptiveFetcherState::observeAcceptedSample,
py::arg("session_epoch"),
py::arg("sample_id"),
py::arg("arrival_ms"),
py::arg("retrieval_delay_ms"),
py::arg("segment_count") = 1,
py::arg("known_produced") = true)
```

### NativeStreamAdaptiveFetcherState · "set_in_flight"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6752)

```cpp
def("set_in_flight",
&nsf::StreamAdaptiveFetcherState::setInFlight,
py::arg("mapping"),
py::arg("payload"),
py::arg("retransmission"))
```

### NativeStreamAdaptiveFetcherState · "set_mapped_live_policy_enabled"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6749)

```cpp
def("set_mapped_live_policy_enabled",
&nsf::StreamAdaptiveFetcherState::setMappedLivePolicyEnabled,
py::arg("enabled"))
```

### NativeStreamAdaptiveFetcherState · "advance_next_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6747)

```cpp
def("advance_next_cursor",
&nsf::StreamAdaptiveFetcherState::advanceNextCursor,
py::arg("next_cursor"))
```

### NativeStreamAdaptiveFetcherState · "update_mapping_frontier"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6744)

```cpp
def("update_mapping_frontier",
&nsf::StreamAdaptiveFetcherState::updateMappingFrontier,
py::arg("mapping_committed_through_cursor"),
py::arg("next_reserved_cursor"))
```

### NativeStreamAdaptiveFetcherState · "reset_mapped_live"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6739)

```cpp
def("reset_mapped_live",
&nsf::StreamAdaptiveFetcherState::resetMappedLive,
py::arg("session_epoch"),
py::arg("next_cursor"),
py::arg("sample_period_ms"),
py::arg("latest_produced_cursor"),
py::arg("mapping_committed_through_cursor"),
py::arg("next_reserved_cursor"),
py::arg("now_ms") = 0)
```

### NativeStreamAdaptiveFetcherState · "configure_mapped_live"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6735)

```cpp
def("configure_mapped_live",
&nsf::StreamAdaptiveFetcherState::configureMappedLive,
py::arg("aggregate_limit"),
py::arg("mapping_reserve"),
py::arg("retransmission_reserve"),
py::arg("block_capacity"),
py::arg("detector_profile"))
```

### NativeStreamAdaptiveFetcherState · "reset_live"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6732)

```cpp
def("reset_live",
&nsf::StreamAdaptiveFetcherState::resetLive,
py::arg("session_epoch"),
py::arg("next_seq"),
py::arg("sample_period_ms"),
py::arg("now_ms") = 0)
```

### NativeStreamAdaptiveFetcherState · "decay"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6731)

```cpp
def("decay",
&nsf::StreamAdaptiveFetcherState::decay,
py::arg("factor") = 0.85)
```

### NativeStreamAdaptiveFetcherState · "set_backlog_pressure"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6730)

```cpp
def("set_backlog_pressure",
&nsf::StreamAdaptiveFetcherState::setBacklogPressure)
```

### NativeStreamAdaptiveFetcherState · "record_duplicate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6729)

```cpp
def("record_duplicate",
&nsf::StreamAdaptiveFetcherState::recordDuplicate)
```

### NativeStreamAdaptiveFetcherState · "record_congestion_mark"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6727)

```cpp
def("record_congestion_mark",
&nsf::StreamAdaptiveFetcherState::recordCongestionMark,
py::arg("cursor"),
py::arg("mark"))
```

### NativeStreamAdaptiveFetcherState · "record_nack_reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6724)

```cpp
def("record_nack_reason",
py::overload_cast<uint64_t, const std::string&>(
           &nsf::StreamAdaptiveFetcherState::recordNack),
py::arg("cursor"),
py::arg("reason"))
```

### NativeStreamAdaptiveFetcherState · "record_nack"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6722)

```cpp
def("record_nack",
py::overload_cast<>(
           &nsf::StreamAdaptiveFetcherState::recordNack))
```

### NativeStreamAdaptiveFetcherState · "record_timeout_evidence"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6719)

```cpp
def("record_timeout_evidence",
py::overload_cast<uint64_t, bool, bool>(
           &nsf::StreamAdaptiveFetcherState::recordTimeout),
py::arg("cursor"),
py::arg("known_produced"),
py::arg("was_future"))
```

### NativeStreamAdaptiveFetcherState · "record_timeout"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6717)

```cpp
def("record_timeout",
py::overload_cast<>(
           &nsf::StreamAdaptiveFetcherState::recordTimeout))
```

### NativeStreamAdaptiveFetcherState · "observe_rtt"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6715)

```cpp
def("observe_rtt",
&nsf::StreamAdaptiveFetcherState::observeRtt,
py::arg("sample_ms"),
py::arg("alpha") = 0.25)
```

### NativeStreamAdaptiveFetcherState · "detector_profile"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6714)

```cpp
def_readwrite("detector_profile",
&nsf::StreamAdaptiveFetcherState::detectorProfile)
```

### NativeStreamAdaptiveFetcherState · "congestion_decrease_multiplier"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6713)

```cpp
def_readwrite("congestion_decrease_multiplier",
&nsf::StreamAdaptiveFetcherState::congestionDecreaseMultiplier)
```

### NativeStreamAdaptiveFetcherState · "adjust_multiplier"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6712)

```cpp
def_readwrite("adjust_multiplier",
&nsf::StreamAdaptiveFetcherState::adjustMultiplier)
```

### NativeStreamAdaptiveFetcherState · "chase_multiplier"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6711)

```cpp
def_readwrite("chase_multiplier",
&nsf::StreamAdaptiveFetcherState::chaseMultiplier)
```

### NativeStreamAdaptiveFetcherState · "mapping_block_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6710)

```cpp
def_readwrite("mapping_block_capacity",
&nsf::StreamAdaptiveFetcherState::mappingBlockCapacity)
```

### NativeStreamAdaptiveFetcherState · "retransmission_reserve"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6709)

```cpp
def_readwrite("retransmission_reserve",
&nsf::StreamAdaptiveFetcherState::retransmissionReserve)
```

### NativeStreamAdaptiveFetcherState · "mapping_reserve"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6708)

```cpp
def_readwrite("mapping_reserve",
&nsf::StreamAdaptiveFetcherState::mappingReserve)
```

### NativeStreamAdaptiveFetcherState · "aggregate_in_flight_limit"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6707)

```cpp
def_readwrite("aggregate_in_flight_limit",
&nsf::StreamAdaptiveFetcherState::aggregateInFlightLimit)
```

### NativeStreamAdaptiveFetcherState · "recovery_reserve_packets"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6706)

```cpp
def_readwrite("recovery_reserve_packets",
&nsf::StreamAdaptiveFetcherState::recoveryReservePackets)
```

### NativeStreamAdaptiveFetcherState · "detection_period_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6705)

```cpp
def_readwrite("detection_period_ms",
&nsf::StreamAdaptiveFetcherState::detectionPeriodMs)
```

### NativeStreamAdaptiveFetcherState · "live_edge_stable_required"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6704)

```cpp
def_readwrite("live_edge_stable_required",
&nsf::StreamAdaptiveFetcherState::liveEdgeStableRequired)
```

### NativeStreamAdaptiveFetcherState · "live_edge_window"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6703)

```cpp
def_readwrite("live_edge_window",
&nsf::StreamAdaptiveFetcherState::liveEdgeWindow)
```

### NativeStreamAdaptiveFetcherState · "live_edge_period_similarity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6702)

```cpp
def_readwrite("live_edge_period_similarity",
&nsf::StreamAdaptiveFetcherState::liveEdgePeriodSimilarity)
```

### NativeStreamAdaptiveFetcherState · "live_edge_change_threshold"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6701)

```cpp
def_readwrite("live_edge_change_threshold",
&nsf::StreamAdaptiveFetcherState::liveEdgeChangeThreshold)
```

### NativeStreamAdaptiveFetcherState · "max_missing_timeout_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6700)

```cpp
def_readwrite("max_missing_timeout_ms",
&nsf::StreamAdaptiveFetcherState::maxMissingTimeoutMs)
```

### NativeStreamAdaptiveFetcherState · "min_missing_timeout_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6699)

```cpp
def_readwrite("min_missing_timeout_ms",
&nsf::StreamAdaptiveFetcherState::minMissingTimeoutMs)
```

### NativeStreamAdaptiveFetcherState · "max_interest_lifetime_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6698)

```cpp
def_readwrite("max_interest_lifetime_ms",
&nsf::StreamAdaptiveFetcherState::maxInterestLifetimeMs)
```

### NativeStreamAdaptiveFetcherState · "min_interest_lifetime_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6697)

```cpp
def_readwrite("min_interest_lifetime_ms",
&nsf::StreamAdaptiveFetcherState::minInterestLifetimeMs)
```

### NativeStreamAdaptiveFetcherState · "max_lookahead"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6696)

```cpp
def_readwrite("max_lookahead",
&nsf::StreamAdaptiveFetcherState::maxLookahead)
```

### NativeStreamAdaptiveFetcherState · "base_lookahead"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6695)

```cpp
def_readwrite("base_lookahead",
&nsf::StreamAdaptiveFetcherState::baseLookahead)
```

### NativeStreamAdaptiveFetcherState · "min_lookahead"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6694)

```cpp
def_readwrite("min_lookahead",
&nsf::StreamAdaptiveFetcherState::minLookahead)
```

### NativeStreamAdaptiveFetcherState · "max_window"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6693)

```cpp
def_readwrite("max_window",
&nsf::StreamAdaptiveFetcherState::maxWindow)
```

### NativeStreamAdaptiveFetcherState · "base_window"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6692)

```cpp
def_readwrite("base_window",
&nsf::StreamAdaptiveFetcherState::baseWindow)
```

### NativeStreamAdaptiveFetcherState · "min_window"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6691)

```cpp
def_readwrite("min_window",
&nsf::StreamAdaptiveFetcherState::minWindow)
```

### NativeStreamAdaptiveFetcherState · "backlog_pressure"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6690)

```cpp
def_readwrite("backlog_pressure",
&nsf::StreamAdaptiveFetcherState::backlogPressure)
```

### NativeStreamAdaptiveFetcherState · "duplicate_pressure"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6689)

```cpp
def_readwrite("duplicate_pressure",
&nsf::StreamAdaptiveFetcherState::duplicatePressure)
```

### NativeStreamAdaptiveFetcherState · "nack_pressure"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6688)

```cpp
def_readwrite("nack_pressure",
&nsf::StreamAdaptiveFetcherState::nackPressure)
```

### NativeStreamAdaptiveFetcherState · "timeout_pressure"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6687)

```cpp
def_readwrite("timeout_pressure",
&nsf::StreamAdaptiveFetcherState::timeoutPressure)
```

### NativeStreamAdaptiveFetcherState · "rtt_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6686)

```cpp
def_readwrite("rtt_ms",
&nsf::StreamAdaptiveFetcherState::rttMs)
```

### NativeLiveStreamFecScheme · "GF256_TWO_REPAIR"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6776)

```cpp
value("GF256_TWO_REPAIR",
nsf::LiveStreamFecScheme::Gf256TwoRepair)
```

### NativeLiveStreamFecScheme · "XOR_ONE_REPAIR"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6775)

```cpp
value("XOR_ONE_REPAIR",
nsf::LiveStreamFecScheme::XorOneRepair)
```

### NativeLiveStreamFecScheme · "NONE"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6774)

```cpp
value("NONE",
nsf::LiveStreamFecScheme::None)
```

### NativeSampleClassProfile · "validate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6792)

```cpp
def("validate",
[] (const nsf::SampleClassProfile& value) -> py::object { implementation omitted })
```

### NativeSampleClassProfile · "bounded"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6787)

```cpp
def_static("bounded",
&nsf::SampleClassProfile::bounded,
py::arg("class_id"),
py::arg("seed_source_items"),
py::arg("hard_max_source_items"),
py::arg("history_capacity") = 32,
py::arg("safety_margin_items") = 1)
```

### NativeSampleClassProfile · "safety_margin_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6785)

```cpp
def_readwrite("safety_margin_items",
&nsf::SampleClassProfile::safetyMarginItems)
```

### NativeSampleClassProfile · "history_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6784)

```cpp
def_readwrite("history_capacity",
&nsf::SampleClassProfile::historyCapacity)
```

### NativeSampleClassProfile · "hard_max_source_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6782)

```cpp
def_readwrite("hard_max_source_items",
&nsf::SampleClassProfile::hardMaxSourceItems)
```

### NativeSampleClassProfile · "seed_source_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6781)

```cpp
def_readwrite("seed_source_items",
&nsf::SampleClassProfile::seedSourceItems)
```

### NativeSampleClassProfile · "class_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6780)

```cpp
def_readwrite("class_id",
&nsf::SampleClassProfile::classId)
```

### NativeSampleClassPredictionStatus · "overpredicted_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6808)

```cpp
def_readonly("overpredicted_items",
&nsf::SampleClassPredictionStatus::overpredictedItems)
```

### NativeSampleClassPredictionStatus · "overpredictions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6806)

```cpp
def_readonly("overpredictions",
&nsf::SampleClassPredictionStatus::overpredictions)
```

### NativeSampleClassPredictionStatus · "underpredicted_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6804)

```cpp
def_readonly("underpredicted_items",
&nsf::SampleClassPredictionStatus::underpredictedItems)
```

### NativeSampleClassPredictionStatus · "underpredictions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6802)

```cpp
def_readonly("underpredictions",
&nsf::SampleClassPredictionStatus::underpredictions)
```

### NativeSampleClassPredictionStatus · "observations"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6801)

```cpp
def_readonly("observations",
&nsf::SampleClassPredictionStatus::observations)
```

### NativeSampleClassPredictionStatus · "prediction"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6800)

```cpp
def_readonly("prediction",
&nsf::SampleClassPredictionStatus::prediction)
```

### NativeSampleClassPredictionStatus · "class_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6799)

```cpp
def_readonly("class_id",
&nsf::SampleClassPredictionStatus::classId)
```

### NativeLiveStreamSamplePredictor · "statuses"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6821)

```cpp
def("statuses",
&nsf::LiveStreamSamplePredictor::statuses)
```

### NativeLiveStreamSamplePredictor · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6820)

```cpp
def("status",
&nsf::LiveStreamSamplePredictor::status)
```

### NativeLiveStreamSamplePredictor · "observe"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6819)

```cpp
def("observe",
&nsf::LiveStreamSamplePredictor::observe)
```

### NativeLiveStreamSamplePredictor · "predict"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6815)

```cpp
def("predict",
[] (const nsf::LiveStreamSamplePredictor& predictor,
                         const std::string& classId) { implementation omitted })
```

### NativeLiveStreamSamplePredictor · "reset"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6814)

```cpp
def("reset",
&nsf::LiveStreamSamplePredictor::reset)
```

### NativeLiveStreamFecOptions · "validate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6842)

```cpp
def("validate",
[] (const nsf::LiveStreamFecOptions& value) -> py::object { implementation omitted })
```

### NativeLiveStreamFecOptions · "enabled"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6841)

```cpp
def_property_readonly("enabled",
&nsf::LiveStreamFecOptions::enabled)
```

### NativeLiveStreamFecOptions · "recovery_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6840)

```cpp
def_property_readonly("recovery_capacity",
&nsf::LiveStreamFecOptions::recoveryCapacity)
```

### NativeLiveStreamFecOptions · "gf256_two_repair"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6837)

```cpp
def_static("gf256_two_repair",
&nsf::LiveStreamFecOptions::gf256TwoRepair,
py::arg("source_items"),
py::arg("max_source_bytes"),
py::arg("recovery_budget_ms") = 500)
```

### NativeLiveStreamFecOptions · "xor_one_repair"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6834)

```cpp
def_static("xor_one_repair",
&nsf::LiveStreamFecOptions::xorOneRepair,
py::arg("source_items"),
py::arg("max_source_bytes"),
py::arg("recovery_budget_ms") = 500)
```

### NativeLiveStreamFecOptions · "none"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6833)

```cpp
def_static("none",
&nsf::LiveStreamFecOptions::none)
```

### NativeLiveStreamFecOptions · "repair_symbols"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6832)

```cpp
def_readwrite("repair_symbols",
&nsf::LiveStreamFecOptions::repairSymbols)
```

### NativeLiveStreamFecOptions · "recovery_budget_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6831)

```cpp
def_readwrite("recovery_budget_ms",
&nsf::LiveStreamFecOptions::recoveryBudgetMs)
```

### NativeLiveStreamFecOptions · "max_source_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6830)

```cpp
def_readwrite("max_source_bytes",
&nsf::LiveStreamFecOptions::maxSourceBytes)
```

### NativeLiveStreamFecOptions · "source_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6827)

```cpp
def_property("source_items",
[] (const nsf::LiveStreamFecOptions& value) { implementation omitted },
[] (nsf::LiveStreamFecOptions& value, size_t count) { implementation omitted })
```

### NativeLiveStreamFecOptions · "max_source_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6826)

```cpp
def_readwrite("max_source_items",
&nsf::LiveStreamFecOptions::maxSourceItems)
```

### NativeLiveStreamFecOptions · "scheme"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6825)

```cpp
def_readwrite("scheme",
&nsf::LiveStreamFecOptions::scheme)
```

### NativeStreamAdvancedOptions · "startup_timeout_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6861)

```cpp
def_readwrite("startup_timeout_ms",
&nsf::StreamAdvancedOptions::startupTimeoutMs)
```

### NativeStreamAdvancedOptions · "signed_wire_cap"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6859)

```cpp
def_readwrite("signed_wire_cap",
&nsf::StreamAdvancedOptions::signedWireCap)
```

### NativeStreamAdvancedOptions · "max_pending_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6857)

```cpp
def_readwrite("max_pending_interests",
&nsf::StreamAdvancedOptions::maxPendingInterests)
```

### NativeStreamAdvancedOptions · "max_name_reservations"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6855)

```cpp
def_readwrite("max_name_reservations",
&nsf::StreamAdvancedOptions::maxNameReservations)
```

### NativeStreamAdvancedOptions · "retained_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6853)

```cpp
def_readwrite("retained_items",
&nsf::StreamAdvancedOptions::retainedItems)
```

### NativeStreamAdvancedOptions · "mapping_ahead_blocks"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6851)

```cpp
def_readwrite("mapping_ahead_blocks",
&nsf::StreamAdvancedOptions::mappingAheadBlocks)
```

### NativeStreamAdvancedOptions · "mapping_block_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6849)

```cpp
def_readwrite("mapping_block_capacity",
&nsf::StreamAdvancedOptions::mappingBlockCapacity)
```

### NativeStreamConfig · "advanced"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6876)

```cpp
def_readwrite("advanced",
&nsf::StreamConfig::advanced)
```

### NativeStreamConfig · "session_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6875)

```cpp
def_readwrite("session_epoch",
&nsf::StreamConfig::sessionEpoch)
```

### NativeStreamConfig · "fec"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6874)

```cpp
def_readwrite("fec",
&nsf::StreamConfig::fec)
```

### NativeStreamConfig · "sample_classes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6873)

```cpp
def_readwrite("sample_classes",
&nsf::StreamConfig::sampleClasses)
```

### NativeStreamConfig · "sample_period_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6872)

```cpp
def_readwrite("sample_period_ms",
&nsf::StreamConfig::samplePeriodMs)
```

### NativeStreamConfig · "data_prefix"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6867)

```cpp
def_property("data_prefix",
[] (const nsf::StreamConfig& value) { implementation omitted },
[] (nsf::StreamConfig& value, const std::string& name) { implementation omitted })
```

### NativeStreamConfig · "stream_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6866)

```cpp
def_readwrite("stream_id",
&nsf::StreamConfig::streamId)
```

### NativeLiveStreamDefinition · "validate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6902)

```cpp
def("validate",
[] (const nsf::LiveStreamDefinition& value) -> py::object { implementation omitted })
```

### NativeLiveStreamDefinition · "mapping_root"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6899)

```cpp
def_property_readonly("mapping_root",
[] (const nsf::LiveStreamDefinition& value) { implementation omitted })
```

### NativeLiveStreamDefinition · "fec"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6898)

```cpp
def_readwrite("fec",
&nsf::LiveStreamDefinition::fec)
```

### NativeLiveStreamDefinition · "sample_classes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6897)

```cpp
def_readwrite("sample_classes",
&nsf::LiveStreamDefinition::sampleClasses)
```

### NativeLiveStreamDefinition · "sample_period_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6896)

```cpp
def_readwrite("sample_period_ms",
&nsf::LiveStreamDefinition::samplePeriodMs)
```

### NativeLiveStreamDefinition · "signed_wire_cap"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6895)

```cpp
def_readwrite("signed_wire_cap",
&nsf::LiveStreamDefinition::signedWireCap)
```

### NativeLiveStreamDefinition · "max_pending_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6894)

```cpp
def_readwrite("max_pending_interests",
&nsf::LiveStreamDefinition::maxPendingInterests)
```

### NativeLiveStreamDefinition · "max_name_reservations"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6893)

```cpp
def_readwrite("max_name_reservations",
&nsf::LiveStreamDefinition::maxNameReservations)
```

### NativeLiveStreamDefinition · "retained_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6892)

```cpp
def_readwrite("retained_items",
&nsf::LiveStreamDefinition::retainedItems)
```

### NativeLiveStreamDefinition · "mapping_ahead_blocks"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6891)

```cpp
def_readwrite("mapping_ahead_blocks",
&nsf::LiveStreamDefinition::mappingAheadBlocks)
```

### NativeLiveStreamDefinition · "mapping_block_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6890)

```cpp
def_readwrite("mapping_block_capacity",
&nsf::LiveStreamDefinition::mappingBlockCapacity)
```

### NativeLiveStreamDefinition · "mapping_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6889)

```cpp
def_readwrite("mapping_version",
&nsf::LiveStreamDefinition::mappingVersion)
```

### NativeLiveStreamDefinition · "session_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6888)

```cpp
def_readwrite("session_epoch",
&nsf::LiveStreamDefinition::sessionEpoch)
```

### NativeLiveStreamDefinition · "semantic_data_prefix"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6885)

```cpp
def_property("semantic_data_prefix",
[] (const nsf::LiveStreamDefinition& value) { implementation omitted },
[] (nsf::LiveStreamDefinition& value, const std::string& name) { implementation omitted })
```

### NativeLiveStreamDefinition · "provider"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6882)

```cpp
def_property("provider",
[] (const nsf::LiveStreamDefinition& value) { implementation omitted },
[] (nsf::LiveStreamDefinition& value, const std::string& name) { implementation omitted })
```

### NativeLiveStreamDefinition · "stream_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6881)

```cpp
def_readwrite("stream_id",
&nsf::LiveStreamDefinition::streamId)
```

### NativeLiveStreamDefinition · "contract_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6880)

```cpp
def_readwrite("contract_version",
&nsf::LiveStreamDefinition::contractVersion)
```

### NativeLiveStreamItemReservation · "mapping_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6914)

```cpp
def_readonly("mapping_version",
&nsf::LiveStreamItemReservation::mappingVersion)
```

### NativeLiveStreamItemReservation · "session_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6913)

```cpp
def_readonly("session_epoch",
&nsf::LiveStreamItemReservation::sessionEpoch)
```

### NativeLiveStreamItemReservation · "original_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6910)

```cpp
def_property_readonly("original_name",
[] (const nsf::LiveStreamItemReservation& value) { implementation omitted })
```

### NativeLiveStreamItemReservation · "cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6909)

```cpp
def_readonly("cursor",
&nsf::LiveStreamItemReservation::cursor)
```

### NativeLiveStreamGroupReservation · "repairs"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6921)

```cpp
def_readonly("repairs",
&nsf::LiveStreamGroupReservation::repairs)
```

### NativeLiveStreamGroupReservation · "sources"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6920)

```cpp
def_readonly("sources",
&nsf::LiveStreamGroupReservation::sources)
```

### NativeLiveStreamGroupReservation · "group_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6917)

```cpp
def_property_readonly("group_id",
[] (const nsf::LiveStreamGroupReservation& value) { implementation omitted })
```

### NativeLiveStreamItemKind · "REPAIR"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6925)

```cpp
value("REPAIR",
nsf::LiveStreamItemKind::Repair)
```

### NativeLiveStreamItemKind · "SOURCE"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6924)

```cpp
value("SOURCE",
nsf::LiveStreamItemKind::Source)
```

### NativeLiveStreamSampleReservation · "group"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6933)

```cpp
def_readonly("group",
&nsf::LiveStreamSampleReservation::group)
```

### NativeLiveStreamSampleReservation · "predicted_source_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6931)

```cpp
def_readonly("predicted_source_items",
&nsf::LiveStreamSampleReservation::predictedSourceItems)
```

### NativeLiveStreamSampleReservation · "sample_class"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6930)

```cpp
def_readonly("sample_class",
&nsf::LiveStreamSampleReservation::sampleClass)
```

### NativeLiveStreamSampleReservation · "sample_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6929)

```cpp
def_readonly("sample_id",
&nsf::LiveStreamSampleReservation::sampleId)
```

### NativeLiveStreamReadiness · "safe_join_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6938)

```cpp
def_readwrite("safe_join_cursor",
&nsf::LiveStreamReadiness::safeJoinCursor)
```

### NativeLiveStreamReadiness · "measured_sample_period_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6937)

```cpp
def_readwrite("measured_sample_period_ms",
&nsf::LiveStreamReadiness::measuredSamplePeriodMs)
```

### NativeLiveStreamDescriptor · "validate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6946)

```cpp
def("validate",
[] (const nsf::LiveStreamDescriptor& value) -> py::object { implementation omitted })
```

### NativeLiveStreamDescriptor · "safe_join_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6945)

```cpp
def_readwrite("safe_join_cursor",
&nsf::LiveStreamDescriptor::safeJoinCursor)
```

### NativeLiveStreamDescriptor · "measured_sample_period_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6944)

```cpp
def_readwrite("measured_sample_period_ms",
&nsf::LiveStreamDescriptor::measuredSamplePeriodMs)
```

### NativeLiveStreamDescriptor · "checkpoint"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6943)

```cpp
def_readwrite("checkpoint",
&nsf::LiveStreamDescriptor::checkpoint)
```

### NativeLiveStreamDescriptor · "definition"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6942)

```cpp
def_readwrite("definition",
&nsf::LiveStreamDescriptor::definition)
```

### NativeLiveStreamLifecycleState · "FAILED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6955)

```cpp
value("FAILED",
nsf::LiveStreamLifecycleState::Failed)
```

### NativeLiveStreamLifecycleState · "STOPPED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6954)

```cpp
value("STOPPED",
nsf::LiveStreamLifecycleState::Stopped)
```

### NativeLiveStreamLifecycleState · "ACTIVE"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6953)

```cpp
value("ACTIVE",
nsf::LiveStreamLifecycleState::Active)
```

### NativeLiveStreamLifecycleState · "PREPARING"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6952)

```cpp
value("PREPARING",
nsf::LiveStreamLifecycleState::Preparing)
```

### NativeLiveStreamItemProvenance · "FEC_RECOVERED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6959)

```cpp
value("FEC_RECOVERED",
nsf::LiveStreamItemProvenance::FecRecovered)
```

### NativeLiveStreamItemProvenance · "SIGNED_DATA"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6958)

```cpp
value("SIGNED_DATA",
nsf::LiveStreamItemProvenance::SignedData)
```

### NativeVerifiedLiveStreamItem · "received_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6973)

```cpp
def_readonly("received_ms",
&nsf::VerifiedLiveStreamItem::receivedMs)
```

### NativeVerifiedLiveStreamItem · "provenance"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6972)

```cpp
def_readonly("provenance",
&nsf::VerifiedLiveStreamItem::provenance)
```

### NativeVerifiedLiveStreamItem · "content"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6969)

```cpp
def_property_readonly("content",
[] (const nsf::VerifiedLiveStreamItem& value) { implementation omitted })
```

### NativeVerifiedLiveStreamItem · "verified_provider"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6966)

```cpp
def_property_readonly("verified_provider",
[] (const nsf::VerifiedLiveStreamItem& value) { implementation omitted })
```

### NativeVerifiedLiveStreamItem · "original_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6963)

```cpp
def_property_readonly("original_name",
[] (const nsf::VerifiedLiveStreamItem& value) { implementation omitted })
```

### NativeVerifiedLiveStreamItem · "cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6962)

```cpp
def_readonly("cursor",
&nsf::VerifiedLiveStreamItem::cursor)
```

### NativeLiveStreamItemAdmission · "reject_item"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6979)

```cpp
def_static("reject_item",
&nsf::LiveStreamItemAdmission::rejectItem)
```

### NativeLiveStreamItemAdmission · "accept_item"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6978)

```cpp
def_static("accept_item",
&nsf::LiveStreamItemAdmission::acceptItem)
```

### NativeLiveStreamItemAdmission · "reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6977)

```cpp
def_readonly("reason",
&nsf::LiveStreamItemAdmission::reason)
```

### NativeLiveStreamItemAdmission · "accepted"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6976)

```cpp
def_readonly("accepted",
&nsf::LiveStreamItemAdmission::accepted)
```

### NativeLiveStreamSampleObservation · "item_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6986)

```cpp
def_readwrite("item_count",
&nsf::LiveStreamSampleObservation::itemCount)
```

### NativeLiveStreamSampleObservation · "retrieval_delay_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6985)

```cpp
def_readwrite("retrieval_delay_ms",
&nsf::LiveStreamSampleObservation::retrievalDelayMs)
```

### NativeLiveStreamSampleObservation · "arrival_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6984)

```cpp
def_readwrite("arrival_ms",
&nsf::LiveStreamSampleObservation::arrivalMs)
```

### NativeLiveStreamSampleObservation · "sample_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6983)

```cpp
def_readwrite("sample_id",
&nsf::LiveStreamSampleObservation::sampleId)
```

### NativeLiveStreamStatus · "fetch_decision"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7098)

```cpp
def_readonly("fetch_decision",
&nsf::LiveStreamStatus::fetchDecision)
```

### NativeLiveStreamStatus · "reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7097)

```cpp
def_readonly("reason",
&nsf::LiveStreamStatus::reason)
```

### NativeLiveStreamStatus · "sample_class_predictions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7095)

```cpp
def_readonly("sample_class_predictions",
&nsf::LiveStreamStatus::sampleClassPredictions)
```

### NativeLiveStreamStatus · "provider_retry_future_hits"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7093)

```cpp
def_readonly("provider_retry_future_hits",
&nsf::LiveStreamStatus::providerRetryFutureHits)
```

### NativeLiveStreamStatus · "provider_retry_future_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7091)

```cpp
def_readonly("provider_retry_future_interests",
&nsf::LiveStreamStatus::providerRetryFutureInterests)
```

### NativeLiveStreamStatus · "provider_initial_future_hits"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7089)

```cpp
def_readonly("provider_initial_future_hits",
&nsf::LiveStreamStatus::providerInitialFutureHits)
```

### NativeLiveStreamStatus · "provider_initial_future_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7087)

```cpp
def_readonly("provider_initial_future_interests",
&nsf::LiveStreamStatus::providerInitialFutureInterests)
```

### NativeLiveStreamStatus · "provider_future_hits"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7086)

```cpp
def_readonly("provider_future_hits",
&nsf::LiveStreamStatus::providerFutureHits)
```

### NativeLiveStreamStatus · "provider_future_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7085)

```cpp
def_readonly("provider_future_interests",
&nsf::LiveStreamStatus::providerFutureInterests)
```

### NativeLiveStreamStatus · "mapping_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7084)

```cpp
def_readonly("mapping_bytes",
&nsf::LiveStreamStatus::mappingBytes)
```

### NativeLiveStreamStatus · "terminal_gap_superseded"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7082)

```cpp
def_readonly("terminal_gap_superseded",
&nsf::LiveStreamStatus::terminalGapSuperseded)
```

### NativeLiveStreamStatus · "stale_ready_drops"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7080)

```cpp
def_readonly("stale_ready_drops",
&nsf::LiveStreamStatus::staleReadyDrops)
```

### NativeLiveStreamStatus · "drain_wake_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7078)

```cpp
def_readonly("drain_wake_count",
&nsf::LiveStreamStatus::drainWakeCount)
```

### NativeLiveStreamStatus · "terminal_gap_queue_depth"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7076)

```cpp
def_readonly("terminal_gap_queue_depth",
&nsf::LiveStreamStatus::terminalGapQueueDepth)
```

### NativeLiveStreamStatus · "oldest_ready_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7074)

```cpp
def_readonly("oldest_ready_cursor",
&nsf::LiveStreamStatus::oldestReadyCursor)
```

### NativeLiveStreamStatus · "ready_queue_depth"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7072)

```cpp
def_readonly("ready_queue_depth",
&nsf::LiveStreamStatus::readyQueueDepth)
```

### NativeLiveStreamStatus · "next_deliver_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7070)

```cpp
def_readonly("next_deliver_cursor",
&nsf::LiveStreamStatus::nextDeliverCursor)
```

### NativeLiveStreamStatus · "recovery_metadata_cache_hits"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7068)

```cpp
def_readonly("recovery_metadata_cache_hits",
&nsf::LiveStreamStatus::recoveryMetadataCacheHits)
```

### NativeLiveStreamStatus · "recovery_coalesced_waiters"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7066)

```cpp
def_readonly("recovery_coalesced_waiters",
&nsf::LiveStreamStatus::recoveryCoalescedWaiters)
```

### NativeLiveStreamStatus · "recovery_group_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7064)

```cpp
def_readonly("recovery_group_interests",
&nsf::LiveStreamStatus::recoveryGroupInterests)
```

### NativeLiveStreamStatus · "recovery_frontier_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7062)

```cpp
def_readonly("recovery_frontier_interests",
&nsf::LiveStreamStatus::recoveryFrontierInterests)
```

### NativeLiveStreamStatus · "recovery_control_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7060)

```cpp
def_readonly("recovery_control_interests",
&nsf::LiveStreamStatus::recoveryControlInterests)
```

### NativeLiveStreamStatus · "recovery_exhaustions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7059)

```cpp
def_readonly("recovery_exhaustions",
&nsf::LiveStreamStatus::recoveryExhaustions)
```

### NativeLiveStreamStatus · "recovery_attempts"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7058)

```cpp
def_readonly("recovery_attempts",
&nsf::LiveStreamStatus::recoveryAttempts)
```

### NativeLiveStreamStatus · "recovered_groups"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7056)

```cpp
def_readonly("recovered_groups",
&nsf::LiveStreamStatus::recoveredGroups)
```

### NativeLiveStreamStatus · "recoverable_groups"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7054)

```cpp
def_readonly("recoverable_groups",
&nsf::LiveStreamStatus::recoverableGroups)
```

### NativeLiveStreamStatus · "terminal_missing_sources"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7052)

```cpp
def_readonly("terminal_missing_sources",
&nsf::LiveStreamStatus::terminalMissingSources)
```

### NativeLiveStreamStatus · "recovery_eligible_sources"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7050)

```cpp
def_readonly("recovery_eligible_sources",
&nsf::LiveStreamStatus::recoveryEligibleSources)
```

### NativeLiveStreamStatus · "declared_recovery_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7048)

```cpp
def_readonly("declared_recovery_capacity",
&nsf::LiveStreamStatus::declaredRecoveryCapacity)
```

### NativeLiveStreamStatus · "retry_suppression_reasons"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7046)

```cpp
def_readonly("retry_suppression_reasons",
&nsf::LiveStreamStatus::retrySuppressionReasons)
```

### NativeLiveStreamStatus · "retry_suppressions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7045)

```cpp
def_readonly("retry_suppressions",
&nsf::LiveStreamStatus::retrySuppressions)
```

### NativeLiveStreamStatus · "retry_successes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7044)

```cpp
def_readonly("retry_successes",
&nsf::LiveStreamStatus::retrySuccesses)
```

### NativeLiveStreamStatus · "future_cursor_horizon"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7042)

```cpp
def_readonly("future_cursor_horizon",
&nsf::LiveStreamStatus::futureCursorHorizon)
```

### NativeLiveStreamStatus · "retry_future_payload_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7040)

```cpp
def_readonly("retry_future_payload_interests",
&nsf::LiveStreamStatus::retryFuturePayloadInterests)
```

### NativeLiveStreamStatus · "initial_future_payload_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7038)

```cpp
def_readonly("initial_future_payload_interests",
&nsf::LiveStreamStatus::initialFuturePayloadInterests)
```

### NativeLiveStreamStatus · "future_payload_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7037)

```cpp
def_readonly("future_payload_interests",
&nsf::LiveStreamStatus::futurePayloadInterests)
```

### NativeLiveStreamStatus · "payload_unresolved_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7035)

```cpp
def_readonly("payload_unresolved_interests",
&nsf::LiveStreamStatus::payloadUnresolvedInterests)
```

### NativeLiveStreamStatus · "payload_nonproductive_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7033)

```cpp
def_readonly("payload_nonproductive_interests",
&nsf::LiveStreamStatus::payloadNonproductiveInterests)
```

### NativeLiveStreamStatus · "payload_protection_only_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7031)

```cpp
def_readonly("payload_protection_only_interests",
&nsf::LiveStreamStatus::payloadProtectionOnlyInterests)
```

### NativeLiveStreamStatus · "payload_application_useful_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7029)

```cpp
def_readonly("payload_application_useful_interests",
&nsf::LiveStreamStatus::payloadApplicationUsefulInterests)
```

### NativeLiveStreamStatus · "payload_repair_data_consumed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7027)

```cpp
def_readonly("payload_repair_data_consumed",
&nsf::LiveStreamStatus::payloadRepairDataConsumed)
```

### NativeLiveStreamStatus · "payload_repair_data_responses"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7025)

```cpp
def_readonly("payload_repair_data_responses",
&nsf::LiveStreamStatus::payloadRepairDataResponses)
```

### NativeLiveStreamStatus · "payload_source_data_admissions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7023)

```cpp
def_readonly("payload_source_data_admissions",
&nsf::LiveStreamStatus::payloadSourceDataAdmissions)
```

### NativeLiveStreamStatus · "payload_unclassified_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7021)

```cpp
def_readonly("payload_unclassified_interests",
&nsf::LiveStreamStatus::payloadUnclassifiedInterests)
```

### NativeLiveStreamStatus · "retry_payload_repair_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7019)

```cpp
def_readonly("retry_payload_repair_interests",
&nsf::LiveStreamStatus::retryPayloadRepairInterests)
```

### NativeLiveStreamStatus · "initial_payload_repair_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7017)

```cpp
def_readonly("initial_payload_repair_interests",
&nsf::LiveStreamStatus::initialPayloadRepairInterests)
```

### NativeLiveStreamStatus · "payload_repair_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7016)

```cpp
def_readonly("payload_repair_interests",
&nsf::LiveStreamStatus::payloadRepairInterests)
```

### NativeLiveStreamStatus · "retry_payload_source_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7014)

```cpp
def_readonly("retry_payload_source_interests",
&nsf::LiveStreamStatus::retryPayloadSourceInterests)
```

### NativeLiveStreamStatus · "initial_payload_source_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7012)

```cpp
def_readonly("initial_payload_source_interests",
&nsf::LiveStreamStatus::initialPayloadSourceInterests)
```

### NativeLiveStreamStatus · "payload_source_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7011)

```cpp
def_readonly("payload_source_interests",
&nsf::LiveStreamStatus::payloadSourceInterests)
```

### NativeLiveStreamStatus · "retry_payload_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7010)

```cpp
def_readonly("retry_payload_interests",
&nsf::LiveStreamStatus::retryPayloadInterests)
```

### NativeLiveStreamStatus · "initial_payload_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7009)

```cpp
def_readonly("initial_payload_interests",
&nsf::LiveStreamStatus::initialPayloadInterests)
```

### NativeLiveStreamStatus · "payload_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7008)

```cpp
def_readonly("payload_interests",
&nsf::LiveStreamStatus::payloadInterests)
```

### NativeLiveStreamStatus · "mapping_new_data_responses"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7006)

```cpp
def_readonly("mapping_new_data_responses",
&nsf::LiveStreamStatus::mappingNewDataResponses)
```

### NativeLiveStreamStatus · "mapping_data_responses"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7005)

```cpp
def_readonly("mapping_data_responses",
&nsf::LiveStreamStatus::mappingDataResponses)
```

### NativeLiveStreamStatus · "mapping_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7004)

```cpp
def_readonly("mapping_interests",
&nsf::LiveStreamStatus::mappingInterests)
```

### NativeLiveStreamStatus · "retry_exhaustions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7003)

```cpp
def_readonly("retry_exhaustions",
&nsf::LiveStreamStatus::retryExhaustions)
```

### NativeLiveStreamStatus · "deadline_skips"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7002)

```cpp
def_readonly("deadline_skips",
&nsf::LiveStreamStatus::deadlineSkips)
```

### NativeLiveStreamStatus · "late_arrivals"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7001)

```cpp
def_readonly("late_arrivals",
&nsf::LiveStreamStatus::lateArrivals)
```

### NativeLiveStreamStatus · "retry_attempts"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7000)

```cpp
def_readonly("retry_attempts",
&nsf::LiveStreamStatus::retryAttempts)
```

### NativeLiveStreamStatus · "nacks"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6999)

```cpp
def_readonly("nacks",
&nsf::LiveStreamStatus::nacks)
```

### NativeLiveStreamStatus · "timeouts"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6998)

```cpp
def_readonly("timeouts",
&nsf::LiveStreamStatus::timeouts)
```

### NativeLiveStreamStatus · "recovered"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6997)

```cpp
def_readonly("recovered",
&nsf::LiveStreamStatus::recovered)
```

### NativeLiveStreamStatus · "rejected"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6996)

```cpp
def_readonly("rejected",
&nsf::LiveStreamStatus::rejected)
```

### NativeLiveStreamStatus · "delivered"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6995)

```cpp
def_readonly("delivered",
&nsf::LiveStreamStatus::delivered)
```

### NativeLiveStreamStatus · "in_flight"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6994)

```cpp
def_readonly("in_flight",
&nsf::LiveStreamStatus::inFlight)
```

### NativeLiveStreamStatus · "mapping_blocks"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6993)

```cpp
def_readonly("mapping_blocks",
&nsf::LiveStreamStatus::mappingBlocks)
```

### NativeLiveStreamStatus · "pending_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6992)

```cpp
def_readonly("pending_interests",
&nsf::LiveStreamStatus::pendingInterests)
```

### NativeLiveStreamStatus · "retained_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6991)

```cpp
def_readonly("retained_items",
&nsf::LiveStreamStatus::retainedItems)
```

### NativeLiveStreamStatus · "frontiers"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6990)

```cpp
def_readonly("frontiers",
&nsf::LiveStreamStatus::frontiers)
```

### NativeLiveStreamStatus · "state"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6989)

```cpp
def_readonly("state",
&nsf::LiveStreamStatus::state)
```

### NativePublishedLiveStreamPacketKind · "REPAIR"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7103)

```cpp
value("REPAIR",
nsf::PublishedLiveStreamPacketKind::Repair)
```

### NativePublishedLiveStreamPacketKind · "SOURCE"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7102)

```cpp
value("SOURCE",
nsf::PublishedLiveStreamPacketKind::Source)
```

### NativePublishedLiveStreamPacketKind · "MAPPING"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7101)

```cpp
value("MAPPING",
nsf::PublishedLiveStreamPacketKind::Mapping)
```

### NativePublishedLiveStreamPacket · "materialized_monotonic_us"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7126)

```cpp
def_readonly("materialized_monotonic_us",
&nsf::PublishedLiveStreamPacket::materializedMonotonicUs)
```

### NativePublishedLiveStreamPacket · "wire_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7123)

```cpp
def_property_readonly("wire_digest",
[] (const nsf::PublishedLiveStreamPacket& value) { implementation omitted })
```

### NativePublishedLiveStreamPacket · "signed_data_wire"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7119)

```cpp
def_property_readonly("signed_data_wire",
[] (const nsf::PublishedLiveStreamPacket& value) { implementation omitted })
```

### NativePublishedLiveStreamPacket · "provider"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7116)

```cpp
def_property_readonly("provider",
[] (const nsf::PublishedLiveStreamPacket& value) { implementation omitted })
```

### NativePublishedLiveStreamPacket · "data_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7113)

```cpp
def_property_readonly("data_name",
[] (const nsf::PublishedLiveStreamPacket& value) { implementation omitted })
```

### NativePublishedLiveStreamPacket · "cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7110)

```cpp
def_property_readonly("cursor",
[] (const nsf::PublishedLiveStreamPacket& value) -> py::object { implementation omitted })
```

### NativePublishedLiveStreamPacket · "mapping_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7109)

```cpp
def_readonly("mapping_version",
&nsf::PublishedLiveStreamPacket::mappingVersion)
```

### NativePublishedLiveStreamPacket · "session_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7108)

```cpp
def_readonly("session_epoch",
&nsf::PublishedLiveStreamPacket::sessionEpoch)
```

### NativePublishedLiveStreamPacket · "stream_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7107)

```cpp
def_readonly("stream_id",
&nsf::PublishedLiveStreamPacket::streamId)
```

### NativePublishedLiveStreamPacket · "kind"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7106)

```cpp
def_readonly("kind",
&nsf::PublishedLiveStreamPacket::kind)
```

### NativePublishedPacketFeedOptions · "max_queued_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7133)

```cpp
def_readwrite("max_queued_bytes",
&nsf::PublishedPacketFeedOptions::maxQueuedBytes)
```

### NativePublishedPacketFeedOptions · "max_queued_packets"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7132)

```cpp
def_readwrite("max_queued_packets",
&nsf::PublishedPacketFeedOptions::maxQueuedPackets)
```

### NativePublishedPacketFeedOptions · "from_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7131)

```cpp
def_readwrite("from_cursor",
&nsf::PublishedPacketFeedOptions::fromCursor)
```

### NativePublishedPacketFeedStatus · "closed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7141)

```cpp
def_readonly("closed",
&nsf::PublishedPacketFeedStatus::closed)
```

### NativePublishedPacketFeedStatus · "last_dropped_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7140)

```cpp
def_readonly("last_dropped_cursor",
&nsf::PublishedPacketFeedStatus::lastDroppedCursor)
```

### NativePublishedPacketFeedStatus · "first_dropped_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7139)

```cpp
def_readonly("first_dropped_cursor",
&nsf::PublishedPacketFeedStatus::firstDroppedCursor)
```

### NativePublishedPacketFeedStatus · "dropped_packets"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7138)

```cpp
def_readonly("dropped_packets",
&nsf::PublishedPacketFeedStatus::droppedPackets)
```

### NativePublishedPacketFeedStatus · "queued_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7137)

```cpp
def_readonly("queued_bytes",
&nsf::PublishedPacketFeedStatus::queuedBytes)
```

### NativePublishedPacketFeedStatus · "queued_packets"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7136)

```cpp
def_readonly("queued_packets",
&nsf::PublishedPacketFeedStatus::queuedPackets)
```

### NativePublishedPacketFeed · "close"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7147)

```cpp
def("close",
&nsf::PublishedPacketFeed::close)
```

### NativePublishedPacketFeed · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7146)

```cpp
def("status",
&nsf::PublishedPacketFeed::status)
```

### NativePublishedPacketFeed · "take_available"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7145)

```cpp
def("take_available",
&nsf::PublishedPacketFeed::takeAvailable)
```

### NativeLiveStreamPublisher · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7214)

```cpp
def("stop",
&nsf::LiveStreamPublisher::stop)
```

### NativeLiveStreamPublisher · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7213)

```cpp
def("status",
&nsf::LiveStreamPublisher::status)
```

### NativeLiveStreamPublisher · "open_published_packet_feed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7212)

```cpp
def("open_published_packet_feed",
&nsf::LiveStreamPublisher::openPublishedPacketFeed)
```

### NativeLiveStreamPublisher · "activate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7211)

```cpp
def("activate",
&nsf::LiveStreamPublisher::activate)
```

### NativeLiveStreamPublisher · "publish_sample"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7201)

```cpp
def("publish_sample",
[] (nsf::LiveStreamPublisher& publisher,
                                 const nsf::LiveStreamSampleReservation& reservation,
                                 const std::vector<py::bytes>& contents) { implementation omitted })
```

### NativeLiveStreamPublisher · "publish_group"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7191)

```cpp
def("publish_group",
[] (nsf::LiveStreamPublisher& publisher,
                               const nsf::LiveStreamGroupReservation& reservation,
                               const std::vector<py::bytes>& contents) { implementation omitted })
```

### NativeLiveStreamPublisher · "publish"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7185)

```cpp
def("publish",
[] (nsf::LiveStreamPublisher& publisher,
                         const nsf::LiveStreamItemReservation& reservation,
                         const py::bytes& content) { implementation omitted })
```

### NativeLiveStreamPublisher · "prepare_sample_extent"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7183)

```cpp
def("prepare_sample_extent",
&nsf::LiveStreamPublisher::prepareSampleExtent,
py::arg("reservation"),
py::arg("actual_source_items"))
```

### NativeLiveStreamPublisher · "announce_sample"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7170)

```cpp
def("announce_sample",
[] (nsf::LiveStreamPublisher& publisher,
                                  uint64_t sampleId,
                                  const std::string& sampleClass,
                                  const py::function& nameFactory) { implementation omitted },
py::arg("sample_id"),
py::arg("sample_class"),
py::arg("name_factory"))
```

### NativeLiveStreamPublisher · "reserve_group"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7160)

```cpp
def("reserve_group",
[] (nsf::LiveStreamPublisher& publisher,
                               const std::string& groupId,
                               const std::vector<std::string>& sourceNames,
                               const std::vector<std::string>& repairNames) { implementation omitted })
```

### NativeLiveStreamPublisher · "reserve_many_ahead"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7154)

```cpp
def("reserve_many_ahead",
[] (nsf::LiveStreamPublisher& publisher,
                                    const std::vector<std::string>& names) { implementation omitted })
```

### NativeLiveStreamPublisher · "reserve_ahead"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7151)

```cpp
def("reserve_ahead",
[] (nsf::LiveStreamPublisher& publisher, const std::string& name) { implementation omitted })
```

### NativeStreamPublisher · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7231)

```cpp
def("stop",
&nsf::StreamPublisher::stop)
```

### NativeStreamPublisher · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7230)

```cpp
def("status",
&nsf::StreamPublisher::status)
```

### NativeStreamPublisher · "flush"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7229)

```cpp
def("flush",
&nsf::StreamPublisher::flush)
```

### NativeStreamPublisher · "push"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7222)

```cpp
def("push",
[] (nsf::StreamPublisher& publisher, const py::bytes& data) { implementation omitted },
py::arg("signed_data"))
```

### NativeStreamPublisher · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7218)

```cpp
def("start",
[] (nsf::StreamPublisher& publisher) { implementation omitted })
```

### NativePredictiveStreamCheckpoint · "next_expected_sample_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7242)

```cpp
def_readwrite("next_expected_sample_id",
&nsf::PredictiveStreamCheckpoint::nextExpectedSampleId)
```

### NativePredictiveStreamCheckpoint · "latest_produced_sample_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7240)

```cpp
def_readwrite("latest_produced_sample_id",
&nsf::PredictiveStreamCheckpoint::latestProducedSampleId)
```

### NativePredictiveStreamCheckpoint · "oldest_retained_sample_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7238)

```cpp
def_readwrite("oldest_retained_sample_id",
&nsf::PredictiveStreamCheckpoint::oldestRetainedSampleId)
```

### NativePredictiveStreamCheckpoint · "initial_sample_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7236)

```cpp
def_readwrite("initial_sample_id",
&nsf::PredictiveStreamCheckpoint::initialSampleId)
```

### NativePredictiveStreamDescriptor · "measured_sample_period_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7276)

```cpp
def_property_readonly("measured_sample_period_ms",
[] (const nsf::PredictiveStreamDescriptor& d) { implementation omitted })
```

### NativePredictiveStreamDescriptor · "frontier_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7272)

```cpp
def_property_readonly("frontier_name",
[] (const nsf::PredictiveStreamDescriptor& d) { implementation omitted })
```

### NativePredictiveStreamDescriptor · "checkpoint"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7268)

```cpp
def_property_readonly("checkpoint",
[] (const nsf::PredictiveStreamDescriptor& d) { implementation omitted })
```

### NativePredictiveStreamDescriptor · "definition"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7264)

```cpp
def_property_readonly("definition",
[] (const nsf::PredictiveStreamDescriptor& d) { implementation omitted })
```

### NativeLiveStreamConsumerHandle · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7286)

```cpp
def("stop",
&nsf::LiveStreamConsumerHandle::stop)
```

### NativeLiveStreamConsumerHandle · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7285)

```cpp
def("status",
&nsf::LiveStreamConsumerHandle::status)
```

### NativeLiveStreamConsumerHandle · "observe_accepted_sample"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7284)

```cpp
def("observe_accepted_sample",
&nsf::LiveStreamConsumerHandle::observeAcceptedSample)
```

### NativeLiveStreamConsumerHandle · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7283)

```cpp
def("start",
&nsf::LiveStreamConsumerHandle::start)
```

### NativePredictiveStreamSubscriber · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7293)

```cpp
def("stop",
&nsf::PredictiveStreamSubscriber::stop)
```

### NativePredictiveStreamSubscriber · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7292)

```cpp
def("status",
&nsf::PredictiveStreamSubscriber::status)
```

### NativePredictiveStreamSubscriber · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7291)

```cpp
def("start",
&nsf::PredictiveStreamSubscriber::start)
```

### ExecutionLeaseState · "EXPIRED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7301)

```cpp
value("EXPIRED",
nsf::ExecutionLeaseState::Expired)
```

### ExecutionLeaseState · "RELEASED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7300)

```cpp
value("RELEASED",
nsf::ExecutionLeaseState::Released)
```

### ExecutionLeaseState · "ABORTED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7299)

```cpp
value("ABORTED",
nsf::ExecutionLeaseState::Aborted)
```

### ExecutionLeaseState · "EXECUTING"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7298)

```cpp
value("EXECUTING",
nsf::ExecutionLeaseState::Executing)
```

### ExecutionLeaseState · "COMMITTED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7297)

```cpp
value("COMMITTED",
nsf::ExecutionLeaseState::Committed)
```

### ExecutionLeaseState · "PREPARED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7296)

```cpp
value("PREPARED",
nsf::ExecutionLeaseState::Prepared)
```

### GenericExecutionLease · "idempotency_key"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7327)

```cpp
def_readwrite("idempotency_key",
&nsf::GenericExecutionLease::idempotencyKey)
```

### GenericExecutionLease · "execution_deadline_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7325)

```cpp
def_readwrite("execution_deadline_ms",
&nsf::GenericExecutionLease::executionDeadlineMs)
```

### GenericExecutionLease · "expires_at_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7324)

```cpp
def_readwrite("expires_at_ms",
&nsf::GenericExecutionLease::expiresAtMs)
```

### GenericExecutionLease · "state"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7323)

```cpp
def_readwrite("state",
&nsf::GenericExecutionLease::state)
```

### GenericExecutionLease · "conflict_keys"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7322)

```cpp
def_readwrite("conflict_keys",
&nsf::GenericExecutionLease::conflictKeys)
```

### GenericExecutionLease · "resource_binding_proof"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7315)

```cpp
def_property("resource_binding_proof",
[] (const nsf::GenericExecutionLease& lease) { implementation omitted },
[] (nsf::GenericExecutionLease& lease, const py::bytes& value) { implementation omitted })
```

### GenericExecutionLease · "resource_binding_schema"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7313)

```cpp
def_readwrite("resource_binding_schema",
&nsf::GenericExecutionLease::resourceBindingSchema)
```

### GenericExecutionLease · "plan_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7312)

```cpp
def_readwrite("plan_digest",
&nsf::GenericExecutionLease::planDigest)
```

### GenericExecutionLease · "service_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7311)

```cpp
def_readwrite("service_name",
&nsf::GenericExecutionLease::serviceName)
```

### GenericExecutionLease · "request_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7310)

```cpp
def_readwrite("request_id",
&nsf::GenericExecutionLease::requestId)
```

### GenericExecutionLease · "requester_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7309)

```cpp
def_readwrite("requester_name",
&nsf::GenericExecutionLease::requesterName)
```

### GenericExecutionLease · "provider_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7308)

```cpp
def_readwrite("provider_epoch",
&nsf::GenericExecutionLease::providerEpoch)
```

### GenericExecutionLease · "provider_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7307)

```cpp
def_readwrite("provider_name",
&nsf::GenericExecutionLease::providerName)
```

### GenericExecutionLease · "lease_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7306)

```cpp
def_readwrite("lease_id",
&nsf::GenericExecutionLease::leaseId)
```

### GenericExecutionLease · "schema"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7305)

```cpp
def_readwrite("schema",
&nsf::GenericExecutionLease::schema)
```

### ExecutionLeaseBinding · "resource_binding_proof"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7337)

```cpp
def_property("resource_binding_proof",
[] (const nsf::ExecutionLeaseBinding& binding) { implementation omitted },
[] (nsf::ExecutionLeaseBinding& binding, const py::bytes& value) { implementation omitted })
```

### ExecutionLeaseBinding · "resource_binding_schema"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7335)

```cpp
def_readwrite("resource_binding_schema",
&nsf::ExecutionLeaseBinding::resourceBindingSchema)
```

### ExecutionLeaseBinding · "plan_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7334)

```cpp
def_readwrite("plan_digest",
&nsf::ExecutionLeaseBinding::planDigest)
```

### ExecutionLeaseBinding · "service_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7333)

```cpp
def_readwrite("service_name",
&nsf::ExecutionLeaseBinding::serviceName)
```

### ExecutionLeaseBinding · "request_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7332)

```cpp
def_readwrite("request_id",
&nsf::ExecutionLeaseBinding::requestId)
```

### ExecutionLeaseBinding · "requester_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7331)

```cpp
def_readwrite("requester_name",
&nsf::ExecutionLeaseBinding::requesterName)
```

### ExecutionLeaseResult · "idempotent_replay"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7351)

```cpp
def_readonly("idempotent_replay",
&nsf::ExecutionLeaseResult::idempotentReplay)
```

### ExecutionLeaseResult · "retry_after_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7350)

```cpp
def_readonly("retry_after_ms",
&nsf::ExecutionLeaseResult::retryAfterMs)
```

### ExecutionLeaseResult · "lease"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7349)

```cpp
def_readonly("lease",
&nsf::ExecutionLeaseResult::lease)
```

### ExecutionLeaseResult · "reason_code"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7348)

```cpp
def_readonly("reason_code",
&nsf::ExecutionLeaseResult::reasonCode)
```

### ExecutionLeaseResult · "operation"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7347)

```cpp
def_readonly("operation",
&nsf::ExecutionLeaseResult::operation)
```

### ExecutionLeaseResult · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7346)

```cpp
def_readonly("status",
&nsf::ExecutionLeaseResult::status)
```

### ExecutionLeaseCounters · "active_executing"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7371)

```cpp
def_readonly("active_executing",
&nsf::ExecutionLeaseCounters::activeExecuting)
```

### ExecutionLeaseCounters · "active_committed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7370)

```cpp
def_readonly("active_committed",
&nsf::ExecutionLeaseCounters::activeCommitted)
```

### ExecutionLeaseCounters · "active_prepared"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7369)

```cpp
def_readonly("active_prepared",
&nsf::ExecutionLeaseCounters::activePrepared)
```

### ExecutionLeaseCounters · "rejected_by_reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7367)

```cpp
def_readonly("rejected_by_reason",
&nsf::ExecutionLeaseCounters::rejectedByReason)
```

### ExecutionLeaseCounters · "cleanup_timeout"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7366)

```cpp
def_readonly("cleanup_timeout",
&nsf::ExecutionLeaseCounters::cleanupTimeout)
```

### ExecutionLeaseCounters · "stale_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7365)

```cpp
def_readonly("stale_epoch",
&nsf::ExecutionLeaseCounters::staleEpoch)
```

### ExecutionLeaseCounters · "conflict"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7364)

```cpp
def_readonly("conflict",
&nsf::ExecutionLeaseCounters::conflict)
```

### ExecutionLeaseCounters · "idempotent_replay"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7362)

```cpp
def_readonly("idempotent_replay",
&nsf::ExecutionLeaseCounters::idempotentReplay)
```

### ExecutionLeaseCounters · "renewed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7361)

```cpp
def_readonly("renewed",
&nsf::ExecutionLeaseCounters::renewed)
```

### ExecutionLeaseCounters · "expired"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7360)

```cpp
def_readonly("expired",
&nsf::ExecutionLeaseCounters::expired)
```

### ExecutionLeaseCounters · "released"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7359)

```cpp
def_readonly("released",
&nsf::ExecutionLeaseCounters::released)
```

### ExecutionLeaseCounters · "aborted"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7358)

```cpp
def_readonly("aborted",
&nsf::ExecutionLeaseCounters::aborted)
```

### ExecutionLeaseCounters · "activated"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7357)

```cpp
def_readonly("activated",
&nsf::ExecutionLeaseCounters::activated)
```

### ExecutionLeaseCounters · "committed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7356)

```cpp
def_readonly("committed",
&nsf::ExecutionLeaseCounters::committed)
```

### ExecutionLeaseCounters · "prepared"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7355)

```cpp
def_readonly("prepared",
&nsf::ExecutionLeaseCounters::prepared)
```

### ProviderExecutionLeaseTable · "counters"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7417)

```cpp
def("counters",
&nsf::ProviderExecutionLeaseTable::counters,
py::arg("now_ms"))
```

### ProviderExecutionLeaseTable · "has_pinned_binding_proof"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7411)

```cpp
def("has_pinned_binding_proof",
[] (nsf::ProviderExecutionLeaseTable& table,
             const py::bytes& proof, uint64_t nowMs) { implementation omitted },
py::arg("resource_binding_proof"),
py::arg("now_ms"))
```

### ProviderExecutionLeaseTable · "has_active_conflict_key"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7408)

```cpp
def("has_active_conflict_key",
&nsf::ProviderExecutionLeaseTable::hasActiveConflictKey,
py::arg("conflict_key"),
py::arg("now_ms"))
```

### ProviderExecutionLeaseTable · "find"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7406)

```cpp
def("find",
&nsf::ProviderExecutionLeaseTable::find,
py::arg("lease_id"))
```

### ProviderExecutionLeaseTable · "cleanup_expired"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7404)

```cpp
def("cleanup_expired",
&nsf::ProviderExecutionLeaseTable::cleanupExpired,
py::arg("now_ms"))
```

### ProviderExecutionLeaseTable · "release"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7400)

```cpp
def("release",
&nsf::ProviderExecutionLeaseTable::release,
py::arg("lease_id"),
py::arg("provider_epoch"),
py::arg("requester_name"),
py::arg("idempotency_key"),
py::arg("now_ms"))
```

### ProviderExecutionLeaseTable · "renew"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7395)

```cpp
def("renew",
&nsf::ProviderExecutionLeaseTable::renew,
py::arg("lease_id"),
py::arg("provider_epoch"),
py::arg("requester_name"),
py::arg("idempotency_key"),
py::arg("now_ms"),
py::arg("expires_at_ms"))
```

### ProviderExecutionLeaseTable · "abort"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7391)

```cpp
def("abort",
&nsf::ProviderExecutionLeaseTable::abort,
py::arg("lease_id"),
py::arg("provider_epoch"),
py::arg("requester_name"),
py::arg("idempotency_key"),
py::arg("now_ms"))
```

### ProviderExecutionLeaseTable · "validate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7388)

```cpp
def("validate",
&nsf::ProviderExecutionLeaseTable::validate,
py::arg("lease_id"),
py::arg("provider_epoch"),
py::arg("binding"),
py::arg("now_ms"))
```

### ProviderExecutionLeaseTable · "validate_and_activate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7383)

```cpp
def("validate_and_activate",
&nsf::ProviderExecutionLeaseTable::validateAndActivate,
py::arg("lease_id"),
py::arg("provider_epoch"),
py::arg("binding"),
py::arg("idempotency_key"),
py::arg("now_ms"),
py::arg("execution_deadline_ms"))
```

### ProviderExecutionLeaseTable · "commit"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7379)

```cpp
def("commit",
&nsf::ProviderExecutionLeaseTable::commit,
py::arg("lease_id"),
py::arg("provider_epoch"),
py::arg("requester_name"),
py::arg("idempotency_key"),
py::arg("now_ms"))
```

### ProviderExecutionLeaseTable · "prepare"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7377)

```cpp
def("prepare",
&nsf::ProviderExecutionLeaseTable::prepare,
py::arg("lease"),
py::arg("now_ms"))
```

### ProviderExecutionLeaseTable · "provider_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7375)

```cpp
def_property_readonly("provider_epoch",
&nsf::ProviderExecutionLeaseTable::providerEpoch)
```

### m · "encode_large_data_reference_payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7420)

```cpp
def("encode_large_data_reference_payload",
[](const std::string& dataName,
           const std::string& objectType,
           const std::string& objectId,
           size_t plaintextSize,
           bool encrypted,
           const std::string& digest) { implementation omitted },
py::arg("data_name"),
py::arg("object_type") = "",
py::arg("object_id") = "",
py::arg("plaintext_size") = 0,
py::arg("encrypted") = true,
py::arg("digest") = "")
```

### m · "parse_large_data_reference_payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7444)

```cpp
def("parse_large_data_reference_payload",
[](const py::bytes& payload) -> py::object { implementation omitted },
py::arg("payload"))
```

### ServiceResponse · "wire_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7462)

```cpp
def_readwrite("wire_digest",
&PyServiceResponse::wireDigest)
```

### ServiceResponse · "signer_certificate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7461)

```cpp
def_readwrite("signer_certificate",
&PyServiceResponse::signerCertificate)
```

### ServiceResponse · "data_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7460)

```cpp
def_readwrite("data_name",
&PyServiceResponse::dataName)
```

### ServiceResponse · "request_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7459)

```cpp
def_readwrite("request_id",
&PyServiceResponse::requestId)
```

### ServiceResponse · "error"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7458)

```cpp
def_readwrite("error",
&PyServiceResponse::error)
```

### ServiceResponse · "payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7457)

```cpp
def_readwrite("payload",
&PyServiceResponse::payload)
```

### ServiceResponse · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7456)

```cpp
def_readwrite("status",
&PyServiceResponse::status)
```

### AckDecision · "pending_state_ttl_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7472)

```cpp
def_readwrite("pending_state_ttl_ms",
&PyAckDecision::pendingStateTtlMs)
```

### AckDecision · "selection_input_key_offer"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7471)

```cpp
def_readwrite("selection_input_key_offer",
&PyAckDecision::selectionInputKeyOffer)
```

### AckDecision · "reservation_lease"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7470)

```cpp
def_readwrite("reservation_lease",
&PyAckDecision::reservationLease)
```

### AckDecision · "suppress"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7469)

```cpp
def_readwrite("suppress",
&PyAckDecision::suppress)
```

### AckDecision · "message"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7468)

```cpp
def_readwrite("message",
&PyAckDecision::message)
```

### AckDecision · "payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7467)

```cpp
def_readwrite("payload",
&PyAckDecision::payload)
```

### AckDecision · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7466)

```cpp
def_readwrite("status",
&PyAckDecision::status)
```

### AckCandidate · "trust_schema_validated"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7488)

```cpp
def_readwrite("trust_schema_validated",
&PyAckCandidate::trustSchemaValidated)
```

### AckCandidate · "validated_wire_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7487)

```cpp
def_readwrite("validated_wire_digest",
&PyAckCandidate::validatedWireDigest)
```

### AckCandidate · "signer_key_locator"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7486)

```cpp
def_readwrite("signer_key_locator",
&PyAckCandidate::signerKeyLocator)
```

### AckCandidate · "signer_identity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7485)

```cpp
def_readwrite("signer_identity",
&PyAckCandidate::signerIdentity)
```

### AckCandidate · "selection_input_key_offer"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7483)

```cpp
def_readwrite("selection_input_key_offer",
&PyAckCandidate::selectionInputKeyOffer)
```

### AckCandidate · "telemetry"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7482)

```cpp
def_readwrite("telemetry",
&PyAckCandidate::telemetry)
```

### AckCandidate · "payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7481)

```cpp
def_readwrite("payload",
&PyAckCandidate::payload)
```

### AckCandidate · "message"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7480)

```cpp
def_readwrite("message",
&PyAckCandidate::message)
```

### AckCandidate · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7479)

```cpp
def_readwrite("status",
&PyAckCandidate::status)
```

### AckCandidate · "request_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7478)

```cpp
def_readwrite("request_id",
&PyAckCandidate::requestId)
```

### AckCandidate · "service_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7477)

```cpp
def_readwrite("service_name",
&PyAckCandidate::serviceName)
```

### AckCandidate · "provider_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7476)

```cpp
def_readwrite("provider_name",
&PyAckCandidate::providerName)
```

### CollaborationAckClosure · "request_deadline_us"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7496)

```cpp
def_readwrite("request_deadline_us",
&PyCollaborationAckClosure::requestDeadlineUs)
```

### CollaborationAckClosure · "closed_at_us"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7495)

```cpp
def_readwrite("closed_at_us",
&PyCollaborationAckClosure::closedAtUs)
```

### CollaborationAckClosure · "digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7494)

```cpp
def_readwrite("digest",
&PyCollaborationAckClosure::digest)
```

### CollaborationAckClosure · "candidates"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7493)

```cpp
def_readwrite("candidates",
&PyCollaborationAckClosure::candidates)
```

### CollaborationAckClosure · "request_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7492)

```cpp
def_readwrite("request_id",
&PyCollaborationAckClosure::requestId)
```

### LargeDataPublishResult · "error"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7510)

```cpp
def_readwrite("error",
&PyLargeDataPublishResult::error)
```

### LargeDataPublishResult · "encrypted"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7509)

```cpp
def_readwrite("encrypted",
&PyLargeDataPublishResult::encrypted)
```

### LargeDataPublishResult · "protection_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7508)

```cpp
def_readwrite("protection_epoch",
&PyLargeDataPublishResult::protectionEpoch)
```

### LargeDataPublishResult · "authorization_scope"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7507)

```cpp
def_readwrite("authorization_scope",
&PyLargeDataPublishResult::authorizationScope)
```

### LargeDataPublishResult · "manifest_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7506)

```cpp
def_readwrite("manifest_digest",
&PyLargeDataPublishResult::manifestDigest)
```

### LargeDataPublishResult · "content_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7505)

```cpp
def_readwrite("content_digest",
&PyLargeDataPublishResult::contentDigest)
```

### LargeDataPublishResult · "plaintext_size"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7504)

```cpp
def_readwrite("plaintext_size",
&PyLargeDataPublishResult::plaintextSize)
```

### LargeDataPublishResult · "object_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7503)

```cpp
def_readwrite("object_id",
&PyLargeDataPublishResult::objectId)
```

### LargeDataPublishResult · "encrypted_data_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7502)

```cpp
def_readwrite("encrypted_data_name",
&PyLargeDataPublishResult::encryptedDataName)
```

### LargeDataPublishResult · "success"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7501)

```cpp
def_readwrite("success",
&PyLargeDataPublishResult::success)
```

### SignedAppDataResult · "error"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7518)

```cpp
def_readwrite("error",
&PySignedAppDataResult::error)
```

### SignedAppDataResult · "payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7517)

```cpp
def_readwrite("payload",
&PySignedAppDataResult::payload)
```

### SignedAppDataResult · "signer_certificate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7516)

```cpp
def_readwrite("signer_certificate",
&PySignedAppDataResult::signerCertificate)
```

### SignedAppDataResult · "data_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7515)

```cpp
def_readwrite("data_name",
&PySignedAppDataResult::dataName)
```

### SignedAppDataResult · "success"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7514)

```cpp
def_readwrite("success",
&PySignedAppDataResult::success)
```

### CollaborationAssignment · "role_providers"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7530)

```cpp
def_readwrite("role_providers",
&PyCollaborationAssignment::roleProviders)
```

### CollaborationAssignment · "assignment_payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7529)

```cpp
def_readwrite("assignment_payload",
&PyCollaborationAssignment::assignmentPayload)
```

### CollaborationAssignment · "selection_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7528)

```cpp
def_readwrite("selection_digest",
&PyCollaborationAssignment::selectionDigest)
```

### CollaborationAssignment · "provisioning_timeout_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7527)

```cpp
def_readwrite("provisioning_timeout_ms",
&PyCollaborationAssignment::provisioningTimeoutMs)
```

### CollaborationAssignment · "requires_provisioning"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7526)

```cpp
def_readwrite("requires_provisioning",
&PyCollaborationAssignment::requiresProvisioning)
```

### CollaborationAssignment · "artifact_data_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7525)

```cpp
def_readwrite("artifact_data_name",
&PyCollaborationAssignment::artifactDataName)
```

### CollaborationAssignment · "assigned_artifact"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7524)

```cpp
def_readwrite("assigned_artifact",
&PyCollaborationAssignment::assignedArtifact)
```

### CollaborationAssignment · "service"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7523)

```cpp
def_readwrite("service",
&PyCollaborationAssignment::service)
```

### CollaborationAssignment · "role"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7522)

```cpp
def_readwrite("role",
&PyCollaborationAssignment::role)
```

### CollaborationData · "payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7540)

```cpp
def_readwrite("payload",
&PyCollaborationData::payload)
```

### CollaborationData · "sequence"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7539)

```cpp
def_readwrite("sequence",
&PyCollaborationData::sequence)
```

### CollaborationData · "producer_role"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7538)

```cpp
def_readwrite("producer_role",
&PyCollaborationData::producerRole)
```

### CollaborationData · "producer"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7537)

```cpp
def_readwrite("producer",
&PyCollaborationData::producer)
```

### CollaborationData · "topic"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7536)

```cpp
def_readwrite("topic",
&PyCollaborationData::topic)
```

### CollaborationData · "key_scope"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7535)

```cpp
def_readwrite("key_scope",
&PyCollaborationData::keyScope)
```

### CollaborationData · "session_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7534)

```cpp
def_readwrite("session_id",
&PyCollaborationData::sessionId)
```

### SegmentedObjectProducer · "error"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7558)

```cpp
def_property_readonly("error",
&NativeSegmentedObjectProducer::error)
```

### SegmentedObjectProducer · "segment_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7557)

```cpp
def_property_readonly("segment_count",
&NativeSegmentedObjectProducer::segmentCount)
```

### SegmentedObjectProducer · "versioned_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7556)

```cpp
def_property_readonly("versioned_name",
&NativeSegmentedObjectProducer::versionedName)
```

### SegmentedObjectProducer · "base_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7555)

```cpp
def_property_readonly("base_name",
&NativeSegmentedObjectProducer::baseName)
```

### SegmentedObjectProducer · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7554)

```cpp
def("stop",
&NativeSegmentedObjectProducer::stop)
```

### SegmentedObjectProducer · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7553)

```cpp
def("start",
&NativeSegmentedObjectProducer::start)
```

### FileSegmentedObjectProducer · "error"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7583)

```cpp
def_property_readonly("error",
&NativeFileSegmentedObjectProducer::error)
```

### FileSegmentedObjectProducer · "public_key_der"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7582)

```cpp
def_property_readonly("public_key_der",
&NativeFileSegmentedObjectProducer::publicKeyDer)
```

### FileSegmentedObjectProducer · "signing_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7581)

```cpp
def_property_readonly("signing_ms",
&NativeFileSegmentedObjectProducer::signingMs)
```

### FileSegmentedObjectProducer · "wire_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7580)

```cpp
def_property_readonly("wire_bytes",
&NativeFileSegmentedObjectProducer::wireBytes)
```

### FileSegmentedObjectProducer · "data_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7579)

```cpp
def_property_readonly("data_count",
&NativeFileSegmentedObjectProducer::dataCount)
```

### FileSegmentedObjectProducer · "file_size"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7578)

```cpp
def_property_readonly("file_size",
&NativeFileSegmentedObjectProducer::fileSize)
```

### FileSegmentedObjectProducer · "segment_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7577)

```cpp
def_property_readonly("segment_count",
&NativeFileSegmentedObjectProducer::segmentCount)
```

### FileSegmentedObjectProducer · "versioned_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7576)

```cpp
def_property_readonly("versioned_name",
&NativeFileSegmentedObjectProducer::versionedName)
```

### FileSegmentedObjectProducer · "base_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7575)

```cpp
def_property_readonly("base_name",
&NativeFileSegmentedObjectProducer::baseName)
```

### FileSegmentedObjectProducer · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7574)

```cpp
def("stop",
&NativeFileSegmentedObjectProducer::stop)
```

### FileSegmentedObjectProducer · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7573)

```cpp
def("start",
&NativeFileSegmentedObjectProducer::start)
```

### DataPacket · "content"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7590)

```cpp
def_readwrite("content",
&PyDataPacket::content)
```

### DataPacket · "wire"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7589)

```cpp
def_readwrite("wire",
&PyDataPacket::wire)
```

### DataPacket · "segment"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7588)

```cpp
def_readwrite("segment",
&PyDataPacket::segment)
```

### DataPacket · "name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7587)

```cpp
def_readwrite("name",
&PyDataPacket::name)
```

### m · "verify_data_packet_signature"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7592)

```cpp
def("verify_data_packet_signature",
&verifyDataPacketSignature,
py::arg("wire"),
py::arg("public_key_der"))
```

### m · "verify_detached_sha256_signature"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7594)

```cpp
def("verify_detached_sha256_signature",
&verifyDetachedSha256Signature,
py::arg("payload"),
py::arg("signature"),
py::arg("public_key_der"))
```

### m · "verify_data_packet_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7596)

```cpp
def("verify_data_packet_digest",
&verifyDataPacketDigest,
py::arg("wire"))
```

### SegmentHintRange · "forwarding_hints"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7603)

```cpp
def_readwrite("forwarding_hints",
&PySegmentHintRange::forwardingHints)
```

### SegmentHintRange · "end"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7602)

```cpp
def_readwrite("end",
&PySegmentHintRange::end)
```

### SegmentHintRange · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7601)

```cpp
def_readwrite("start",
&PySegmentHintRange::start)
```

### StoredDataProducer · "error"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7617)

```cpp
def_property_readonly("error",
&NativeWireDataProducer::error)
```

### StoredDataProducer · "segment_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7616)

```cpp
def_property_readonly("segment_count",
&NativeWireDataProducer::segmentCount)
```

### StoredDataProducer · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7615)

```cpp
def("stop",
&NativeWireDataProducer::stop)
```

### StoredDataProducer · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7614)

```cpp
def("start",
&NativeWireDataProducer::start)
```

### RepoDataPlaneProducer · "error"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7636)

```cpp
def_property_readonly("error",
&NativeRepoDataPlaneProducer::error)
```

### RepoDataPlaneProducer · "thread_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7635)

```cpp
def_property_readonly("thread_count",
&NativeRepoDataPlaneProducer::threadCount)
```

### RepoDataPlaneProducer · "miss_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7634)

```cpp
def_property_readonly("miss_count",
&NativeRepoDataPlaneProducer::missCount)
```

### RepoDataPlaneProducer · "hit_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7633)

```cpp
def_property_readonly("hit_count",
&NativeRepoDataPlaneProducer::hitCount)
```

### RepoDataPlaneProducer · "interest_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7631)

```cpp
def_property_readonly("interest_count",
&NativeRepoDataPlaneProducer::interestCount)
```

### RepoDataPlaneProducer · "active_prefix_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7629)

```cpp
def_property_readonly("active_prefix_count",
&NativeRepoDataPlaneProducer::activePrefixCount)
```

### RepoDataPlaneProducer · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7628)

```cpp
def("stop",
&NativeRepoDataPlaneProducer::stop)
```

### RepoDataPlaneProducer · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7627)

```cpp
def("start",
&NativeRepoDataPlaneProducer::start)
```

### RepoDataPlaneProducer · "activate_prefix"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7626)

```cpp
def("activate_prefix",
&NativeRepoDataPlaneProducer::activatePrefix)
```

### m · "make_segmented_data_packets"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7638)

```cpp
def("make_segmented_data_packets",
&makeSegmentedDataPackets,
py::arg("base_name"),
py::arg("payload"),
py::arg("signing_identity") = "",
py::arg("max_segment_size") = 6000,
py::arg("freshness_ms") = 60000)
```

### m · "make_signed_data"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7646)

```cpp
def("make_signed_data",
&makeSignedData,
py::arg("name"),
py::arg("content"),
py::arg("signing_identity") = "",
py::arg("freshness_ms") = 300)
```

### m · "make_predictive_data_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7653)

```cpp
def("make_predictive_data_name",
&makePredictiveDataNameUri,
py::arg("mapping_root"),
py::arg("mapping_version"),
py::arg("sequence"))
```

### m · "wrap_selection_gated_input_key"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7659)

```cpp
def("wrap_selection_gated_input_key",
[] (const py::bytes& key, const py::bytes& recipientPublicKey) { implementation omitted },
py::arg("key"),
py::arg("recipient_public_key"))
```

### m · "decode_data_packet"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7667)

```cpp
def("decode_data_packet",
&decodeDataPacket,
py::arg("wire"))
```

### m · "fetch_segmented_data_packets"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7671)

```cpp
def("fetch_segmented_data_packets",
&fetchSegmentedDataPackets,
py::arg("base_name"),
py::arg("timeout_ms") = 30000,
py::arg("interest_lifetime_ms") = 10000,
py::arg("forwarding_hints") = std::vector<std::string>{})
```

### AdaptiveSegmentFetchResult · "final_window"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7702)

```cpp
def_readonly("final_window",
&PyAdaptiveSegmentFetchResult::finalWindow)
```

### AdaptiveSegmentFetchResult · "maximum_in_flight"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7700)

```cpp
def_readonly("maximum_in_flight",
&PyAdaptiveSegmentFetchResult::maximumInFlight)
```

### AdaptiveSegmentFetchResult · "retransmitted_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7698)

```cpp
def_readonly("retransmitted_bytes",
&PyAdaptiveSegmentFetchResult::retransmittedBytes)
```

### AdaptiveSegmentFetchResult · "wire_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7697)

```cpp
def_readonly("wire_bytes",
&PyAdaptiveSegmentFetchResult::wireBytes)
```

### AdaptiveSegmentFetchResult · "interest_wire_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7695)

```cpp
def_readonly("interest_wire_bytes",
&PyAdaptiveSegmentFetchResult::interestWireBytes)
```

### AdaptiveSegmentFetchResult · "data_wire_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7693)

```cpp
def_readonly("data_wire_bytes",
&PyAdaptiveSegmentFetchResult::dataWireBytes)
```

### AdaptiveSegmentFetchResult · "logical_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7691)

```cpp
def_readonly("logical_bytes",
&PyAdaptiveSegmentFetchResult::logicalBytes)
```

### AdaptiveSegmentFetchResult · "timeout_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7689)

```cpp
def_readonly("timeout_count",
&PyAdaptiveSegmentFetchResult::timeoutCount)
```

### AdaptiveSegmentFetchResult · "duplicate_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7687)

```cpp
def_readonly("duplicate_count",
&PyAdaptiveSegmentFetchResult::duplicateCount)
```

### AdaptiveSegmentFetchResult · "retransmission_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7685)

```cpp
def_readonly("retransmission_count",
&PyAdaptiveSegmentFetchResult::retransmissionCount)
```

### AdaptiveSegmentFetchResult · "interest_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7683)

```cpp
def_readonly("interest_count",
&PyAdaptiveSegmentFetchResult::interestCount)
```

### AdaptiveSegmentFetchResult · "delivered_segments"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7681)

```cpp
def_readonly("delivered_segments",
&PyAdaptiveSegmentFetchResult::deliveredSegments)
```

### AdaptiveSegmentFetchResult · "total_segments"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7679)

```cpp
def_readonly("total_segments",
&PyAdaptiveSegmentFetchResult::totalSegments)
```

### m · "fetch_adaptive_segmented_data_packets"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7705)

```cpp
def("fetch_adaptive_segmented_data_packets",
&fetchAdaptiveSegmentedDataPackets,
py::arg("base_name"),
py::arg("timeout_ms") = 30000,
py::arg("interest_lifetime_ms") = 1000,
py::arg("initial_window") = 4,
py::arg("maximum_window") = 64,
py::arg("maximum_retries") = 5,
py::arg("persistence_backlog_limit") = 16,
py::arg("forwarding_hints") = std::vector<std::string>{},
py::arg("on_packet"))
```

### m · "fetch_exact_data_packet"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7717)

```cpp
def("fetch_exact_data_packet",
&fetchExactDataPacket,
py::arg("data_name"),
py::arg("timeout_ms") = 30000,
py::arg("interest_lifetime_ms") = 2000,
py::arg("forwarding_hints") = std::vector<std::string>{},
py::call_guard<py::gil_scoped_release>())
```

### m · "verify_and_unwrap_native_grant"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7729)

```cpp
def("verify_and_unwrap_native_grant",
[] (const std::string& wireJson,
            const std::string& authorityPublicKeyRawHex,
            const std::string& recipientSeedHex,
            const std::string& providerIdentity,
            const std::string& requestId,
            std::uint64_t attempt,
            const std::string& planCoreDigest,
            const std::string& modelManifestDigest,
            const std::string& protectionEpoch,
            std::uint64_t nowMs) -> py::dict { implementation omitted },
py::arg("wire_json"),
py::arg("authority_public_key_raw_hex"),
py::arg("recipient_seed_hex"),
py::arg("provider_identity"),
py::arg("request_id"),
py::arg("attempt"),
py::arg("plan_core_digest"),
py::arg("model_manifest_digest"),
py::arg("protection_epoch"),
py::arg("now_ms"),
py::call_guard<py::gil_scoped_release>())
```

### m · "fetch_segmented_object"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7775)

```cpp
def("fetch_segmented_object",
&fetchSegmentedObject,
py::arg("base_name"),
py::arg("timeout_ms") = 30000,
py::arg("interest_lifetime_ms") = 10000,
py::arg("init_cwnd") = 8.0,
py::arg("forwarding_hints") = std::vector<std::string>{},
py::call_guard<py::gil_scoped_release>())
```

### m · "fetch_segmented_object_with_segment_hints"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7784)

```cpp
def("fetch_segmented_object_with_segment_hints",
&fetchSegmentedObjectWithSegmentHints,
py::arg("base_name"),
py::arg("timeout_ms") = 30000,
py::arg("interest_lifetime_ms") = 10000,
py::arg("hint_ranges") = std::vector<PySegmentHintRange>{},
py::call_guard<py::gil_scoped_release>())
```

### m · "fetch_known_segmented_object_with_segment_hints"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7791)

```cpp
def("fetch_known_segmented_object_with_segment_hints",
&fetchKnownSegmentedObjectWithSegmentHints,
py::arg("versioned_name"),
py::arg("segment_count"),
py::arg("timeout_ms") = 30000,
py::arg("interest_lifetime_ms") = 10000,
py::arg("hint_ranges") = std::vector<PySegmentHintRange>{},
py::call_guard<py::gil_scoped_release>())
```

### CollaborationContext · "stream_cancelled"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7864)

```cpp
def_property_readonly("stream_cancelled",
&PyCollaborationContext::streamCancelled)
```

### CollaborationContext · "fail_stream"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7862)

```cpp
def("fail_stream",
&PyCollaborationContext::failStream,
py::arg("code"),
py::arg("message"))
```

### CollaborationContext · "finish_stream"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7860)

```cpp
def("finish_stream",
&PyCollaborationContext::finishStream,
py::arg("payload"),
py::arg("reason") = 4)
```

### CollaborationContext · "publish_stream_event"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7858)

```cpp
def("publish_stream_event",
&PyCollaborationContext::publishStreamEvent,
py::arg("payload"))
```

### CollaborationContext · "is_streamed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7857)

```cpp
def_property_readonly("is_streamed",
&PyCollaborationContext::isStreamed)
```

### CollaborationContext · "publish_final_response"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7855)

```cpp
def("publish_final_response",
&PyCollaborationContext::publishFinalResponse,
py::arg("payload"))
```

### CollaborationContext · "report_operation_status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7853)

```cpp
def("report_operation_status",
&PyCollaborationContext::reportOperationStatus,
py::arg("status"))
```

### CollaborationContext · "wait_for"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7848)

```cpp
def("wait_for",
&PyCollaborationContext::waitFor,
py::arg("key_scope"),
py::arg("topic_prefix"),
py::arg("min_count"),
py::arg("timeout_ms") = 5000)
```

### CollaborationContext · "wait_one"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7844)

```cpp
def("wait_one",
&PyCollaborationContext::waitOne,
py::arg("key_scope"),
py::arg("topic_prefix"),
py::arg("timeout_ms") = 5000)
```

### CollaborationContext · "fetch_large_exact"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7839)

```cpp
def("fetch_large_exact",
&PyCollaborationContext::fetchLargeExact,
py::arg("data_name"),
py::arg("key_scope"),
py::arg("timeout_ms") = 5000,
py::arg("expected_segments"))
```

### CollaborationContext · "fetch_large"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7835)

```cpp
def("fetch_large",
&PyCollaborationContext::fetchLarge,
py::arg("data_name"),
py::arg("key_scope"),
py::arg("timeout_ms") = 5000)
```

### CollaborationContext · "publish_large_named"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7829)

```cpp
def("publish_large_named",
&PyCollaborationContext::publishLargeNamed,
py::arg("key_scope"),
py::arg("data_name"),
py::arg("payload"),
py::arg("max_segment_size") = 7000,
py::arg("freshness_ms") = 60000)
```

### CollaborationContext · "publish_large"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7823)

```cpp
def("publish_large",
&PyCollaborationContext::publishLarge,
py::arg("key_scope"),
py::arg("topic"),
py::arg("payload"),
py::arg("max_segment_size") = 7000,
py::arg("freshness_ms") = 60000)
```

### CollaborationContext · "publish"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7819)

```cpp
def("publish",
&PyCollaborationContext::publish,
py::arg("key_scope"),
py::arg("topic"),
py::arg("payload"))
```

### CollaborationContext · "allow_data"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7816)

```cpp
def("allow_data",
&PyCollaborationContext::allowData,
py::arg("key_scope"),
py::arg("topic_prefix"))
```

### CollaborationContext · "fail"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7814)

```cpp
def("fail",
&PyCollaborationContext::fail,
py::arg("reason"))
```

### CollaborationContext · "fetch_encrypted_large_data"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7811)

```cpp
def("fetch_encrypted_large_data",
&PyCollaborationContext::fetchEncryptedLargeData,
py::arg("data_name"),
py::arg("service") = "")
```

### CollaborationContext · "get_artifact"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7809)

```cpp
def("get_artifact",
&PyCollaborationContext::getArtifact,
py::arg("artifact_name"))
```

### CollaborationContext · "fetch_artifact"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7806)

```cpp
def("fetch_artifact",
&PyCollaborationContext::fetchArtifact,
py::arg("artifact_name"),
py::arg("timeout_ms") = 5000)
```

### CollaborationContext · "assignment"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7805)

```cpp
def_property_readonly("assignment",
&PyCollaborationContext::assignment)
```

### CollaborationContext · "local_provider"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7804)

```cpp
def_property_readonly("local_provider",
&PyCollaborationContext::localProvider)
```

### CollaborationContext · "requester_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7803)

```cpp
def_property_readonly("requester_name",
&PyCollaborationContext::requesterName)
```

### CollaborationContext · "role"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7802)

```cpp
def_property_readonly("role",
&PyCollaborationContext::role)
```

### CollaborationContext · "session_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7801)

```cpp
def_property_readonly("session_id",
&PyCollaborationContext::sessionId)
```

### NativeServiceController · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7884)

```cpp
def("stop",
&NativeServiceController::stop)
```

### NativeServiceController · "run"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7883)

```cpp
def("run",
&NativeServiceController::run,
py::call_guard<py::gil_scoped_release>())
```

### NativeServiceController · "wait_until_ready"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7880)

```cpp
def("wait_until_ready",
&NativeServiceController::waitUntilReady,
py::call_guard<py::gil_scoped_release>(),
py::arg("timeout_ms") = 10000)
```

### NativeServiceController · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7879)

```cpp
def("start",
&NativeServiceController::start)
```

### StreamWriter · "remaining_deadline_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7893)

```cpp
def_property_readonly("remaining_deadline_ms",
&PyStreamWriter::remaining_deadline_ms)
```

### StreamWriter · "cancelled"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7892)

```cpp
def_property_readonly("cancelled",
&PyStreamWriter::cancelled)
```

### StreamWriter · "fail"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7890)

```cpp
def("fail",
&PyStreamWriter::fail,
py::arg("code"),
py::arg("message"))
```

### StreamWriter · "finish_stream"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7888)

```cpp
def("finish_stream",
&PyStreamWriter::finish,
py::arg("payload") = py::bytes(),
py::arg("reason") = 4)
```

### StreamWriter · "publish_event"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7887)

```cpp
def("publish_event",
&PyStreamWriter::publish,
py::arg("payload"))
```

### NativeStreamedInvocationHandle · "cancel"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7900)

```cpp
def("cancel",
&PyStreamedInvocationHandle::cancel)
```

### NativeStreamedInvocationHandle · "metrics"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7899)

```cpp
def_property_readonly("metrics",
&PyStreamedInvocationHandle::metrics)
```

### NativeStreamedInvocationHandle · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7898)

```cpp
def_property_readonly("status",
&PyStreamedInvocationHandle::status)
```

### NativeStreamedInvocationHandle · "request_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7897)

```cpp
def_property_readonly("request_id",
&PyStreamedInvocationHandle::requestId)
```

### NativeServiceProvider · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7989)

```cpp
def("stop",
&NativeServiceProvider::stop)
```

### NativeServiceProvider · "wait_until_ready"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7987)

```cpp
def("wait_until_ready",
&NativeServiceProvider::waitUntilReady,
py::arg("timeout_ms") = 15000)
```

### NativeServiceProvider · "run"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7986)

```cpp
def("run",
&NativeServiceProvider::run,
py::call_guard<py::gil_scoped_release>())
```

### NativeServiceProvider · "create_stream"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7984)

```cpp
def("create_stream",
&NativeServiceProvider::createStream,
py::arg("config"))
```

### NativeServiceProvider · "create_live_stream"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7982)

```cpp
def("create_live_stream",
&NativeServiceProvider::createLiveStream,
py::arg("definition"))
```

### NativeServiceProvider · "start_ndnsd_periodic_publish"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7980)

```cpp
def("start_ndnsd_periodic_publish",
&NativeServiceProvider::startNdnsdPeriodicPublish,
py::arg("interval_seconds"))
```

### NativeServiceProvider · "set_ndnsd_meta"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7978)

```cpp
def("set_ndnsd_meta",
&NativeServiceProvider::setNdnsdMeta,
py::arg("meta"))
```

### NativeServiceProvider · "update_ndnsd_meta"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7976)

```cpp
def("update_ndnsd_meta",
&NativeServiceProvider::updateNdnsdMeta,
py::arg("key"),
py::arg("value"))
```

### NativeServiceProvider · "publish_service_info"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7974)

```cpp
def("publish_service_info",
&NativeServiceProvider::publishServiceInfo,
py::arg("service_name"),
py::arg("service_lifetime_seconds"),
py::arg("meta_info") = py::dict())
```

### NativeServiceProvider · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7973)

```cpp
def("start",
&NativeServiceProvider::start)
```

### NativeServiceProvider · "add_collaboration_service"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7967)

```cpp
def("add_collaboration_service",
&NativeServiceProvider::addCollaborationService,
py::arg("service"),
py::arg("allowed_roles"),
py::arg("collaboration_handler"),
py::arg("ack_handler") = std::optional<py::function>(),
py::arg("include_ack_context") = false)
```

### NativeServiceProvider · "set_r1_reservation_terminal_handler"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7964)

```cpp
def("set_r1_reservation_terminal_handler",
&NativeServiceProvider::setR1ReservationTerminalHandler,
py::arg("service"),
py::arg("handler"))
```

### NativeServiceProvider · "set_r1_selection_decision_handler"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7961)

```cpp
def("set_r1_selection_decision_handler",
&NativeServiceProvider::setR1SelectionDecisionHandler,
py::arg("service"),
py::arg("handler"))
```

### NativeServiceProvider · "register_opaque_selection_participant"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7956)

```cpp
def("register_opaque_selection_participant",
&NativeServiceProvider::registerOpaqueSelectionParticipant,
py::arg("service"),
py::arg("participant_id"),
py::arg("participant_version"),
py::arg("prepare"),
py::arg("on_committed"),
py::arg("on_aborted"))
```

### NativeServiceProvider · "configure_opaque_selection_store"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7952)

```cpp
def("configure_opaque_selection_store",
&NativeServiceProvider::configureOpaqueSelectionStore,
py::arg("wal_path"),
py::arg("storage_key"),
py::arg("storage_key_epoch"),
py::arg("max_prepare_ms") = 1000)
```

### NativeServiceProvider · "publish_stream_packet_for_test"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7949)

```cpp
def("publish_stream_packet_for_test",
&NativeServiceProvider::publishStreamPacketForTest,
py::arg("wire"))
```

### NativeServiceProvider · "set_stream_retention_interceptor_for_test"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7946)

```cpp
def("set_stream_retention_interceptor_for_test",
&NativeServiceProvider::setStreamRetentionInterceptorForTest,
py::arg("callback"))
```

### NativeServiceProvider · "set_stream_publication_interceptor_for_test"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7943)

```cpp
def("set_stream_publication_interceptor_for_test",
&NativeServiceProvider::setStreamPublicationInterceptorForTest,
py::arg("callback"))
```

### NativeServiceProvider · "provider_signing_certificate_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7941)

```cpp
def_property_readonly("provider_signing_certificate_name",
&NativeServiceProvider::providerSigningCertificateName)
```

### NativeServiceProvider · "provider_signing_key_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7939)

```cpp
def_property_readonly("provider_signing_key_name",
&NativeServiceProvider::providerSigningKeyName)
```

### NativeServiceProvider · "provider_identity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7937)

```cpp
def_property_readonly("provider_identity",
&NativeServiceProvider::providerIdentity)
```

### NativeServiceProvider · "provider_boot_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7935)

```cpp
def_property_readonly("provider_boot_epoch",
&NativeServiceProvider::providerBootEpoch)
```

### NativeServiceProvider · "set_deployment_prepare_handler"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7932)

```cpp
def("set_deployment_prepare_handler",
&NativeServiceProvider::setDeploymentPrepareHandler,
py::arg("handler"))
```

### NativeServiceProvider · "add_streaming_context_service"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7929)

```cpp
def("add_streaming_context_service",
&NativeServiceProvider::addStreamingContextService,
py::arg("service"),
py::arg("handler"))
```

### NativeServiceProvider · "add_streaming_service"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7927)

```cpp
def("add_streaming_service",
&NativeServiceProvider::addStreamingService,
py::arg("service"),
py::arg("handler"))
```

### NativeServiceProvider · "add_service"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7921)

```cpp
def("add_service",
&NativeServiceProvider::addService,
py::arg("service"),
py::arg("request_handler"),
py::arg("ack_handler") = std::optional<py::function>(),
py::arg("include_request_context") = false,
py::arg("include_ack_context") = false)
```

### NativeServiceUser · "pump"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8186)

```cpp
def("pump",
&NativeServiceUser::pump)
```

### NativeServiceUser · "get_ndnsd_services"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8185)

```cpp
def("get_ndnsd_services",
&NativeServiceUser::getNdnsdServices)
```

### NativeServiceUser · "refresh_permissions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8184)

```cpp
def("refresh_permissions",
&NativeServiceUser::refreshPermissions)
```

### NativeServiceUser · "get_allowed_services"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8183)

```cpp
def("get_allowed_services",
&NativeServiceUser::getAllowedServices)
```

### NativeServiceUser · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8181)

```cpp
def("stop",
&NativeServiceUser::stop,
py::call_guard<py::gil_scoped_release>())
```

### NativeServiceUser · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8176)

```cpp
def("start",
&NativeServiceUser::start)
```

### NativeServiceUser · "get_collaboration_status_snapshot"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8173)

```cpp
def("get_collaboration_status_snapshot",
&NativeServiceUser::getCollaborationStatusSnapshot,
py::arg("request_id"),
py::arg("timeout_ms") = 500)
```

### NativeServiceUser · "query_collaboration_status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8170)

```cpp
def("query_collaboration_status",
&NativeServiceUser::queryCollaborationStatus,
py::arg("provider"),
py::arg("service"),
py::arg("selection_digest"),
py::arg("timeout_ms") = 500)
```

### NativeServiceUser · "request_collaboration_async"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8155)

```cpp
def("request_collaboration_async",
&NativeServiceUser::requestCollaborationAsync,
py::arg("service"),
py::arg("payload"),
py::arg("roles"),
py::arg("key_scopes"),
py::arg("dependencies"),
py::arg("artifact_data_names"),
py::arg("scope_key_data_names"),
py::arg("role_scopes"),
py::arg("on_response"),
py::arg("on_timeout"),
py::arg("ack_timeout_ms") = 300,
py::arg("timeout_ms") = 10000,
py::arg("role_provider_assignments") = std::map<std::string, std::string>{},
py::arg("request_id") = "")
```

### NativeServiceUser · "publish_collaboration_data"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8151)

```cpp
def("publish_collaboration_data",
&NativeServiceUser::publishCollaborationData,
py::arg("target_provider"),
py::arg("request_id"),
py::arg("key_scope"),
py::arg("topic"),
py::arg("payload"))
```

### NativeServiceUser · "clear_verified_collaboration_data"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8148)

```cpp
def("clear_verified_collaboration_data",
&NativeServiceUser::clearVerifiedCollaborationData,
py::arg("request_id"),
py::arg("key_scope"))
```

### NativeServiceUser · "wait_for_verified_collaboration_data"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8143)

```cpp
def("wait_for_verified_collaboration_data",
&NativeServiceUser::waitForVerifiedCollaborationData,
py::arg("request_id"),
py::arg("key_scope"),
py::arg("topic_prefix"),
py::arg("min_count"),
py::arg("timeout_ms"),
py::arg("consume") = true)
```

### NativeServiceUser · "commit_collaboration_plan"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8133)

```cpp
def("commit_collaboration_plan",
&NativeServiceUser::commitCollaborationPlan,
py::arg("service"),
py::arg("request_id"),
py::arg("ack_closed_digest"),
py::arg("roles"),
py::arg("key_scopes"),
py::arg("dependencies"),
py::arg("artifact_data_names"),
py::arg("scope_key_data_names"),
py::arg("role_scopes"),
py::arg("ack_timeout_ms") = 300,
py::arg("timeout_ms") = 10000,
py::arg("role_provider_assignments") =
           std::map<std::string, std::string>{})
```

### NativeServiceUser · "begin_collaboration"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8122)

```cpp
def("begin_collaboration",
&NativeServiceUser::beginCollaboration,
py::arg("service"),
py::arg("payload"),
py::arg("on_ack_closed"),
py::arg("on_response"),
py::arg("on_timeout"),
py::arg("ack_timeout_ms") = 300,
py::arg("timeout_ms") = 10000,
py::arg("request_id") = "",
py::arg("ack_coverage_predicate") = py::none(),
py::arg("request_capabilities") = std::nullopt,
py::arg("stream_options") = py::none(),
py::arg("on_stream_event") = py::none(),
py::arg("on_stream_complete") = py::none(),
py::arg("on_stream_error") = py::none())
```

### NativeServiceUser · "request_collaboration"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8108)

```cpp
def("request_collaboration",
&NativeServiceUser::requestCollaboration,
py::arg("service"),
py::arg("payload"),
py::arg("roles"),
py::arg("key_scopes"),
py::arg("dependencies"),
py::arg("artifact_data_names"),
py::arg("scope_key_data_names"),
py::arg("role_scopes"),
py::arg("ack_timeout_ms") = 300,
py::arg("timeout_ms") = 10000,
py::arg("ack_observer") = py::none(),
py::arg("role_provider_assignments") = std::map<std::string, std::string>{},
py::arg("request_id") = "")
```

### NativeServiceUser · "fetch_signed_app_data"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8105)

```cpp
def("fetch_signed_app_data",
&NativeServiceUser::fetchSignedAppData,
py::arg("data_name"),
py::arg("expected_signer"),
py::arg("timeout_ms") = 5000)
```

### NativeServiceUser · "publish_signed_app_data"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8102)

```cpp
def("publish_signed_app_data",
&NativeServiceUser::publishSignedAppData,
py::arg("data_name"),
py::arg("payload"),
py::arg("freshness_ms") = 60000)
```

### NativeServiceUser · "publish_encrypted_large_data"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8097)

```cpp
def("publish_encrypted_large_data",
&NativeServiceUser::publishEncryptedLargeData,
py::arg("service"),
py::arg("payload"),
py::arg("object_label") = "",
py::arg("freshness_ms") = 60000)
```

### NativeServiceUser · "stream_metrics_for_test"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8095)

```cpp
def("stream_metrics_for_test",
&NativeServiceUser::streamMetricsForTest,
py::arg("request_id"))
```

### NativeServiceUser · "cancel_stream_request"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8093)

```cpp
def("cancel_stream_request",
&NativeServiceUser::cancelStreamRequest,
py::arg("request_id"))
```

### NativeServiceUser · "request_service_streaming_handle"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8088)

```cpp
def("request_service_streaming_handle",
&NativeServiceUser::requestServiceStreamingHandle,
py::arg("service"),
py::arg("payload"),
py::arg("provider"),
py::arg("options"),
py::arg("strategy"),
py::arg("on_event"),
py::arg("on_complete"),
py::arg("on_error"))
```

### NativeServiceUser · "request_service_streaming"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8084)

```cpp
def("request_service_streaming",
&NativeServiceUser::requestServiceStreaming,
py::arg("service"),
py::arg("payload"),
py::arg("provider"),
py::arg("on_event"),
py::arg("on_complete"),
py::arg("on_error"))
```

### NativeServiceUser · "request_service_targeted_async"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8077)

```cpp
def("request_service_targeted_async",
&NativeServiceUser::requestServiceTargetedAsync,
py::arg("provider"),
py::arg("service"),
py::arg("payload"),
py::arg("on_response"),
py::arg("on_timeout"),
py::arg("timeout_ms") = 5000)
```

### NativeServiceUser · "request_service_async"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8069)

```cpp
def("request_service_async",
&NativeServiceUser::requestServiceAsync,
py::arg("service"),
py::arg("payload"),
py::arg("on_response"),
py::arg("on_timeout"),
py::arg("ack_timeout_ms") = 300,
py::arg("timeout_ms") = 5000,
py::arg("strategy") = "first-responding")
```

### NativeServiceUser · "request_service_select"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8060)

```cpp
def("request_service_select",
&NativeServiceUser::requestServiceSelect,
py::arg("service"),
py::arg("payload"),
py::arg("selector"),
py::arg("ack_timeout_ms") = 300,
py::arg("timeout_ms") = 5000,
py::arg("request_strategy") = "first-responding",
py::arg("deployment_intent") = std::nullopt,
py::arg("request_capabilities") = std::nullopt)
```

### NativeServiceUser · "request_service_targeted"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8055)

```cpp
def("request_service_targeted",
&NativeServiceUser::requestServiceTargeted,
py::arg("provider"),
py::arg("service"),
py::arg("payload"),
py::arg("timeout_ms") = 5000)
```

### NativeServiceUser · "request_service"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8046)

```cpp
def("request_service",
&NativeServiceUser::requestService,
py::arg("service"),
py::arg("payload"),
py::arg("ack_timeout_ms") = 300,
py::arg("timeout_ms") = 5000,
py::arg("strategy") = "first-responding",
py::arg("request_id") = "",
py::arg("deployment_intent") = std::nullopt,
py::arg("request_capabilities") = std::nullopt)
```

### NativeServiceUser · "subscribe_stream"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8037)

```cpp
def("subscribe_stream",
&NativeServiceUser::subscribeStream,
py::arg("descriptor"),
py::arg("on_item"),
py::arg("start") = "latest",
py::arg("prefetch_policy") = std::optional<std::string>(),
py::arg("aggregate_interest_limit") = 64,
py::arg("enable_fec_recovery") = true,
py::arg("require_full_delivery") = false,
py::arg("interest_lifetime_ms") = 500,
py::arg("on_status") = std::optional<py::function>())
```

### NativeServiceUser · "open_live_stream"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8029)

```cpp
def("open_live_stream",
&NativeServiceUser::openLiveStream,
py::arg("descriptor"),
py::arg("on_item"),
py::arg("start") = "latest",
py::arg("prefetch_policy") = "mapped-pressure",
py::arg("aggregate_interest_limit") = 64,
py::arg("enable_fec_recovery") = false,
py::arg("interest_lifetime_ms") = 500,
py::arg("on_status") = std::optional<py::function>())
```

### NativeServiceUser · "native_conversation_coordinator_from_config"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8025)

```cpp
def("native_conversation_coordinator_from_config",
&NativeServiceUser::nativeConversationCoordinatorFromConfig,
py::arg("configuration_json"),
py::arg("base_directory") = ".",
py::keep_alive<0, 1>())
```

### NativeServiceUser · "native_inference_client_configured"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8020)

```cpp
def("native_inference_client_configured",
&NativeServiceUser::nativeInferenceClientConfigured,
py::arg("runtime"),
py::arg("preparation"),
py::arg("admission"),
py::arg("conversations") = nullptr,
py::keep_alive<0, 1>())
```

### NativeServiceUser · "native_grant_client_from_config"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8016)

```cpp
def("native_grant_client_from_config",
&NativeServiceUser::nativeGrantClientFromConfig,
py::arg("configuration_json"),
py::arg("base_directory") = ".",
py::keep_alive<0, 1>())
```

### NativeServiceUser · "native_preparation"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8014)

```cpp
def("native_preparation",
&NativeServiceUser::nativePreparation,
py::arg("catalog"),
py::arg("service_name"),
py::keep_alive<0, 1>())
```

### NativeServiceUser · "native_inference_client"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8012)

```cpp
def("native_inference_client",
&NativeServiceUser::nativeInferenceClient,
py::arg("adapters"),
py::keep_alive<0, 1>())
```

## pythonWrapper/src/ndnsf/di_bindings.cpp

SHA-256：`e214e8e6773a97bb3499b4f806a5132483bff268102e4fd725aa7ab344ae19da`。

### CachePolicy · "REFRESH"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L210)

```cpp
value("REFRESH",
di::CachePolicy::Refresh)
```

### CachePolicy · "USE_OR_FETCH"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L209)

```cpp
value("USE_OR_FETCH",
di::CachePolicy::UseOrFetch)
```

### CachePolicy · "USE_OR_WAIT"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L208)

```cpp
value("USE_OR_WAIT",
di::CachePolicy::UseOrWait)
```

### CachePolicy · "REQUIRE_READY"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L207)

```cpp
value("REQUIRE_READY",
di::CachePolicy::RequireReady)
```

### PreparationStatus · "CANCELLED"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L216)

```cpp
value("CANCELLED",
di::PreparationStatus::Cancelled)
```

### PreparationStatus · "FAILED"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L215)

```cpp
value("FAILED",
di::PreparationStatus::Failed)
```

### PreparationStatus · "READY"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L214)

```cpp
value("READY",
di::PreparationStatus::Ready)
```

### PreparationStatus · "PENDING"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L213)

```cpp
value("PENDING",
di::PreparationStatus::Pending)
```

### PreparationOrigin · "REFRESHED"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L222)

```cpp
value("REFRESHED",
di::PreparationReceipt::Origin::Refreshed)
```

### PreparationOrigin · "FETCHED"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L221)

```cpp
value("FETCHED",
di::PreparationReceipt::Origin::Fetched)
```

### PreparationOrigin · "JOINED_IN_FLIGHT"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L220)

```cpp
value("JOINED_IN_FLIGHT",
di::PreparationReceipt::Origin::JoinedInFlight)
```

### PreparationOrigin · "CACHE_HIT"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L219)

```cpp
value("CACHE_HIT",
di::PreparationReceipt::Origin::CacheHit)
```

### RequestStatus · "CANCELLED"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L228)

```cpp
value("CANCELLED",
di::RequestStatus::Cancelled)
```

### RequestStatus · "FAILED"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L227)

```cpp
value("FAILED",
di::RequestStatus::Failed)
```

### RequestStatus · "SUCCEEDED"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L226)

```cpp
value("SUCCEEDED",
di::RequestStatus::Succeeded)
```

### RequestStatus · "PENDING"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L225)

```cpp
value("PENDING",
di::RequestStatus::Pending)
```

### ModelRegistration · "native_config_path"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L233)

```cpp
def_readwrite("native_config_path",
&di::ModelRegistration::nativeConfigPath)
```

### ModelRegistration · "key"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L232)

```cpp
def_readwrite("key",
&di::ModelRegistration::key)
```

### RuntimeConfig · "preparation_job_timeout_s"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L241)

```cpp
def_property("preparation_job_timeout_s",
[] (const di::RuntimeConfig& config) { implementation omitted },
[] (di::RuntimeConfig& config, const py::object& value) { implementation omitted })
```

### RuntimeConfig · "max_prepared_entries"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L240)

```cpp
def_readwrite("max_prepared_entries",
&di::RuntimeConfig::maxPreparedEntries)
```

### RuntimeConfig · "max_prepared_bytes"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L239)

```cpp
def_readwrite("max_prepared_bytes",
&di::RuntimeConfig::maxPreparedBytes)
```

### RuntimeConfig · "models"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L238)

```cpp
def_readwrite("models",
&di::RuntimeConfig::models)
```

### RuntimeConfig · "native_config_path"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L237)

```cpp
def_readwrite("native_config_path",
&di::RuntimeConfig::nativeConfigPath)
```

### UserConfig · "profile_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L251)

```cpp
def_readwrite("profile_name",
&di::UserConfig::profileName)
```

### PrepareOptions · "timeout_s"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L256)

```cpp
def_property("timeout_s",
[] (const di::PrepareOptions& options) { implementation omitted },
[] (di::PrepareOptions& options, const py::object& value) { implementation omitted })
```

### PrepareOptions · "cache"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L255)

```cpp
def_readwrite("cache",
&di::PrepareOptions::cache)
```

### ModelCapabilities · "conversations"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L274)

```cpp
def_readonly("conversations",
&di::ModelCapabilities::conversations)
```

### ModelCapabilities · "streaming"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L273)

```cpp
def_readonly("streaming",
&di::ModelCapabilities::streaming)
```

### ModelCapabilities · "output_modes"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L272)

```cpp
def_readonly("output_modes",
&di::ModelCapabilities::outputModes)
```

### ModelCapabilities · "input_kinds"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L271)

```cpp
def_readonly("input_kinds",
&di::ModelCapabilities::inputKinds)
```

### ModelCapabilities · "output_schema_json"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L270)

```cpp
def_readonly("output_schema_json",
&di::ModelCapabilities::outputSchemaJson)
```

### ModelCapabilities · "input_schema_json"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L269)

```cpp
def_readonly("input_schema_json",
&di::ModelCapabilities::inputSchemaJson)
```

### ModelManifest · "preparation_key_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L286)

```cpp
def_readonly("preparation_key_digest",
&di::ModelManifest::preparationKeyDigest)
```

### ModelManifest · "task_contract_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L285)

```cpp
def_readonly("task_contract_digest",
&di::ModelManifest::taskContractDigest)
```

### ModelManifest · "catalog_configuration_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L284)

```cpp
def_readonly("catalog_configuration_digest",
&di::ModelManifest::catalogConfigurationDigest)
```

### ModelManifest · "planning_graph_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L283)

```cpp
def_readonly("planning_graph_digest",
&di::ModelManifest::planningGraphDigest)
```

### ModelManifest · "canonical_graph_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L282)

```cpp
def_readonly("canonical_graph_digest",
&di::ModelManifest::canonicalGraphDigest)
```

### ModelManifest · "task_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L281)

```cpp
def_readonly("task_name",
&di::ModelManifest::taskName)
```

### ModelManifest · "model_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L280)

```cpp
def_readonly("model_digest",
&di::ModelManifest::modelDigest)
```

### ModelManifest · "model_revision"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L279)

```cpp
def_readonly("model_revision",
&di::ModelManifest::modelRevision)
```

### ModelManifest · "model_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L278)

```cpp
def_readonly("model_name",
&di::ModelManifest::modelName)
```

### PreparationReceipt · "elapsed_s"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L292)

```cpp
def_property_readonly("elapsed_s",
[] (const di::PreparationReceipt& receipt) { implementation omitted })
```

### PreparationReceipt · "manifest_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L291)

```cpp
def_readonly("manifest_digest",
&di::PreparationReceipt::manifestDigest)
```

### PreparationReceipt · "preparation_key_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L290)

```cpp
def_readonly("preparation_key_digest",
&di::PreparationReceipt::preparationKeyDigest)
```

### PreparationReceipt · "origin"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L289)

```cpp
def_readonly("origin",
&di::PreparationReceipt::origin)
```

### GenerationOptions · "max_new_tokens"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L298)

```cpp
def_readwrite("max_new_tokens",
&di::GenerationOptions::maxNewTokens)
```

### StreamOptions · "max_replacements"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L304)

```cpp
def_readwrite("max_replacements",
&di::StreamOptions::maxReplacements)
```

### StreamOptions · "allow_replacement"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L303)

```cpp
def_readwrite("allow_replacement",
&di::StreamOptions::allowReplacement)
```

### StreamOptions · "enabled"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L302)

```cpp
def_readwrite("enabled",
&di::StreamOptions::enabled)
```

### RequestOptions · "stream"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L338)

```cpp
def_readwrite("stream",
&di::RequestOptions::stream)
```

### RequestOptions · "generation"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L337)

```cpp
def_readwrite("generation",
&di::RequestOptions::generation)
```

### RequestOptions · "output_mode"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L336)

```cpp
def_readwrite("output_mode",
&di::RequestOptions::outputMode)
```

### RequestOptions · "application_request_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L335)

```cpp
def_readwrite("application_request_id",
&di::RequestOptions::applicationRequestId)
```

### RequestOptions · "provider_names"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L334)

```cpp
def_readwrite("provider_names",
&di::RequestOptions::providerNames)
```

### RequestOptions · "placement"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L325)

```cpp
def_property("placement",
[] (const di::RequestOptions& options) { implementation omitted },
[] (di::RequestOptions& options, std::shared_ptr<di::PlacementStrategy> value) { implementation omitted })
```

### RequestOptions · "ack_timeout_s"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L318)

```cpp
def_property("ack_timeout_s",
[] (const di::RequestOptions& options) { implementation omitted },
[] (di::RequestOptions& options, const py::object& value) { implementation omitted })
```

### RequestOptions · "timeout_s"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L311)

```cpp
def_property("timeout_s",
[] (const di::RequestOptions& options) { implementation omitted },
[] (di::RequestOptions& options, const py::object& value) { implementation omitted })
```

### DataRef · "canonical_metadata"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L343)

```cpp
def("canonical_metadata",
&di::DataRef::canonicalMetadata)
```

### DataRef · "from_published_metadata"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L341)

```cpp
def_static("from_published_metadata",
&di::DataRef::fromPublishedMetadata,
py::arg("canonical_reference_json"))
```

### Input · "repository"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L349)

```cpp
def_static("repository",
&di::Input::repository,
py::arg("reference"))
```

### Input · "text"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L348)

```cpp
def_static("text",
&di::Input::text,
py::arg("utf8"))
```

### Input · "inline_bytes"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L346)

```cpp
def_static("inline_bytes",
&di::Input::inlineBytes,
py::arg("payload"),
py::arg("application_options") = std::vector<std::uint8_t>{})
```

### Result · "matches_float32_tensor"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L356)

```cpp
def("matches_float32_tensor",
&di::Result::matchesFloat32Tensor,
py::arg("tensor_name"),
py::arg("expected"),
py::arg("tolerance"))
```

### Result · "plan_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L355)

```cpp
def_readonly("plan_digest",
&di::Result::planDigest)
```

### Result · "model_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L354)

```cpp
def_readonly("model_digest",
&di::Result::modelDigest)
```

### Result · "request_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L353)

```cpp
def_readonly("request_id",
&di::Result::requestId)
```

### Result · "payload"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L352)

```cpp
def_readonly("payload",
&di::Result::payload)
```

### Event · "sequence"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L363)

```cpp
def_readonly("sequence",
&di::Event::sequence)
```

### Event · "terminal"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L362)

```cpp
def_readonly("terminal",
&di::Event::terminal)
```

### Event · "payload"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L361)

```cpp
def_readonly("payload",
&di::Event::payload)
```

### Event · "request_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L360)

```cpp
def_readonly("request_id",
&di::Event::requestId)
```

### RequestDiagnostics · "observation_dropped"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L366)

```cpp
def_readonly("observation_dropped",
&di::RequestDiagnostics::observationDropped)
```

### ConversationCheckpoint · "to_bytes"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L371)

```cpp
def("to_bytes",
&di::ConversationCheckpoint::bytes)
```

### ConversationCheckpoint · "from_bytes"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L369)

```cpp
def_static("from_bytes",
&di::ConversationCheckpoint::fromBytes,
py::arg("bytes"))
```

### ConversationOptions · "checkpoint"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L376)

```cpp
def_readwrite("checkpoint",
&di::ConversationOptions::checkpoint)
```

### ConversationOptions · "conversation_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L375)

```cpp
def_readwrite("conversation_id",
&di::ConversationOptions::conversationId)
```

### Subscription · "__exit__"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L387)

```cpp
def("__exit__",
[] (ndn_service_framework::OperationSubscription& subscription,
                           py::object, py::object, py::object) { implementation omitted })
```

### Subscription · "__enter__"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L383)

```cpp
def("__enter__",
[] (ndn_service_framework::OperationSubscription& subscription)
         -> ndn_service_framework::OperationSubscription& { implementation omitted },
py::return_value_policy::reference_internal)
```

### Subscription · "unsubscribe"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L382)

```cpp
def("unsubscribe",
&ndn_service_framework::OperationSubscription::unsubscribe)
```

### Subscription · "cancel"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L381)

```cpp
def("cancel",
&ndn_service_framework::OperationSubscription::cancel)
```

### NativeRequestStatus · "CANCELLED"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L397)

```cpp
value("CANCELLED",
di::NativeRequestStatus::Cancelled)
```

### NativeRequestStatus · "FAILED"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L396)

```cpp
value("FAILED",
di::NativeRequestStatus::Failed)
```

### NativeRequestStatus · "SUCCEEDED"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L395)

```cpp
value("SUCCEEDED",
di::NativeRequestStatus::Succeeded)
```

### NativeRequestStatus · "PENDING"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L394)

```cpp
value("PENDING",
di::NativeRequestStatus::Pending)
```

### NativeInputTransportMode · "REPOSITORY_REFERENCE"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L402)

```cpp
value("REPOSITORY_REFERENCE",
di::NativeInputTransportMode::RepositoryReference)
```

### NativeInputTransportMode · "INLINE"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L401)

```cpp
value("INLINE",
di::NativeInputTransportMode::Inline)
```

### NativeInvocationMode · "TARGETED"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L407)

```cpp
value("TARGETED",
ndn_service_framework::InvocationMode::Targeted)
```

### NativeInvocationMode · "NORMAL"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L406)

```cpp
value("NORMAL",
ndn_service_framework::InvocationMode::Normal)
```

### NativeControllerVersion · "to_string"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L417)

```cpp
def("to_string",
&ndn_service_framework::ControllerVersion::toString)
```

### NativeControllerVersion · "is_valid"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L416)

```cpp
def("is_valid",
&ndn_service_framework::ControllerVersion::isValid)
```

### NativeControllerVersion · "controller_epoch"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L414)

```cpp
def_readwrite("controller_epoch",
&ndn_service_framework::ControllerVersion::controllerEpoch)
```

### NativeControllerVersion · "controller_generation_timestamp"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L412)

```cpp
def_readwrite("controller_generation_timestamp",
&ndn_service_framework::ControllerVersion::controllerGenerationTimestamp)
```

### NativeGenerationExecutionContractV1 · "streaming_operation_stride"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L456)

```cpp
def_readwrite("streaming_operation_stride",
&di::NativeGenerationExecutionContractV1::streamingOperationStride)
```

### NativeGenerationExecutionContractV1 · "committed_prefix_token_ids"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L454)

```cpp
def_readwrite("committed_prefix_token_ids",
&di::NativeGenerationExecutionContractV1::committedPrefixTokenIds)
```

### NativeGenerationExecutionContractV1 · "generation_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L452)

```cpp
def_readwrite("generation_id",
&di::NativeGenerationExecutionContractV1::generationId)
```

### NativeGenerationExecutionContractV1 · "stop_strings"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L450)

```cpp
def_readwrite("stop_strings",
&di::NativeGenerationExecutionContractV1::stopStrings)
```

### NativeGenerationExecutionContractV1 · "sampling_seed"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L448)

```cpp
def_readwrite("sampling_seed",
&di::NativeGenerationExecutionContractV1::samplingSeed)
```

### NativeGenerationExecutionContractV1 · "sampling_repetition_penalty"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L446)

```cpp
def_readwrite("sampling_repetition_penalty",
&di::NativeGenerationExecutionContractV1::samplingRepetitionPenalty)
```

### NativeGenerationExecutionContractV1 · "sampling_top_p"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L444)

```cpp
def_readwrite("sampling_top_p",
&di::NativeGenerationExecutionContractV1::samplingTopP)
```

### NativeGenerationExecutionContractV1 · "sampling_top_k"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L442)

```cpp
def_readwrite("sampling_top_k",
&di::NativeGenerationExecutionContractV1::samplingTopK)
```

### NativeGenerationExecutionContractV1 · "sampling_temperature"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L440)

```cpp
def_readwrite("sampling_temperature",
&di::NativeGenerationExecutionContractV1::samplingTemperature)
```

### NativeGenerationExecutionContractV1 · "sampling_mode"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L438)

```cpp
def_readwrite("sampling_mode",
&di::NativeGenerationExecutionContractV1::samplingMode)
```

### NativeGenerationExecutionContractV1 · "tokenizer_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L436)

```cpp
def_readwrite("tokenizer_digest",
&di::NativeGenerationExecutionContractV1::tokenizerDigest)
```

### NativeGenerationExecutionContractV1 · "sampling_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L434)

```cpp
def_readwrite("sampling_digest",
&di::NativeGenerationExecutionContractV1::samplingDigest)
```

### NativeGenerationExecutionContractV1 · "eos_token_ids"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L432)

```cpp
def_readwrite("eos_token_ids",
&di::NativeGenerationExecutionContractV1::eosTokenIds)
```

### NativeGenerationExecutionContractV1 · "state_output_names"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L430)

```cpp
def_readwrite("state_output_names",
&di::NativeGenerationExecutionContractV1::stateOutputNames)
```

### NativeGenerationExecutionContractV1 · "state_input_names"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L428)

```cpp
def_readwrite("state_input_names",
&di::NativeGenerationExecutionContractV1::stateInputNames)
```

### NativeGenerationExecutionContractV1 · "token_input_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L426)

```cpp
def_readwrite("token_input_name",
&di::NativeGenerationExecutionContractV1::tokenInputName)
```

### NativeGenerationExecutionContractV1 · "max_generated_tokens"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L424)

```cpp
def_readwrite("max_generated_tokens",
&di::NativeGenerationExecutionContractV1::maxGeneratedTokens)
```

### NativeGenerationExecutionContractV1 · "mode"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L423)

```cpp
def_readwrite("mode",
&di::NativeGenerationExecutionContractV1::mode)
```

### NativeGenerationExecutionContractV1 · "enabled"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L422)

```cpp
def_readwrite("enabled",
&di::NativeGenerationExecutionContractV1::enabled)
```

### NativeConversationContinuation · "expected_roles"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L480)

```cpp
def_readwrite("expected_roles",
&di::NativeConversationContinuation::expectedRoles)
```

### NativeConversationContinuation · "canonical_token_ids"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L478)

```cpp
def_readwrite("canonical_token_ids",
&di::NativeConversationContinuation::canonicalTokenIds)
```

### NativeConversationContinuation · "generation_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L477)

```cpp
def_readwrite("generation_id",
&di::NativeConversationContinuation::generationId)
```

### NativeConversationContinuation · "parent_checkpoint_wire"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L475)

```cpp
def_readwrite("parent_checkpoint_wire",
&di::NativeConversationContinuation::parentCheckpointWire)
```

### NativeConversationContinuation · "mode"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L474)

```cpp
def_readwrite("mode",
&di::NativeConversationContinuation::mode)
```

### NativeConversationContinuation · "retention_deadline_ms"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L472)

```cpp
def_readwrite("retention_deadline_ms",
&di::NativeConversationContinuation::retentionDeadlineMs)
```

### NativeConversationContinuation · "request_contract_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L470)

```cpp
def_readwrite("request_contract_digest",
&di::NativeConversationContinuation::requestContractDigest)
```

### NativeConversationContinuation · "parent_checkpoint_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L468)

```cpp
def_readwrite("parent_checkpoint_digest",
&di::NativeConversationContinuation::parentCheckpointDigest)
```

### NativeConversationContinuation · "plan_role_map_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L466)

```cpp
def_readwrite("plan_role_map_digest",
&di::NativeConversationContinuation::planRoleMapDigest)
```

### NativeConversationContinuation · "service_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L465)

```cpp
def_readwrite("service_name",
&di::NativeConversationContinuation::serviceName)
```

### NativeConversationContinuation · "parent_context_epoch"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L463)

```cpp
def_readwrite("parent_context_epoch",
&di::NativeConversationContinuation::parentContextEpoch)
```

### NativeConversationContinuation · "conversation_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L462)

```cpp
def_readwrite("conversation_id",
&di::NativeConversationContinuation::conversationId)
```

### NativeStreamRequestOptions · "wire_decode"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L529)

```cpp
def("wire_decode",
[] (ndn_service_framework::StreamRequestOptions& options,
                              const py::bytes& wireBytes) { implementation omitted })
```

### NativeStreamRequestOptions · "wire_encode"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L525)

```cpp
def("wire_encode",
[] (const ndn_service_framework::StreamRequestOptions& options) { implementation omitted })
```

### NativeStreamRequestOptions · "validate"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L524)

```cpp
def("validate",
&ndn_service_framework::StreamRequestOptions::validate)
```

### NativeStreamRequestOptions · "max_replacements"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L522)

```cpp
def_readwrite("max_replacements",
&ndn_service_framework::StreamRequestOptions::maxReplacements)
```

### NativeStreamRequestOptions · "allow_replacement"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L520)

```cpp
def_readwrite("allow_replacement",
&ndn_service_framework::StreamRequestOptions::allowReplacement)
```

### NativeStreamRequestOptions · "max_event_wire_bytes"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L518)

```cpp
def_readwrite("max_event_wire_bytes",
&ndn_service_framework::StreamRequestOptions::maxEventWireBytes)
```

### NativeStreamRequestOptions · "completion_grace_ms"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L516)

```cpp
def_readwrite("completion_grace_ms",
&ndn_service_framework::StreamRequestOptions::completionGraceMs)
```

### NativeStreamRequestOptions · "retention_ms"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L515)

```cpp
def_readwrite("retention_ms",
&ndn_service_framework::StreamRequestOptions::retentionMs)
```

### NativeStreamRequestOptions · "reorder_capacity"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L513)

```cpp
def_readwrite("reorder_capacity",
&ndn_service_framework::StreamRequestOptions::reorderCapacity)
```

### NativeStreamRequestOptions · "callback_queue_capacity"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L511)

```cpp
def_readwrite("callback_queue_capacity",
&ndn_service_framework::StreamRequestOptions::callbackQueueCapacity)
```

### NativeStreamRequestOptions · "publisher_queue_capacity"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L509)

```cpp
def_readwrite("publisher_queue_capacity",
&ndn_service_framework::StreamRequestOptions::publisherQueueCapacity)
```

### NativeStreamRequestOptions · "max_event_retries"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L507)

```cpp
def_readwrite("max_event_retries",
&ndn_service_framework::StreamRequestOptions::maxEventRetries)
```

### NativeStreamRequestOptions · "interest_lifetime_ms"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L505)

```cpp
def_readwrite("interest_lifetime_ms",
&ndn_service_framework::StreamRequestOptions::interestLifetimeMs)
```

### NativeStreamRequestOptions · "interest_window"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L504)

```cpp
def_readwrite("interest_window",
&ndn_service_framework::StreamRequestOptions::interestWindow)
```

### NativeStreamRequestOptions · "max_events"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L503)

```cpp
def_readwrite("max_events",
&ndn_service_framework::StreamRequestOptions::maxEvents)
```

### NativeStreamRequestOptions · "event_key_grant_wire"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L496)

```cpp
def_property("event_key_grant_wire",
[] (const ndn_service_framework::StreamRequestOptions& options) { implementation omitted },
[] (ndn_service_framework::StreamRequestOptions& options, const py::object& value) { implementation omitted })
```

### NativeStreamRequestOptions · "controller_version"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L494)

```cpp
def_readwrite("controller_version",
&ndn_service_framework::StreamRequestOptions::controllerVersion)
```

### NativeStreamRequestOptions · "deadline_epoch_ms"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L492)

```cpp
def_readwrite("deadline_epoch_ms",
&ndn_service_framework::StreamRequestOptions::deadlineEpochMs)
```

### NativeStreamRequestOptions · "event_key_commitment"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L490)

```cpp
def_readwrite("event_key_commitment",
&ndn_service_framework::StreamRequestOptions::eventKeyCommitment)
```

### NativeStreamRequestOptions · "stream_epoch"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L489)

```cpp
def_readwrite("stream_epoch",
&ndn_service_framework::StreamRequestOptions::streamEpoch)
```

### NativeStreamRequestOptions · "attempt_epoch"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L488)

```cpp
def_readwrite("attempt_epoch",
&ndn_service_framework::StreamRequestOptions::attemptEpoch)
```

### NativeStreamRequestOptions · "generation_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L487)

```cpp
def_readwrite("generation_id",
&ndn_service_framework::StreamRequestOptions::generationId)
```

### NativeStreamRequestOptions · "mode"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L486)

```cpp
def_readwrite("mode",
&ndn_service_framework::StreamRequestOptions::mode)
```

### NativeStreamRequestOptions · "version"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L485)

```cpp
def_readwrite("version",
&ndn_service_framework::StreamRequestOptions::version)
```

### NativeAdapterDescriptor · "descriptor_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L558)

```cpp
def_property_readonly("descriptor_digest",
&di::NativeAdapterDescriptor::descriptorDigest)
```

### NativeAdapterDescriptor · "canonical_json"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L557)

```cpp
def("canonical_json",
&di::NativeAdapterDescriptor::canonicalJson)
```

### NativeAdapterDescriptor · "deterministic_analysis"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L556)

```cpp
def_readwrite("deterministic_analysis",
&di::NativeAdapterDescriptor::deterministicAnalysis)
```

### NativeAdapterDescriptor · "splittable"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L555)

```cpp
def_readwrite("splittable",
&di::NativeAdapterDescriptor::splittable)
```

### NativeAdapterDescriptor · "graph_inspectable"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L554)

```cpp
def_readwrite("graph_inspectable",
&di::NativeAdapterDescriptor::graphInspectable)
```

### NativeAdapterDescriptor · "state_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L553)

```cpp
def_readwrite("state_schema_digest",
&di::NativeAdapterDescriptor::stateSchemaDigest)
```

### NativeAdapterDescriptor · "split_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L552)

```cpp
def_readwrite("split_schema_digest",
&di::NativeAdapterDescriptor::splitSchemaDigest)
```

### NativeAdapterDescriptor · "graph_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L551)

```cpp
def_readwrite("graph_schema_digest",
&di::NativeAdapterDescriptor::graphSchemaDigest)
```

### NativeAdapterDescriptor · "result_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L550)

```cpp
def_readwrite("result_schema_digest",
&di::NativeAdapterDescriptor::resultSchemaDigest)
```

### NativeAdapterDescriptor · "options_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L549)

```cpp
def_readwrite("options_schema_digest",
&di::NativeAdapterDescriptor::optionsSchemaDigest)
```

### NativeAdapterDescriptor · "input_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L548)

```cpp
def_readwrite("input_schema_digest",
&di::NativeAdapterDescriptor::inputSchemaDigest)
```

### NativeAdapterDescriptor · "precisions"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L547)

```cpp
def_readwrite("precisions",
&di::NativeAdapterDescriptor::precisions)
```

### NativeAdapterDescriptor · "backends"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L546)

```cpp
def_readwrite("backends",
&di::NativeAdapterDescriptor::backends)
```

### NativeAdapterDescriptor · "tasks"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L545)

```cpp
def_readwrite("tasks",
&di::NativeAdapterDescriptor::tasks)
```

### NativeAdapterDescriptor · "model_formats"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L544)

```cpp
def_readwrite("model_formats",
&di::NativeAdapterDescriptor::modelFormats)
```

### NativeAdapterDescriptor · "abi"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L543)

```cpp
def_readwrite("abi",
&di::NativeAdapterDescriptor::abi)
```

### NativeAdapterDescriptor · "state_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L542)

```cpp
def_readwrite("state_digest",
&di::NativeAdapterDescriptor::stateDigest)
```

### NativeAdapterDescriptor · "version"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L541)

```cpp
def_readwrite("version",
&di::NativeAdapterDescriptor::version)
```

### NativeAdapterDescriptor · "name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L540)

```cpp
def_readwrite("name",
&di::NativeAdapterDescriptor::name)
```

### NativeModelDescriptor · "model_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L573)

```cpp
def_property_readonly("model_digest",
&di::NativeModelDescriptor::modelDigest)
```

### NativeModelDescriptor · "canonical_json"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L572)

```cpp
def("canonical_json",
&di::NativeModelDescriptor::canonicalJson)
```

### NativeModelDescriptor · "source_revision"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L571)

```cpp
def_readwrite("source_revision",
&di::NativeModelDescriptor::sourceRevision)
```

### NativeModelDescriptor · "adapter"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L570)

```cpp
def_readwrite("adapter",
&di::NativeModelDescriptor::adapter)
```

### NativeModelDescriptor · "adapter_version"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L569)

```cpp
def_readwrite("adapter_version",
&di::NativeModelDescriptor::adapterVersion)
```

### NativeModelDescriptor · "adapter_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L568)

```cpp
def_readwrite("adapter_id",
&di::NativeModelDescriptor::adapterId)
```

### NativeModelDescriptor · "precision"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L567)

```cpp
def_readwrite("precision",
&di::NativeModelDescriptor::precision)
```

### NativeModelDescriptor · "model_format"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L566)

```cpp
def_readwrite("model_format",
&di::NativeModelDescriptor::modelFormat)
```

### NativeModelDescriptor · "graph_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L565)

```cpp
def_readwrite("graph_digest",
&di::NativeModelDescriptor::graphDigest)
```

### NativeModelDescriptor · "semantics_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L564)

```cpp
def_readwrite("semantics_digest",
&di::NativeModelDescriptor::semanticsDigest)
```

### NativeModelDescriptor · "content_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L563)

```cpp
def_readwrite("content_digest",
&di::NativeModelDescriptor::contentDigest)
```

### NativeModelDescriptor · "model_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L562)

```cpp
def_readwrite("model_name",
&di::NativeModelDescriptor::modelName)
```

### NativeApplicationInput · "repository_reference"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L586)

```cpp
def_readwrite("repository_reference",
&di::NativeApplicationInput::repositoryReference)
```

### NativeApplicationInput · "transport_mode"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L585)

```cpp
def_readwrite("transport_mode",
&di::NativeApplicationInput::transportMode)
```

### NativeApplicationInput · "options"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L584)

```cpp
def_readwrite("options",
&di::NativeApplicationInput::options)
```

### NativeApplicationInput · "payload"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L583)

```cpp
def_readwrite("payload",
&di::NativeApplicationInput::payload)
```

### NativeApplicationInput · "options_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L582)

```cpp
def_readwrite("options_schema_digest",
&di::NativeApplicationInput::optionsSchemaDigest)
```

### NativeApplicationInput · "input_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L581)

```cpp
def_readwrite("input_schema_digest",
&di::NativeApplicationInput::inputSchemaDigest)
```

### NativeApplicationInput · "task_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L580)

```cpp
def_readwrite("task_name",
&di::NativeApplicationInput::taskName)
```

### NativeRequestOptions · "conversation"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L597)

```cpp
def_readwrite("conversation",
&di::NativeRequestOptions::conversation)
```

### NativeRequestOptions · "stream"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L596)

```cpp
def_readwrite("stream",
&di::NativeRequestOptions::stream)
```

### NativeRequestOptions · "generation"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L595)

```cpp
def_readwrite("generation",
&di::NativeRequestOptions::generation)
```

### NativeRequestOptions · "application_request_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L594)

```cpp
def_readwrite("application_request_id",
&di::NativeRequestOptions::applicationRequestId)
```

### NativeRequestOptions · "output_mode"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L593)

```cpp
def_readwrite("output_mode",
&di::NativeRequestOptions::outputMode)
```

### NativeRequestOptions · "task_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L592)

```cpp
def_readwrite("task_name",
&di::NativeRequestOptions::taskName)
```

### NativeRequestOptions · "ack_timeout_ms"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L591)

```cpp
def_readwrite("ack_timeout_ms",
&di::NativeRequestOptions::ackTimeoutMs)
```

### NativeRequestOptions · "timeout_ms"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L590)

```cpp
def_readwrite("timeout_ms",
&di::NativeRequestOptions::timeoutMs)
```

### NativeRequestContract · "tokenizer_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L608)

```cpp
def_readwrite("tokenizer_digest",
&di::NativeRequestContract::tokenizerDigest)
```

### NativeRequestContract · "generation_mode"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L607)

```cpp
def_readwrite("generation_mode",
&di::NativeRequestContract::generationMode)
```

### NativeRequestContract · "task_descriptor_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L606)

```cpp
def_readwrite("task_descriptor_digest",
&di::NativeRequestContract::taskDescriptorDigest)
```

### NativeRequestContract · "adapter_composition_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L605)

```cpp
def_readwrite("adapter_composition_digest",
&di::NativeRequestContract::adapterCompositionDigest)
```

### NativeRequestContract · "adapter_descriptor_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L604)

```cpp
def_readwrite("adapter_descriptor_digest",
&di::NativeRequestContract::adapterDescriptorDigest)
```

### NativeRequestContract · "adapter_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L603)

```cpp
def_readwrite("adapter_name",
&di::NativeRequestContract::adapterName)
```

### NativeRequestContract · "task_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L602)

```cpp
def_readwrite("task_name",
&di::NativeRequestContract::taskName)
```

### NativeRequestContract · "service_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L601)

```cpp
def_readwrite("service_name",
&di::NativeRequestContract::serviceName)
```

### NativeSecurityPolicySnapshot · "require_protected_artifacts"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L613)

```cpp
def_readwrite("require_protected_artifacts",
&di::NativeSecurityPolicySnapshot::requireProtectedArtifacts)
```

### NativeSecurityPolicySnapshot · "policy_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L612)

```cpp
def_readwrite("policy_digest",
&di::NativeSecurityPolicySnapshot::policyDigest)
```

### NativeCandidateBudget · "max_reentries"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L619)

```cpp
def_readwrite("max_reentries",
&di::NativeCandidateBudget::maxReentries)
```

### NativeCandidateBudget · "max_policy_ms"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L618)

```cpp
def_readwrite("max_policy_ms",
&di::NativeCandidateBudget::maxPolicyMs)
```

### NativeCandidateBudget · "max_candidates"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L617)

```cpp
def_readwrite("max_candidates",
&di::NativeCandidateBudget::maxCandidates)
```

### NativeStateTensorMapping · "outputs"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L624)

```cpp
def_readwrite("outputs",
&di::NativeStateTensorMapping::outputs)
```

### NativeStateTensorMapping · "inputs"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L623)

```cpp
def_readwrite("inputs",
&di::NativeStateTensorMapping::inputs)
```

### NativeCanonicalPreparationCatalog · "adapters"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L633)

```cpp
def_property_readonly("adapters",
&di::NativeCanonicalPreparationCatalog::adapters)
```

### NativeRequestCatalog · "load"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L676)

```cpp
def_static("load",
[](const std::string& configuration_json,
                            const py::bytes& model_bytes,
                            const py::object& initializer_bytes,
                            std::uint64_t max_source_bytes,
                            std::uint64_t max_assembled_bytes) { implementation omitted },
py::arg("configuration_json"),
py::arg("model_bytes"),
py::arg("initializer_bytes") = py::none(),
py::arg("max_source_bytes") = 256ULL * 1024ULL * 1024ULL,
py::arg("max_assembled_bytes") = 512ULL * 1024ULL * 1024ULL)
```

### NativeRequestCatalog · "state_mapping"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L675)

```cpp
def_readonly("state_mapping",
&di::NativeRequestCatalog::stateMapping)
```

### NativeRequestCatalog · "splitter"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L674)

```cpp
def_readonly("splitter",
&di::NativeRequestCatalog::splitter)
```

### NativeRequestCatalog · "preparation"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L673)

```cpp
def_readonly("preparation",
&di::NativeRequestCatalog::preparation)
```

### NativeRequestCatalog · "canonical_initializer_object_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L670)

```cpp
def_property_readonly("canonical_initializer_object_digest",
[] (const di::NativeRequestCatalog& catalog) { implementation omitted })
```

### NativeRequestCatalog · "canonical_source_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L667)

```cpp
def_property_readonly("canonical_source_digest",
[] (const di::NativeRequestCatalog& catalog) { implementation omitted })
```

### NativeRequestCatalog · "model_manifest_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L664)

```cpp
def_property_readonly("model_manifest_digest",
[] (const di::NativeRequestCatalog& catalog) { implementation omitted })
```

### NativeRequestCatalog · "model_ref"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L659)

```cpp
def_property_readonly("model_ref",
[] (const di::NativeRequestCatalog& catalog) { implementation omitted })
```

### NativeRequestRuntime · "max_segments"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L713)

```cpp
def_readwrite("max_segments",
&di::NativeRequestRuntime::maxSegments)
```

### NativeRequestRuntime · "no_progress_ms"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L712)

```cpp
def_readwrite("no_progress_ms",
&di::NativeRequestRuntime::noProgressMs)
```

### NativeRequestRuntime · "state_mapping"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L711)

```cpp
def_readwrite("state_mapping",
&di::NativeRequestRuntime::stateMapping)
```

### NativeRequestRuntime · "catalog"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L710)

```cpp
def_readwrite("catalog",
&di::NativeRequestRuntime::catalog)
```

### NativeRequestRuntime · "grants"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L709)

```cpp
def_readwrite("grants",
&di::NativeRequestRuntime::grants)
```

### NativeRequestRuntime · "budget"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L708)

```cpp
def_readwrite("budget",
&di::NativeRequestRuntime::budget)
```

### NativeRequestRuntime · "security"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L707)

```cpp
def_readwrite("security",
&di::NativeRequestRuntime::security)
```

### NativeRequestRuntime · "input_layout_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L706)

```cpp
def_readwrite("input_layout_digest",
&di::NativeRequestRuntime::inputLayoutDigest)
```

### NativeRequestRuntime · "protection_epoch"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L705)

```cpp
def_readwrite("protection_epoch",
&di::NativeRequestRuntime::protectionEpoch)
```

### NativeRequestRuntime · "requester_identity"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L704)

```cpp
def_readwrite("requester_identity",
&di::NativeRequestRuntime::requesterIdentity)
```

### NativeRequestRuntime · "contract"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L703)

```cpp
def_readwrite("contract",
&di::NativeRequestRuntime::contract)
```

### module · "native_request_runtime_from_json"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L715)

```cpp
def("native_request_runtime_from_json",
[](const std::string& configuration_json,
                const di::NativeRequestCatalog& catalog,
                std::shared_ptr<const di::NativeAuthenticatedGrantClient> grants) { implementation omitted },
py::arg("configuration_json"),
py::arg("catalog"),
py::arg("grants"))
```

### NativeInferenceResult · "plan_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L727)

```cpp
def_readonly("plan_digest",
&di::NativeInferenceResult::planDigest)
```

### NativeInferenceResult · "model_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L726)

```cpp
def_readonly("model_digest",
&di::NativeInferenceResult::modelDigest)
```

### NativeInferenceResult · "payload"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L725)

```cpp
def_readonly("payload",
&di::NativeInferenceResult::payload)
```

### NativeInferenceEvent · "terminal"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L732)

```cpp
def_readonly("terminal",
&di::NativeInferenceEvent::terminal)
```

### NativeInferenceEvent · "payload"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L731)

```cpp
def_readonly("payload",
&di::NativeInferenceEvent::payload)
```

### NativeInferenceEvent · "request_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L730)

```cpp
def_readonly("request_id",
&di::NativeInferenceEvent::requestId)
```

### NativeAdapterRegistry · "frozen"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L739)

```cpp
def_property_readonly("frozen",
&di::NativeAdapterRegistry::frozen)
```

### NativeAdapterRegistry · "find"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L738)

```cpp
def("find",
&di::NativeAdapterRegistry::find)
```

### NativeAdapterRegistry · "freeze"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L737)

```cpp
def("freeze",
&di::NativeAdapterRegistry::freeze)
```

### NativeYoloComponentSpec · "candidate_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L777)

```cpp
def_readwrite("candidate_digest",
&di::yolo::NativeYoloComponentSpec::candidateDigest)
```

### NativeYoloComponentSpec · "merge_kind"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L776)

```cpp
def_readwrite("merge_kind",
&di::yolo::NativeYoloComponentSpec::mergeKind)
```

### NativeYoloComponentSpec · "result_egress_role"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L774)

```cpp
def_readwrite("result_egress_role",
&di::yolo::NativeYoloComponentSpec::resultEgressRole)
```

### NativeYoloComponentSpec · "input_ingress_role"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L772)

```cpp
def_readwrite("input_ingress_role",
&di::yolo::NativeYoloComponentSpec::inputIngressRole)
```

### NativeYoloComponentSpec · "node_names_by_role"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L770)

```cpp
def_readwrite("node_names_by_role",
&di::yolo::NativeYoloComponentSpec::nodeNamesByRole)
```

### NativeYoloComponentSpec · "roles"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L769)

```cpp
def_readwrite("roles",
&di::yolo::NativeYoloComponentSpec::roles)
```

### NativeYoloComponentSpec · "priority"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L768)

```cpp
def_readwrite("priority",
&di::yolo::NativeYoloComponentSpec::priority)
```

### NativeYoloComponentSpec · "candidate_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L767)

```cpp
def_readwrite("candidate_id",
&di::yolo::NativeYoloComponentSpec::candidateId)
```

### NativeInferenceHandle · "status_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L819)

```cpp
def_property_readonly("status_name",
[](const di::NativeInferenceHandle& handle) { implementation omitted })
```

### NativeInferenceHandle · "observe"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L803)

```cpp
def("observe",
[] (di::NativeInferenceHandle& handle,
                         py::function observer) { implementation omitted },
py::arg("observer"))
```

### NativeInferenceHandle · "cancel"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L802)

```cpp
def("cancel",
&di::NativeInferenceHandle::cancel)
```

### NativeInferenceHandle · "result"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L797)

```cpp
def("result",
[](const di::NativeInferenceHandle& handle,
                       std::uint64_t wait_timeout_ms) { implementation omitted },
py::arg("wait_timeout_ms") = 0)
```

### NativeInferenceHandle · "status"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L796)

```cpp
def_property_readonly("status",
&di::NativeInferenceHandle::status)
```

### NativeInferenceHandle · "conversation_checkpoint"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L795)

```cpp
def_property_readonly("conversation_checkpoint",
&di::NativeInferenceHandle::conversationCheckpoint)
```

### NativeInferenceHandle · "application_request_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L794)

```cpp
def_property_readonly("application_request_id",
&di::NativeInferenceHandle::applicationRequestId)
```

### NativeInferenceHandle · "request_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L793)

```cpp
def_property_readonly("request_id",
&di::NativeInferenceHandle::requestId)
```

### NativeInferenceClient · "request"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L826)

```cpp
def("request",
&di::NativeInferenceClient::request,
py::arg("model"),
py::arg("input"),
py::arg("split_strategy"),
py::arg("placement_strategy"),
py::arg("options"))
```

### NativeInferenceClient · "close"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L825)

```cpp
def("close",
&di::NativeInferenceClient::close)
```

### EventReader · "__exit__"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L874)

```cpp
def("__exit__",
[] (di::EventReader& reader, py::object, py::object, py::object) { implementation omitted })
```

### EventReader · "__enter__"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L871)

```cpp
def("__enter__",
[] (di::EventReader& reader) -> di::EventReader& { implementation omitted },
py::return_value_policy::reference_internal)
```

### EventReader · "close"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L870)

```cpp
def("close",
&di::EventReader::close)
```

### EventReader · "next_async"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L839)

```cpp
def("next_async",
[] (const di::EventReader& reader,
                             const py::object& timeout,
                             const py::object& callback) -> py::object { implementation omitted },
py::kw_only(),
py::arg("timeout_s") = py::none(),
py::arg("callback") = py::none())
```

### EventReader · "next"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L831)

```cpp
def("next",
[] (di::EventReader& reader, const py::object& timeout) { implementation omitted },
py::kw_only(),
py::arg("timeout_s") = py::none())
```

### RequestHandle · "cancel"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L977)

```cpp
def("cancel",
&di::RequestHandle::cancel)
```

### RequestHandle · "observe"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L964)

```cpp
def("observe",
[] (const di::RequestHandle& handle, py::function callback) { implementation omitted },
py::arg("callback"))
```

### RequestHandle · "result_async"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L931)

```cpp
def("result_async",
[] (const di::RequestHandle& handle, const py::object& timeout,
                              const py::object& callback) -> py::object { implementation omitted },
py::kw_only(),
py::arg("timeout_s") = py::none(),
py::arg("callback") = py::none())
```

### RequestHandle · "on_completion"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L917)

```cpp
def("on_completion",
[] (const di::RequestHandle& handle, py::function callback) { implementation omitted },
py::arg("callback"))
```

### RequestHandle · "diagnostics"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L916)

```cpp
def_property_readonly("diagnostics",
&di::RequestHandle::diagnostics)
```

### RequestHandle · "events_async"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L910)

```cpp
def("events_async",
[] (const di::RequestHandle& handle,
                               const py::object& timeout) { implementation omitted },
py::kw_only(),
py::arg("timeout_s") = py::none())
```

### RequestHandle · "events"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L909)

```cpp
def("events",
&di::RequestHandle::events)
```

### RequestHandle · "wait"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L900)

```cpp
def("wait",
[] (const di::RequestHandle& handle, const py::object& timeout) { implementation omitted },
py::kw_only(),
py::arg("timeout_s") = py::none())
```

### RequestHandle · "result"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L891)

```cpp
def("result",
[] (const di::RequestHandle& handle, const py::object& timeout) { implementation omitted },
py::kw_only(),
py::arg("timeout_s") = py::none())
```

### RequestHandle · "status_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L882)

```cpp
def_property_readonly("status_name",
[] (const di::RequestHandle& handle) { implementation omitted })
```

### RequestHandle · "status"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L881)

```cpp
def_property_readonly("status",
&di::RequestHandle::status)
```

### RequestHandle · "id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L880)

```cpp
def_property_readonly("id",
&di::RequestHandle::id)
```

### PreparedModel · "open_conversation"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L994)

```cpp
def("open_conversation",
&di::PreparedModel::openConversation,
py::kw_only(),
py::arg("options") = di::ConversationOptions{})
```

### PreparedModel · "run"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L988)

```cpp
def("run",
[] (const di::PreparedModel& model, const di::Input& input,
                     const di::RequestOptions& options) { implementation omitted },
py::arg("input"),
py::kw_only(),
py::arg("options") = di::RequestOptions{})
```

### PreparedModel · "request"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L985)

```cpp
def("request",
&di::PreparedModel::request,
py::arg("input"),
py::kw_only(),
py::arg("options") = di::RequestOptions{})
```

### PreparedModel · "capabilities"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L984)

```cpp
def_property_readonly("capabilities",
&di::PreparedModel::capabilities)
```

### PreparedModel · "receipt"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L982)

```cpp
def_property_readonly("receipt",
&di::PreparedModel::receipt,
py::return_value_policy::reference_internal)
```

### PreparedModel · "manifest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L980)

```cpp
def_property_readonly("manifest",
&di::PreparedModel::manifest,
py::return_value_policy::reference_internal)
```

### PreparationHandle · "cancel"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1064)

```cpp
def("cancel",
&di::PreparationHandle::cancel)
```

### PreparationHandle · "on_completion"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1050)

```cpp
def("on_completion",
[] (const di::PreparationHandle& handle, py::function callback) { implementation omitted },
py::arg("callback"))
```

### PreparationHandle · "result_async"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1017)

```cpp
def("result_async",
[] (const di::PreparationHandle& handle, const py::object& timeout,
                              const py::object& callback) -> py::object { implementation omitted },
py::kw_only(),
py::arg("timeout_s") = py::none(),
py::arg("callback") = py::none())
```

### PreparationHandle · "result"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1008)

```cpp
def("result",
[] (const di::PreparationHandle& handle, const py::object& timeout) { implementation omitted },
py::kw_only(),
py::arg("timeout_s") = py::none())
```

### PreparationHandle · "status_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L999)

```cpp
def_property_readonly("status_name",
[] (const di::PreparationHandle& handle) { implementation omitted })
```

### PreparationHandle · "status"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L998)

```cpp
def_property_readonly("status",
&di::PreparationHandle::status)
```

### Conversation · "__exit__"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1080)

```cpp
def("__exit__",
[] (di::Conversation& conversation, py::object, py::object, py::object) { implementation omitted })
```

### Conversation · "__enter__"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1077)

```cpp
def("__enter__",
[] (di::Conversation& conversation) -> di::Conversation& { implementation omitted },
py::return_value_policy::reference_internal)
```

### Conversation · "close"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1076)

```cpp
def("close",
&di::Conversation::close)
```

### Conversation · "export_checkpoint"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1071)

```cpp
def("export_checkpoint",
[] (const di::Conversation& conversation,
                                   const std::string& destination) { implementation omitted },
py::arg("destination"))
```

### Conversation · "checkpoint"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1070)

```cpp
def("checkpoint",
&di::Conversation::checkpoint)
```

### Conversation · "request"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1067)

```cpp
def("request",
&di::Conversation::request,
py::arg("input"),
py::kw_only(),
py::arg("options") = di::RequestOptions{})
```

### NativeProviderConfig · "equivalent"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1089)

```cpp
def("equivalent",
&di::ProviderConfig::equivalent)
```

### NativeProviderConfig · "valid"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1088)

```cpp
def("valid",
&di::ProviderConfig::valid)
```

### NativeProviderConfig · "from_file"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1087)

```cpp
def_static("from_file",
&di::ProviderConfig::fromFile,
py::arg("path"))
```

### ServiceDefinition · "allowed_roles"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1094)

```cpp
def_readwrite("allowed_roles",
&di::ServiceDefinition::allowedRoles)
```

### ServiceDefinition · "service_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1093)

```cpp
def_readwrite("service_name",
&di::ServiceDefinition::serviceName)
```

### ProviderCounters · "active_leases"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1101)

```cpp
def_readonly("active_leases",
&di::ProviderCounters::activeLeases)
```

### ProviderCounters · "runners_created"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1100)

```cpp
def_readonly("runners_created",
&di::ProviderCounters::runnersCreated)
```

### ProviderCounters · "template_hits"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1099)

```cpp
def_readonly("template_hits",
&di::ProviderCounters::templateHits)
```

### ProviderCounters · "assemblies"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1098)

```cpp
def_readonly("assemblies",
&di::ProviderCounters::assemblies)
```

### ProviderCounters · "source_fetches"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1097)

```cpp
def_readonly("source_fetches",
&di::ProviderCounters::sourceFetches)
```

### ProviderRegistration · "__exit__"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1111)

```cpp
def("__exit__",
[] (di::ProviderRegistration& registration, py::object, py::object, py::object) { implementation omitted })
```

### ProviderRegistration · "__enter__"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1108)

```cpp
def("__enter__",
[] (di::ProviderRegistration& registration) -> di::ProviderRegistration& { implementation omitted },
py::return_value_policy::reference_internal)
```

### ProviderRegistration · "service_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1107)

```cpp
def_property_readonly("service_name",
&di::ProviderRegistration::serviceName)
```

### ProviderRegistration · "valid"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1106)

```cpp
def("valid",
&di::ProviderRegistration::valid)
```

### ProviderRegistration · "closed"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1105)

```cpp
def("closed",
&di::ProviderRegistration::closed)
```

### ProviderRegistration · "close"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1104)

```cpp
def("close",
&di::ProviderRegistration::close)
```

### Provider · "counters"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1148)

```cpp
def_property_readonly("counters",
&di::Provider::counters)
```

### Provider · "valid"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1147)

```cpp
def("valid",
&di::Provider::valid)
```

### Provider · "drain_async"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1128)

```cpp
def("drain_async",
[] (const di::Provider& provider, const py::object& timeout,
                             const py::object& callback) -> py::object { implementation omitted },
py::kw_only(),
py::arg("timeout_s") = py::none(),
py::arg("callback") = py::none())
```

### Provider · "drain"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1123)

```cpp
def("drain",
[] (const di::Provider& provider, const py::object& timeout) { implementation omitted },
py::kw_only(),
py::arg("timeout_s") = py::none())
```

### Provider · "stop"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1122)

```cpp
def("stop",
&di::Provider::stop)
```

### Provider · "serve"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1118)

```cpp
def("serve",
[] (di::Provider& provider, const di::ServiceDefinition& definition) { implementation omitted },
py::arg("definition"))
```

### User · "prepare_async"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1167)

```cpp
def("prepare_async",
[] (const di::User& user, const std::string& modelKey,
                                const di::PrepareOptions& options,
                                const py::object& timeout) { implementation omitted },
py::arg("model_key") = "default",
py::kw_only(),
py::arg("options") = di::PrepareOptions{},
py::arg("timeout_s") = py::none())
```

### User · "start_prepare"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1159)

```cpp
def("start_prepare",
[] (const di::User& user, const std::string& modelKey,
                                di::PrepareOptions options, const py::object& timeout) { implementation omitted },
py::arg("model_key") = "default",
py::kw_only(),
py::arg("options") = di::PrepareOptions{},
py::arg("timeout_s") = py::none())
```

### User · "prepare"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1151)

```cpp
def("prepare",
[] (const di::User& user, const std::string& modelKey,
                         di::PrepareOptions options, const py::object& timeout) { implementation omitted },
py::arg("model_key") = "default",
py::kw_only(),
py::arg("options") = di::PrepareOptions{},
py::arg("timeout_s") = py::none())
```

### Runtime · "__aexit__"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1249)

```cpp
def("__aexit__",
[] (std::shared_ptr<di::Runtime> runtime,
                            py::object excType, py::object exc, py::object traceback) { implementation omitted })
```

### Runtime · "__aenter__"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1245)

```cpp
def("__aenter__",
[] (std::shared_ptr<di::Runtime> runtime) { implementation omitted })
```

### Runtime · "__exit__"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1227)

```cpp
def("__exit__",
[] (di::Runtime& runtime, py::object excType,
                           py::object, py::object) { implementation omitted })
```

### Runtime · "__enter__"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1224)

```cpp
def("__enter__",
[] (di::Runtime& runtime) -> di::Runtime& { implementation omitted },
py::return_value_policy::reference_internal)
```

### Runtime · "drain_async"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1204)

```cpp
def("drain_async",
[] (std::shared_ptr<di::Runtime> runtime,
                             const py::object& timeout,
                             const py::object& callback) -> py::object { implementation omitted },
py::kw_only(),
py::arg("timeout_s") = py::none(),
py::arg("callback") = py::none())
```

### Runtime · "drain"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1199)

```cpp
def("drain",
[] (const di::Runtime& runtime, const py::object& timeout) { implementation omitted },
py::kw_only(),
py::arg("timeout_s") = py::none())
```

### Runtime · "close"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1198)

```cpp
def("close",
&di::Runtime::close)
```

### Runtime · "provider"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1195)

```cpp
def("provider",
[] (di::Runtime& runtime) { implementation omitted })
```

### Runtime · "provider"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1192)

```cpp
def("provider",
[] (di::Runtime& runtime, const di::ProviderConfig& config) { implementation omitted },
py::arg("config"))
```

### Runtime · "placement_strategy"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1187)

```cpp
def("placement_strategy",
[] (const di::Runtime& runtime, const std::string& id) { implementation omitted },
py::arg("id"))
```

### Runtime · "user"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1186)

```cpp
def("user",
&di::Runtime::user,
py::arg("config") = di::UserConfig{})
```

### Runtime · "open"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1182)

```cpp
def_static("open",
[] (di::ProviderConfig config) { implementation omitted },
py::arg("provider_config"))
```

### Runtime · "open"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L1178)

```cpp
def_static("open",
[] (di::RuntimeConfig config) { implementation omitted },
py::arg("config"))
```
