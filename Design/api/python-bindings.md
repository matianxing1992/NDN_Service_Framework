# Python 原生绑定映射

从 pybind11 绑定声明静态提取 Python 名称、C++ 目标或 lambda 参数、py::arg 默认值与调用策略。lambda 实现体省略；构造器 py::init 与动态导出须另查源文件。此表不导入扩展，不声明 ABI 或运行通过。

## pythonWrapper/src/ndnsf/_ndnsf.cpp

SHA-256：`4cd84559f3469c85ceb9e4cc5cbbb898390f8dc76989b524c53602d97f893db7`。

### see-source-owner · "decode"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6067)

```cpp
def_static("decode",
[] (const py::bytes& wire) { implementation omitted },
py::arg("wire"))
```

### see-source-owner · "wire_encode"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6064)

```cpp
def("wire_encode",
[] (const T& value) { implementation omitted })
```

### see-source-owner · "digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6063)

```cpp
def("digest",
&T::computeDigest)
```

### see-source-owner · "fields"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6060)

```cpp
def_property_readonly("fields",
[] (const T& value) { implementation omitted })
```

### see-source-owner · "get_field"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6058)

```cpp
def("get_field",
&T::getField,
py::arg("name"),
py::return_value_policy::copy)
```

### see-source-owner · "has_field"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6057)

```cpp
def("has_field",
&T::hasField,
py::arg("name"))
```

### see-source-owner · "set_field"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6056)

```cpp
def("set_field",
&T::setField,
py::arg("name"),
py::arg("value"))
```

### see-source-owner · "version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6055)

```cpp
def_property("version",
&T::getVersion,
&T::setVersion)
```

### m · "make_opaque_control_handle"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6100)

```cpp
def("make_opaque_control_handle",
&nsf::makeOpaqueControlHandle,
py::arg("bytes") = 24)
```

### m · "is_valid_opaque_control_handle"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6102)

```cpp
def("is_valid_opaque_control_handle",
&nsf::isValidOpaqueControlHandle,
py::arg("handle"))
```

### NativeStreamFecInfo · "enabled"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6116)

```cpp
def_property_readonly("enabled",
&nsf::StreamFecInfo::enabled)
```

### NativeStreamFecInfo · "metadata"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6115)

```cpp
def_readwrite("metadata",
&nsf::StreamFecInfo::metadata)
```

### NativeStreamFecInfo · "repair_symbol"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6114)

```cpp
def_readwrite("repair_symbol",
&nsf::StreamFecInfo::repairSymbol)
```

### NativeStreamFecInfo · "source_block_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6113)

```cpp
def_readwrite("source_block_id",
&nsf::StreamFecInfo::sourceBlockId)
```

### NativeStreamFecInfo · "data_lengths"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6112)

```cpp
def_readwrite("data_lengths",
&nsf::StreamFecInfo::dataLengths)
```

### NativeStreamFecInfo · "symbol_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6111)

```cpp
def_readwrite("symbol_count",
&nsf::StreamFecInfo::symbolCount)
```

### NativeStreamFecInfo · "symbol_index"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6110)

```cpp
def_readwrite("symbol_index",
&nsf::StreamFecInfo::symbolIndex)
```

### NativeStreamFecInfo · "parity_shards"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6109)

```cpp
def_readwrite("parity_shards",
&nsf::StreamFecInfo::parityShards)
```

### NativeStreamFecInfo · "data_shards"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6108)

```cpp
def_readwrite("data_shards",
&nsf::StreamFecInfo::dataShards)
```

### NativeStreamFecInfo · "scheme"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6107)

```cpp
def_readwrite("scheme",
&nsf::StreamFecInfo::scheme)
```

### NativeStreamChunk · "metadata"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6143)

```cpp
def_readwrite("metadata",
&nsf::StreamChunk::metadata)
```

### NativeStreamChunk · "fec"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6142)

```cpp
def_readwrite("fec",
&nsf::StreamChunk::fec)
```

### NativeStreamChunk · "segment_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6141)

```cpp
def_readwrite("segment_count",
&nsf::StreamChunk::segmentCount)
```

### NativeStreamChunk · "segment_index"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6140)

```cpp
def_readwrite("segment_index",
&nsf::StreamChunk::segmentIndex)
```

### NativeStreamChunk · "frame_last_seq"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6139)

```cpp
def_readwrite("frame_last_seq",
&nsf::StreamChunk::frameLastSeq)
```

### NativeStreamChunk · "frame_first_seq"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6138)

```cpp
def_readwrite("frame_first_seq",
&nsf::StreamChunk::frameFirstSeq)
```

### NativeStreamChunk · "frame_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6137)

```cpp
def_readwrite("frame_id",
&nsf::StreamChunk::frameId)
```

### NativeStreamChunk · "key_chunk"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6136)

```cpp
def_readwrite("key_chunk",
&nsf::StreamChunk::keyChunk)
```

### NativeStreamChunk · "deadline_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6135)

```cpp
def_readwrite("deadline_ms",
&nsf::StreamChunk::deadlineMs)
```

### NativeStreamChunk · "arrival_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6134)

```cpp
def_readwrite("arrival_ms",
&nsf::StreamChunk::arrivalMs)
```

### NativeStreamChunk · "capture_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6133)

```cpp
def_readwrite("capture_ms",
&nsf::StreamChunk::captureMs)
```

### NativeStreamChunk · "content_type"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6132)

```cpp
def_readwrite("content_type",
&nsf::StreamChunk::contentType)
```

### NativeStreamChunk · "payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6123)

```cpp
def_property("payload",
[] (const nsf::StreamChunk& chunk) { implementation omitted },
[] (nsf::StreamChunk& chunk, const py::bytes& value) { implementation omitted })
```

### NativeStreamChunk · "seq"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6122)

```cpp
def_readwrite("seq",
&nsf::StreamChunk::seq)
```

### NativeStreamChunk · "session_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6121)

```cpp
def_readwrite("session_epoch",
&nsf::StreamChunk::sessionEpoch)
```

### NativeStreamChunk · "stream_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6120)

```cpp
def_readwrite("stream_id",
&nsf::StreamChunk::streamId)
```

### NativeStreamNameMapEntry · "predicted_group_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6180)

```cpp
def_property_readonly("predicted_group_items",
&nsf::StreamNameMapEntry::predictedGroupItems)
```

### NativeStreamNameMapEntry · "has_group_binding"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6178)

```cpp
def_property_readonly("has_group_binding",
&nsf::StreamNameMapEntry::hasGroupBinding)
```

### NativeStreamNameMapEntry · "is_tombstone"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6177)

```cpp
def("is_tombstone",
&nsf::StreamNameMapEntry::isTombstone)
```

### NativeStreamNameMapEntry · "make_tombstone"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6176)

```cpp
def_static("make_tombstone",
&nsf::StreamNameMapEntry::makeTombstone)
```

### NativeStreamNameMapEntry · "from_grouped_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6166)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6162)

```cpp
def_static("from_name",
[] (const std::string& name) { implementation omitted },
py::arg("name"))
```

### NativeStreamNameMapEntry · "predicted_repair_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6160)

```cpp
def_readwrite("predicted_repair_items",
&nsf::StreamNameMapEntry::predictedRepairItems)
```

### NativeStreamNameMapEntry · "predicted_source_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6158)

```cpp
def_readwrite("predicted_source_items",
&nsf::StreamNameMapEntry::predictedSourceItems)
```

### NativeStreamNameMapEntry · "group_item_index"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6157)

```cpp
def_readwrite("group_item_index",
&nsf::StreamNameMapEntry::groupItemIndex)
```

### NativeStreamNameMapEntry · "sample_class"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6156)

```cpp
def_readwrite("sample_class",
&nsf::StreamNameMapEntry::sampleClass)
```

### NativeStreamNameMapEntry · "group_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6155)

```cpp
def_readwrite("group_id",
&nsf::StreamNameMapEntry::groupId)
```

### NativeStreamNameMapEntry · "tombstone"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6154)

```cpp
def_readwrite("tombstone",
&nsf::StreamNameMapEntry::tombstone)
```

### NativeStreamNameMapEntry · "original_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6147)

```cpp
def_property("original_name",
[] (const nsf::StreamNameMapEntry& entry) { implementation omitted },
[] (nsf::StreamNameMapEntry& entry, const std::string& value) { implementation omitted })
```

### NativeStreamNameMapBlock · "last_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6231)

```cpp
def("last_cursor",
&nsf::StreamNameMapBlock::lastCursor)
```

### NativeStreamNameMapBlock · "fits_signed_wire_budget"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6229)

```cpp
def("fits_signed_wire_budget",
&nsf::StreamNameMapBlock::fitsSignedWireBudget,
py::arg("signed_envelope_overhead"),
py::arg("configured_wire_cap"))
```

### NativeStreamNameMapBlock · "content_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6226)

```cpp
def("content_digest",
[] (const nsf::StreamNameMapBlock& block) { implementation omitted })
```

### NativeStreamNameMapBlock · "canonical_content"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6223)

```cpp
def("canonical_content",
[] (const nsf::StreamNameMapBlock& block) { implementation omitted })
```

### NativeStreamNameMapBlock · "decode"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6215)

```cpp
def_static("decode",
[] (const py::bytes& wire) { implementation omitted },
py::arg("wire"))
```

### NativeStreamNameMapBlock · "wire_encode"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6212)

```cpp
def("wire_encode",
[] (const nsf::StreamNameMapBlock& block) { implementation omitted })
```

### NativeStreamNameMapBlock · "validate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6208)

```cpp
def("validate",
[] (const nsf::StreamNameMapBlock& block) -> py::object { implementation omitted })
```

### NativeStreamNameMapBlock · "entries"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6207)

```cpp
def_readwrite("entries",
&nsf::StreamNameMapBlock::entries)
```

### NativeStreamNameMapBlock · "previous_content_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6192)

```cpp
def_property("previous_content_digest",
[] (const nsf::StreamNameMapBlock& block) -> py::object { implementation omitted },
[] (nsf::StreamNameMapBlock& block, const py::object& value) { implementation omitted })
```

### NativeStreamNameMapBlock · "first_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6191)

```cpp
def_readwrite("first_cursor",
&nsf::StreamNameMapBlock::firstCursor)
```

### NativeStreamNameMapBlock · "block_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6190)

```cpp
def_readwrite("block_capacity",
&nsf::StreamNameMapBlock::blockCapacity)
```

### NativeStreamNameMapBlock · "block_number"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6189)

```cpp
def_readwrite("block_number",
&nsf::StreamNameMapBlock::blockNumber)
```

### NativeStreamNameMapBlock · "mapping_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6188)

```cpp
def_readwrite("mapping_version",
&nsf::StreamNameMapBlock::mappingVersion)
```

### NativeStreamNameMapBlock · "session_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6187)

```cpp
def_readwrite("session_epoch",
&nsf::StreamNameMapBlock::sessionEpoch)
```

### NativeStreamNameMapBlock · "stream_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6186)

```cpp
def_readwrite("stream_id",
&nsf::StreamNameMapBlock::streamId)
```

### NativeStreamNameMapBlock · "contract_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6185)

```cpp
def_readwrite("contract_version",
&nsf::StreamNameMapBlock::contractVersion)
```

### m · "make_stream_name_map_root"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6233)

```cpp
def("make_stream_name_map_root",
[] (const std::string& provider, const std::string& streamId) { implementation omitted },
py::arg("provider"),
py::arg("stream_id"))
```

### m · "make_stream_name_map_block_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6238)

```cpp
def("make_stream_name_map_block_name",
[] (const std::string& mappingRoot, uint64_t mappingVersion,
        uint64_t blockNumber) { implementation omitted },
py::arg("mapping_root"),
py::arg("mapping_version"),
py::arg("block_number"))
```

### NativeStreamCursorFrontiers · "validate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6255)

```cpp
def("validate",
[] (const nsf::StreamCursorFrontiers& frontiers,
                         uint64_t blockCapacity,
                         uint64_t checkpointBlock) -> py::object { implementation omitted },
py::arg("block_capacity"),
py::arg("checkpoint_block"))
```

### NativeStreamCursorFrontiers · "next_reserved"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6254)

```cpp
def_readwrite("next_reserved",
&nsf::StreamCursorFrontiers::nextReserved)
```

### NativeStreamCursorFrontiers · "mapping_committed_through"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6252)

```cpp
def_readwrite("mapping_committed_through",
&nsf::StreamCursorFrontiers::mappingCommittedThrough)
```

### NativeStreamCursorFrontiers · "latest_produced"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6251)

```cpp
def_readwrite("latest_produced",
&nsf::StreamCursorFrontiers::latestProduced)
```

### NativeStreamCursorFrontiers · "latest_join"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6250)

```cpp
def_readwrite("latest_join",
&nsf::StreamCursorFrontiers::latestJoin)
```

### NativeStreamCursorFrontiers · "oldest_retained"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6249)

```cpp
def_readwrite("oldest_retained",
&nsf::StreamCursorFrontiers::oldestRetained)
```

### NativeStreamNameMapCheckpoint · "content_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6266)

```cpp
def_property("content_digest",
[] (const nsf::StreamNameMapCheckpoint& checkpoint) { implementation omitted },
[] (nsf::StreamNameMapCheckpoint& checkpoint, const py::bytes& value) { implementation omitted })
```

### NativeStreamNameMapCheckpoint · "block_number"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6265)

```cpp
def_readwrite("block_number",
&nsf::StreamNameMapCheckpoint::blockNumber)
```

### NativeStreamNameMapCheckpoint · "frontiers"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6264)

```cpp
def_readwrite("frontiers",
&nsf::StreamNameMapCheckpoint::frontiers)
```

### NativeStreamNameMapResolverConfig · "max_original_name_wire_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6309)

```cpp
def_readwrite("max_original_name_wire_bytes",
&nsf::StreamNameMapResolverConfig::maxOriginalNameWireBytes)
```

### NativeStreamNameMapResolverConfig · "max_reverse_entries"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6307)

```cpp
def_readwrite("max_reverse_entries",
&nsf::StreamNameMapResolverConfig::maxReverseEntries)
```

### NativeStreamNameMapResolverConfig · "max_quarantine_blocks"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6305)

```cpp
def_readwrite("max_quarantine_blocks",
&nsf::StreamNameMapResolverConfig::maxQuarantineBlocks)
```

### NativeStreamNameMapResolverConfig · "max_verified_blocks"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6303)

```cpp
def_readwrite("max_verified_blocks",
&nsf::StreamNameMapResolverConfig::maxVerifiedBlocks)
```

### NativeStreamNameMapResolverConfig · "signed_wire_cap"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6302)

```cpp
def_readwrite("signed_wire_cap",
&nsf::StreamNameMapResolverConfig::signedWireCap)
```

### NativeStreamNameMapResolverConfig · "payload_prefix"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6295)

```cpp
def_property("payload_prefix",
[] (const nsf::StreamNameMapResolverConfig& config) { implementation omitted },
[] (nsf::StreamNameMapResolverConfig& config, const std::string& value) { implementation omitted })
```

### NativeStreamNameMapResolverConfig · "mapping_root"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6288)

```cpp
def_property("mapping_root",
[] (const nsf::StreamNameMapResolverConfig& config) { implementation omitted },
[] (nsf::StreamNameMapResolverConfig& config, const std::string& value) { implementation omitted })
```

### NativeStreamNameMapResolverConfig · "expected_provider"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6281)

```cpp
def_property("expected_provider",
[] (const nsf::StreamNameMapResolverConfig& config) { implementation omitted },
[] (nsf::StreamNameMapResolverConfig& config, const std::string& value) { implementation omitted })
```

### NativeStreamNameMapResolverConfig · "block_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6280)

```cpp
def_readwrite("block_capacity",
&nsf::StreamNameMapResolverConfig::blockCapacity)
```

### NativeStreamNameMapResolverConfig · "mapping_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6279)

```cpp
def_readwrite("mapping_version",
&nsf::StreamNameMapResolverConfig::mappingVersion)
```

### NativeStreamNameMapResolverConfig · "session_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6278)

```cpp
def_readwrite("session_epoch",
&nsf::StreamNameMapResolverConfig::sessionEpoch)
```

### NativeStreamNameMapResolverConfig · "stream_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6277)

```cpp
def_readwrite("stream_id",
&nsf::StreamNameMapResolverConfig::streamId)
```

### NativeStreamNameMapResolverConfig · "contract_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6276)

```cpp
def_readwrite("contract_version",
&nsf::StreamNameMapResolverConfig::contractVersion)
```

### NativeVerifiedStreamNameMapData · "required_before_monotonic_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6340)

```cpp
def_readwrite("required_before_monotonic_ms",
&nsf::VerifiedStreamNameMapData::requiredBeforeMonotonicMs)
```

### NativeVerifiedStreamNameMapData · "received_monotonic_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6338)

```cpp
def_readwrite("received_monotonic_ms",
&nsf::VerifiedStreamNameMapData::receivedMonotonicMs)
```

### NativeVerifiedStreamNameMapData · "content"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6331)

```cpp
def_property("content",
[] (const nsf::VerifiedStreamNameMapData& input) { implementation omitted },
[] (nsf::VerifiedStreamNameMapData& input, const py::bytes& value) { implementation omitted })
```

### NativeVerifiedStreamNameMapData · "signed_wire_size"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6330)

```cpp
def_readwrite("signed_wire_size",
&nsf::VerifiedStreamNameMapData::signedWireSize)
```

### NativeVerifiedStreamNameMapData · "has_final_block"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6329)

```cpp
def_readwrite("has_final_block",
&nsf::VerifiedStreamNameMapData::hasFinalBlock)
```

### NativeVerifiedStreamNameMapData · "content_type"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6328)

```cpp
def_readwrite("content_type",
&nsf::VerifiedStreamNameMapData::contentType)
```

### NativeVerifiedStreamNameMapData · "verified_provider"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6321)

```cpp
def_property("verified_provider",
[] (const nsf::VerifiedStreamNameMapData& input) { implementation omitted },
[] (nsf::VerifiedStreamNameMapData& input, const std::string& value) { implementation omitted })
```

### NativeVerifiedStreamNameMapData · "data_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6314)

```cpp
def_property("data_name",
[] (const nsf::VerifiedStreamNameMapData& input) { implementation omitted },
[] (nsf::VerifiedStreamNameMapData& input, const std::string& value) { implementation omitted })
```

### NativeStreamNameMapAdmissionDisposition · "FATAL_SESSION"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6349)

```cpp
value("FATAL_SESSION",
nsf::StreamNameMapAdmissionDisposition::FatalSession)
```

### NativeStreamNameMapAdmissionDisposition · "REJECTED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6348)

```cpp
value("REJECTED",
nsf::StreamNameMapAdmissionDisposition::Rejected)
```

### NativeStreamNameMapAdmissionDisposition · "QUARANTINED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6347)

```cpp
value("QUARANTINED",
nsf::StreamNameMapAdmissionDisposition::Quarantined)
```

### NativeStreamNameMapAdmissionDisposition · "DUPLICATE"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6346)

```cpp
value("DUPLICATE",
nsf::StreamNameMapAdmissionDisposition::Duplicate)
```

### NativeStreamNameMapAdmissionDisposition · "ADMITTED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6345)

```cpp
value("ADMITTED",
nsf::StreamNameMapAdmissionDisposition::Admitted)
```

### NativeStreamNameMapTiming · "LATE"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6354)

```cpp
value("LATE",
nsf::StreamNameMapTiming::Late)
```

### NativeStreamNameMapTiming · "AHEAD"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6353)

```cpp
value("AHEAD",
nsf::StreamNameMapTiming::Ahead)
```

### NativeStreamNameMapTiming · "UNCLASSIFIED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6352)

```cpp
value("UNCLASSIFIED",
nsf::StreamNameMapTiming::Unclassified)
```

### NativeStreamNameMapAdmissionResult · "fatal"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6371)

```cpp
def_property_readonly("fatal",
&nsf::StreamNameMapAdmissionResult::fatal)
```

### NativeStreamNameMapAdmissionResult · "accepted"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6370)

```cpp
def_property_readonly("accepted",
&nsf::StreamNameMapAdmissionResult::accepted)
```

### NativeStreamNameMapAdmissionResult · "mapping_committed_through"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6368)

```cpp
def_readonly("mapping_committed_through",
&nsf::StreamNameMapAdmissionResult::mappingCommittedThrough)
```

### NativeStreamNameMapAdmissionResult · "state_changed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6367)

```cpp
def_readonly("state_changed",
&nsf::StreamNameMapAdmissionResult::stateChanged)
```

### NativeStreamNameMapAdmissionResult · "reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6366)

```cpp
def_readonly("reason",
&nsf::StreamNameMapAdmissionResult::reason)
```

### NativeStreamNameMapAdmissionResult · "timing_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6362)

```cpp
def_property_readonly("timing_name",
[] (const nsf::StreamNameMapAdmissionResult& result) { implementation omitted })
```

### NativeStreamNameMapAdmissionResult · "disposition_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6358)

```cpp
def_property_readonly("disposition_name",
[] (const nsf::StreamNameMapAdmissionResult& result) { implementation omitted })
```

### NativeStreamNameMapResolution · "predicted_group_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6397)

```cpp
def_property_readonly("predicted_group_items",
&nsf::StreamNameMapResolution::predictedGroupItems)
```

### NativeStreamNameMapResolution · "has_group_binding"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6395)

```cpp
def_property_readonly("has_group_binding",
&nsf::StreamNameMapResolution::hasGroupBinding)
```

### NativeStreamNameMapResolution · "schedulable"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6394)

```cpp
def_property_readonly("schedulable",
&nsf::StreamNameMapResolution::schedulable)
```

### NativeStreamNameMapResolution · "timing_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6390)

```cpp
def_property_readonly("timing_name",
[] (const nsf::StreamNameMapResolution& resolution) { implementation omitted })
```

### NativeStreamNameMapResolution · "predicted_repair_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6388)

```cpp
def_readonly("predicted_repair_items",
&nsf::StreamNameMapResolution::predictedRepairItems)
```

### NativeStreamNameMapResolution · "predicted_source_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6386)

```cpp
def_readonly("predicted_source_items",
&nsf::StreamNameMapResolution::predictedSourceItems)
```

### NativeStreamNameMapResolution · "group_item_index"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6385)

```cpp
def_readonly("group_item_index",
&nsf::StreamNameMapResolution::groupItemIndex)
```

### NativeStreamNameMapResolution · "sample_class"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6384)

```cpp
def_readonly("sample_class",
&nsf::StreamNameMapResolution::sampleClass)
```

### NativeStreamNameMapResolution · "group_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6383)

```cpp
def_readonly("group_id",
&nsf::StreamNameMapResolution::groupId)
```

### NativeStreamNameMapResolution · "terminal_unproduced"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6381)

```cpp
def_readonly("terminal_unproduced",
&nsf::StreamNameMapResolution::terminalUnproduced)
```

### NativeStreamNameMapResolution · "tombstone"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6380)

```cpp
def_readonly("tombstone",
&nsf::StreamNameMapResolution::tombstone)
```

### NativeStreamNameMapResolution · "original_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6375)

```cpp
def_property_readonly("original_name",
[] (const nsf::StreamNameMapResolution& resolution) { implementation omitted })
```

### NativeStreamNameMapResolution · "cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6374)

```cpp
def_readonly("cursor",
&nsf::StreamNameMapResolution::cursor)
```

### NativeStreamNameResolverState · "diagnostics"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6446)

```cpp
def("diagnostics",
&nsf::StreamNameResolverState::diagnostics)
```

### NativeStreamNameResolverState · "binding_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6445)

```cpp
def("binding_count",
&nsf::StreamNameResolverState::bindingCount)
```

### NativeStreamNameResolverState · "quarantined_block_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6443)

```cpp
def("quarantined_block_count",
&nsf::StreamNameResolverState::quarantinedBlockCount)
```

### NativeStreamNameResolverState · "verified_block_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6442)

```cpp
def("verified_block_count",
&nsf::StreamNameResolverState::verifiedBlockCount)
```

### NativeStreamNameResolverState · "faulted"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6441)

```cpp
def("faulted",
&nsf::StreamNameResolverState::faulted)
```

### NativeStreamNameResolverState · "checkpoint"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6440)

```cpp
def("checkpoint",
&nsf::StreamNameResolverState::checkpoint)
```

### NativeStreamNameResolverState · "frontiers"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6439)

```cpp
def("frontiers",
&nsf::StreamNameResolverState::frontiers)
```

### NativeStreamNameResolverState · "evict_local_block"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6437)

```cpp
def("evict_local_block",
&nsf::StreamNameResolverState::evictLocalBlock,
py::arg("block_number"))
```

### NativeStreamNameResolverState · "mark_terminal_unproduced"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6434)

```cpp
def("mark_terminal_unproduced",
&nsf::StreamNameResolverState::markTerminalUnproduced,
py::arg("cursor"))
```

### NativeStreamNameResolverState · "reverse_resolve"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6429)

```cpp
def("reverse_resolve",
[] (const nsf::StreamNameResolverState& resolver,
                                const std::string& originalName) -> py::object { implementation omitted },
py::arg("original_name"))
```

### NativeStreamNameResolverState · "resolve"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6424)

```cpp
def("resolve",
[] (const nsf::StreamNameResolverState& resolver,
                        nsf::StreamCursor cursor) -> py::object { implementation omitted },
py::arg("cursor"))
```

### NativeStreamNameResolverState · "lookup"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6422)

```cpp
def("lookup",
&nsf::StreamNameResolverState::lookup,
py::arg("cursor"))
```

### NativeStreamNameResolverState · "refresh_checkpoint"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6420)

```cpp
def("refresh_checkpoint",
&nsf::StreamNameResolverState::refreshCheckpoint,
py::arg("checkpoint"))
```

### NativeStreamNameResolverState · "admit_verified_wire"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6406)

```cpp
def("admit_verified_wire",
[] (nsf::StreamNameResolverState& resolver,
          nsf::VerifiedStreamNameMapData input,
          const py::bytes& contentWire) { implementation omitted },
py::arg("input"),
py::arg("content_wire"))
```

### NativeStreamNameResolverState · "admit_verified_block"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6404)

```cpp
def("admit_verified_block",
&nsf::StreamNameResolverState::admitVerifiedBlock,
py::arg("input"))
```

### NativeStreamNameResolverState · "reset"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6402)

```cpp
def("reset",
&nsf::StreamNameResolverState::reset,
py::arg("config"),
py::arg("checkpoint"))
```

### NativeStreamMetrics · "bytes_received"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6462)

```cpp
def_readwrite("bytes_received",
&nsf::StreamMetrics::bytesReceived)
```

### NativeStreamMetrics · "bytes_produced"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6461)

```cpp
def_readwrite("bytes_produced",
&nsf::StreamMetrics::bytesProduced)
```

### NativeStreamMetrics · "max_pending"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6460)

```cpp
def_readwrite("max_pending",
&nsf::StreamMetrics::maxPending)
```

### NativeStreamMetrics · "overflows"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6459)

```cpp
def_readwrite("overflows",
&nsf::StreamMetrics::overflows)
```

### NativeStreamMetrics · "nacks"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6458)

```cpp
def_readwrite("nacks",
&nsf::StreamMetrics::nacks)
```

### NativeStreamMetrics · "timeouts"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6457)

```cpp
def_readwrite("timeouts",
&nsf::StreamMetrics::timeouts)
```

### NativeStreamMetrics · "gaps"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6456)

```cpp
def_readwrite("gaps",
&nsf::StreamMetrics::gaps)
```

### NativeStreamMetrics · "stale"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6455)

```cpp
def_readwrite("stale",
&nsf::StreamMetrics::stale)
```

### NativeStreamMetrics · "duplicates"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6454)

```cpp
def_readwrite("duplicates",
&nsf::StreamMetrics::duplicates)
```

### NativeStreamMetrics · "emitted"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6453)

```cpp
def_readwrite("emitted",
&nsf::StreamMetrics::emitted)
```

### NativeStreamMetrics · "received"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6452)

```cpp
def_readwrite("received",
&nsf::StreamMetrics::received)
```

### NativeStreamMetrics · "evicted"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6451)

```cpp
def_readwrite("evicted",
&nsf::StreamMetrics::evicted)
```

### NativeStreamMetrics · "produced"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6450)

```cpp
def_readwrite("produced",
&nsf::StreamMetrics::produced)
```

### NativeStreamProducerBuffer · "metrics"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6470)

```cpp
def_property_readonly("metrics",
&nsf::StreamProducerBuffer::metrics)
```

### NativeStreamProducerBuffer · "size"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6469)

```cpp
def("size",
&nsf::StreamProducerBuffer::size)
```

### NativeStreamProducerBuffer · "sequences"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6468)

```cpp
def("sequences",
&nsf::StreamProducerBuffer::sequences)
```

### NativeStreamProducerBuffer · "get"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6467)

```cpp
def("get",
&nsf::StreamProducerBuffer::get)
```

### NativeStreamProducerBuffer · "put"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6466)

```cpp
def("put",
&nsf::StreamProducerBuffer::put)
```

### NativeStreamConsumerReorderBuffer · "metrics"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6489)

```cpp
def_property_readonly("metrics",
&nsf::StreamConsumerReorderBuffer::metrics)
```

### NativeStreamConsumerReorderBuffer · "pending_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6488)

```cpp
def_property_readonly("pending_bytes",
&nsf::StreamConsumerReorderBuffer::pendingBytes)
```

### NativeStreamConsumerReorderBuffer · "pending_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6487)

```cpp
def_property_readonly("pending_count",
&nsf::StreamConsumerReorderBuffer::pendingCount)
```

### NativeStreamConsumerReorderBuffer · "next_seq"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6486)

```cpp
def_property_readonly("next_seq",
&nsf::StreamConsumerReorderBuffer::nextSeq)
```

### NativeStreamConsumerReorderBuffer · "skip_to"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6485)

```cpp
def("skip_to",
&nsf::StreamConsumerReorderBuffer::skipTo)
```

### NativeStreamConsumerReorderBuffer · "drain_ready"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6484)

```cpp
def("drain_ready",
&nsf::StreamConsumerReorderBuffer::drainReady)
```

### NativeStreamConsumerReorderBuffer · "pending_sequences"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6482)

```cpp
def("pending_sequences",
&nsf::StreamConsumerReorderBuffer::pendingSequences,
py::arg("limit") = 0)
```

### NativeStreamConsumerReorderBuffer · "missing_sequences"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6480)

```cpp
def("missing_sequences",
&nsf::StreamConsumerReorderBuffer::missingSequences,
py::arg("limit") = 32)
```

### NativeStreamConsumerReorderBuffer · "push"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6479)

```cpp
def("push",
&nsf::StreamConsumerReorderBuffer::push)
```

### NativeStreamConsumerReorderBuffer · "reset"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6477)

```cpp
def("reset",
&nsf::StreamConsumerReorderBuffer::reset,
py::arg("stream_id"),
py::arg("session_epoch"),
py::arg("next_seq") = 0)
```

### NativeStreamPrefetchPhase · "STOPPED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6497)

```cpp
value("STOPPED",
nsf::StreamPrefetchPhase::Stopped)
```

### NativeStreamPrefetchPhase · "RECOVERING"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6496)

```cpp
value("RECOVERING",
nsf::StreamPrefetchPhase::Recovering)
```

### NativeStreamPrefetchPhase · "FETCHING"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6495)

```cpp
value("FETCHING",
nsf::StreamPrefetchPhase::Fetching)
```

### NativeStreamPrefetchPhase · "ADJUSTING"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6494)

```cpp
value("ADJUSTING",
nsf::StreamPrefetchPhase::Adjusting)
```

### NativeStreamPrefetchPhase · "CHASING"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6493)

```cpp
value("CHASING",
nsf::StreamPrefetchPhase::Chasing)
```

### NativeStreamPrefetchPhase · "INACTIVE"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6492)

```cpp
value("INACTIVE",
nsf::StreamPrefetchPhase::Inactive)
```

### NativeStreamFetchDecision · "reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6537)

```cpp
def_readwrite("reason",
&nsf::StreamFetchDecision::reason)
```

### NativeStreamFetchDecision · "capacity_reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6536)

```cpp
def_readwrite("capacity_reason",
&nsf::StreamFetchDecision::capacityReason)
```

### NativeStreamFetchDecision · "mapping_wait_reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6535)

```cpp
def_readwrite("mapping_wait_reason",
&nsf::StreamFetchDecision::mappingWaitReason)
```

### NativeStreamFetchDecision · "detector_profile"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6534)

```cpp
def_readwrite("detector_profile",
&nsf::StreamFetchDecision::detectorProfile)
```

### NativeStreamFetchDecision · "policy_mode"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6533)

```cpp
def_readwrite("policy_mode",
&nsf::StreamFetchDecision::policyMode)
```

### NativeStreamFetchDecision · "phase_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6530)

```cpp
def_property_readonly("phase_name",
[] (const nsf::StreamFetchDecision& value) { implementation omitted })
```

### NativeStreamFetchDecision · "phase"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6529)

```cpp
def_readwrite("phase",
&nsf::StreamFetchDecision::phase)
```

### NativeStreamFetchDecision · "retransmission_eligible"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6528)

```cpp
def_readwrite("retransmission_eligible",
&nsf::StreamFetchDecision::retransmissionEligible)
```

### NativeStreamFetchDecision · "congestion_hold"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6527)

```cpp
def_readwrite("congestion_hold",
&nsf::StreamFetchDecision::congestionHold)
```

### NativeStreamFetchDecision · "future_wait"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6526)

```cpp
def_readwrite("future_wait",
&nsf::StreamFetchDecision::futureWait)
```

### NativeStreamFetchDecision · "mapping_ready"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6525)

```cpp
def_readwrite("mapping_ready",
&nsf::StreamFetchDecision::mappingReady)
```

### NativeStreamFetchDecision · "live_edge_confidence"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6524)

```cpp
def_readwrite("live_edge_confidence",
&nsf::StreamFetchDecision::liveEdgeConfidence)
```

### NativeStreamFetchDecision · "pressure"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6523)

```cpp
def_readwrite("pressure",
&nsf::StreamFetchDecision::pressure)
```

### NativeStreamFetchDecision · "atomic_deferrals"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6522)

```cpp
def_readwrite("atomic_deferrals",
&nsf::StreamFetchDecision::atomicDeferrals)
```

### NativeStreamFetchDecision · "atomic_expansions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6521)

```cpp
def_readwrite("atomic_expansions",
&nsf::StreamFetchDecision::atomicExpansions)
```

### NativeStreamFetchDecision · "later_cursor_advice"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6520)

```cpp
def_readwrite("later_cursor_advice",
&nsf::StreamFetchDecision::laterCursorAdvice)
```

### NativeStreamFetchDecision · "terminal_unproduced_advice"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6519)

```cpp
def_readwrite("terminal_unproduced_advice",
&nsf::StreamFetchDecision::terminalUnproducedAdvice)
```

### NativeStreamFetchDecision · "future_wait_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6518)

```cpp
def_readwrite("future_wait_count",
&nsf::StreamFetchDecision::futureWaitCount)
```

### NativeStreamFetchDecision · "retransmission_budget"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6517)

```cpp
def_readwrite("retransmission_budget",
&nsf::StreamFetchDecision::retransmissionBudget)
```

### NativeStreamFetchDecision · "payload_budget"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6516)

```cpp
def_readwrite("payload_budget",
&nsf::StreamFetchDecision::payloadBudget)
```

### NativeStreamFetchDecision · "mapping_budget"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6515)

```cpp
def_readwrite("mapping_budget",
&nsf::StreamFetchDecision::mappingBudget)
```

### NativeStreamFetchDecision · "aggregate_in_flight_limit"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6514)

```cpp
def_readwrite("aggregate_in_flight_limit",
&nsf::StreamFetchDecision::aggregateInFlightLimit)
```

### NativeStreamFetchDecision · "payload_end_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6513)

```cpp
def_readwrite("payload_end_cursor",
&nsf::StreamFetchDecision::payloadEndCursor)
```

### NativeStreamFetchDecision · "payload_begin_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6512)

```cpp
def_readwrite("payload_begin_cursor",
&nsf::StreamFetchDecision::payloadBeginCursor)
```

### NativeStreamFetchDecision · "mapping_end_block"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6511)

```cpp
def_readwrite("mapping_end_block",
&nsf::StreamFetchDecision::mappingEndBlock)
```

### NativeStreamFetchDecision · "mapping_begin_block"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6510)

```cpp
def_readwrite("mapping_begin_block",
&nsf::StreamFetchDecision::mappingBeginBlock)
```

### NativeStreamFetchDecision · "remaining_recovery_budget_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6509)

```cpp
def_readwrite("remaining_recovery_budget_ms",
&nsf::StreamFetchDecision::remainingRecoveryBudgetMs)
```

### NativeStreamFetchDecision · "recovery_checkpoint_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6508)

```cpp
def_readwrite("recovery_checkpoint_ms",
&nsf::StreamFetchDecision::recoveryCheckpointMs)
```

### NativeStreamFetchDecision · "hold_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6507)

```cpp
def_readwrite("hold_ms",
&nsf::StreamFetchDecision::holdMs)
```

### NativeStreamFetchDecision · "packet_demand"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6506)

```cpp
def_readwrite("packet_demand",
&nsf::StreamFetchDecision::packetDemand)
```

### NativeStreamFetchDecision · "sample_demand"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6505)

```cpp
def_readwrite("sample_demand",
&nsf::StreamFetchDecision::sampleDemand)
```

### NativeStreamFetchDecision · "missing_timeout_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6504)

```cpp
def_readwrite("missing_timeout_ms",
&nsf::StreamFetchDecision::missingTimeoutMs)
```

### NativeStreamFetchDecision · "interest_lifetime_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6503)

```cpp
def_readwrite("interest_lifetime_ms",
&nsf::StreamFetchDecision::interestLifetimeMs)
```

### NativeStreamFetchDecision · "lookahead"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6502)

```cpp
def_readwrite("lookahead",
&nsf::StreamFetchDecision::lookahead)
```

### NativeStreamFetchDecision · "window"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6501)

```cpp
def_readwrite("window",
&nsf::StreamFetchDecision::window)
```

### NativeStreamAdaptiveFetcherState · "decide"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6625)

```cpp
def("decide",
&nsf::StreamAdaptiveFetcherState::decide,
py::arg("now_ms") = 0,
py::arg("playout_deadline_ms") = 0)
```

### NativeStreamAdaptiveFetcherState · "invalid_observations"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6624)

```cpp
def_property_readonly("invalid_observations",
&nsf::StreamAdaptiveFetcherState::invalidObservations)
```

### NativeStreamAdaptiveFetcherState · "phase_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6621)

```cpp
def_property_readonly("phase_name",
[] (const nsf::StreamAdaptiveFetcherState& value) { implementation omitted })
```

### NativeStreamAdaptiveFetcherState · "stop_live"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6620)

```cpp
def("stop_live",
&nsf::StreamAdaptiveFetcherState::stopLive)
```

### NativeStreamAdaptiveFetcherState · "record_invalid_observation"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6619)

```cpp
def("record_invalid_observation",
&nsf::StreamAdaptiveFetcherState::recordInvalidObservation)
```

### NativeStreamAdaptiveFetcherState · "record_recovery"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6617)

```cpp
def("record_recovery",
&nsf::StreamAdaptiveFetcherState::recordRecovery,
py::arg("completed"))
```

### NativeStreamAdaptiveFetcherState · "begin_recovery"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6615)

```cpp
def("begin_recovery",
&nsf::StreamAdaptiveFetcherState::beginRecovery,
py::arg("now_ms"),
py::arg("playout_deadline_ms"))
```

### NativeStreamAdaptiveFetcherState · "observe_sample_extent"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6613)

```cpp
def("observe_sample_extent",
&nsf::StreamAdaptiveFetcherState::observeSampleExtent,
py::arg("predicted_count"),
py::arg("actual_count"))
```

### NativeStreamAdaptiveFetcherState · "observe_accepted_sample"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6609)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6607)

```cpp
def("set_in_flight",
&nsf::StreamAdaptiveFetcherState::setInFlight,
py::arg("mapping"),
py::arg("payload"),
py::arg("retransmission"))
```

### NativeStreamAdaptiveFetcherState · "set_mapped_live_policy_enabled"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6604)

```cpp
def("set_mapped_live_policy_enabled",
&nsf::StreamAdaptiveFetcherState::setMappedLivePolicyEnabled,
py::arg("enabled"))
```

### NativeStreamAdaptiveFetcherState · "advance_next_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6602)

```cpp
def("advance_next_cursor",
&nsf::StreamAdaptiveFetcherState::advanceNextCursor,
py::arg("next_cursor"))
```

### NativeStreamAdaptiveFetcherState · "update_mapping_frontier"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6599)

```cpp
def("update_mapping_frontier",
&nsf::StreamAdaptiveFetcherState::updateMappingFrontier,
py::arg("mapping_committed_through_cursor"),
py::arg("next_reserved_cursor"))
```

### NativeStreamAdaptiveFetcherState · "reset_mapped_live"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6594)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6590)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6587)

```cpp
def("reset_live",
&nsf::StreamAdaptiveFetcherState::resetLive,
py::arg("session_epoch"),
py::arg("next_seq"),
py::arg("sample_period_ms"),
py::arg("now_ms") = 0)
```

### NativeStreamAdaptiveFetcherState · "decay"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6586)

```cpp
def("decay",
&nsf::StreamAdaptiveFetcherState::decay,
py::arg("factor") = 0.85)
```

### NativeStreamAdaptiveFetcherState · "set_backlog_pressure"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6585)

```cpp
def("set_backlog_pressure",
&nsf::StreamAdaptiveFetcherState::setBacklogPressure)
```

### NativeStreamAdaptiveFetcherState · "record_duplicate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6584)

```cpp
def("record_duplicate",
&nsf::StreamAdaptiveFetcherState::recordDuplicate)
```

### NativeStreamAdaptiveFetcherState · "record_congestion_mark"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6582)

```cpp
def("record_congestion_mark",
&nsf::StreamAdaptiveFetcherState::recordCongestionMark,
py::arg("cursor"),
py::arg("mark"))
```

### NativeStreamAdaptiveFetcherState · "record_nack_reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6579)

```cpp
def("record_nack_reason",
py::overload_cast<uint64_t, const std::string&>(
           &nsf::StreamAdaptiveFetcherState::recordNack),
py::arg("cursor"),
py::arg("reason"))
```

### NativeStreamAdaptiveFetcherState · "record_nack"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6577)

```cpp
def("record_nack",
py::overload_cast<>(
           &nsf::StreamAdaptiveFetcherState::recordNack))
```

### NativeStreamAdaptiveFetcherState · "record_timeout_evidence"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6574)

```cpp
def("record_timeout_evidence",
py::overload_cast<uint64_t, bool, bool>(
           &nsf::StreamAdaptiveFetcherState::recordTimeout),
py::arg("cursor"),
py::arg("known_produced"),
py::arg("was_future"))
```

### NativeStreamAdaptiveFetcherState · "record_timeout"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6572)

```cpp
def("record_timeout",
py::overload_cast<>(
           &nsf::StreamAdaptiveFetcherState::recordTimeout))
```

### NativeStreamAdaptiveFetcherState · "observe_rtt"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6570)

```cpp
def("observe_rtt",
&nsf::StreamAdaptiveFetcherState::observeRtt,
py::arg("sample_ms"),
py::arg("alpha") = 0.25)
```

### NativeStreamAdaptiveFetcherState · "detector_profile"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6569)

```cpp
def_readwrite("detector_profile",
&nsf::StreamAdaptiveFetcherState::detectorProfile)
```

### NativeStreamAdaptiveFetcherState · "congestion_decrease_multiplier"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6568)

```cpp
def_readwrite("congestion_decrease_multiplier",
&nsf::StreamAdaptiveFetcherState::congestionDecreaseMultiplier)
```

### NativeStreamAdaptiveFetcherState · "adjust_multiplier"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6567)

```cpp
def_readwrite("adjust_multiplier",
&nsf::StreamAdaptiveFetcherState::adjustMultiplier)
```

### NativeStreamAdaptiveFetcherState · "chase_multiplier"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6566)

```cpp
def_readwrite("chase_multiplier",
&nsf::StreamAdaptiveFetcherState::chaseMultiplier)
```

### NativeStreamAdaptiveFetcherState · "mapping_block_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6565)

```cpp
def_readwrite("mapping_block_capacity",
&nsf::StreamAdaptiveFetcherState::mappingBlockCapacity)
```

### NativeStreamAdaptiveFetcherState · "retransmission_reserve"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6564)

```cpp
def_readwrite("retransmission_reserve",
&nsf::StreamAdaptiveFetcherState::retransmissionReserve)
```

### NativeStreamAdaptiveFetcherState · "mapping_reserve"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6563)

```cpp
def_readwrite("mapping_reserve",
&nsf::StreamAdaptiveFetcherState::mappingReserve)
```

### NativeStreamAdaptiveFetcherState · "aggregate_in_flight_limit"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6562)

```cpp
def_readwrite("aggregate_in_flight_limit",
&nsf::StreamAdaptiveFetcherState::aggregateInFlightLimit)
```

### NativeStreamAdaptiveFetcherState · "recovery_reserve_packets"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6561)

```cpp
def_readwrite("recovery_reserve_packets",
&nsf::StreamAdaptiveFetcherState::recoveryReservePackets)
```

### NativeStreamAdaptiveFetcherState · "detection_period_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6560)

```cpp
def_readwrite("detection_period_ms",
&nsf::StreamAdaptiveFetcherState::detectionPeriodMs)
```

### NativeStreamAdaptiveFetcherState · "live_edge_stable_required"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6559)

```cpp
def_readwrite("live_edge_stable_required",
&nsf::StreamAdaptiveFetcherState::liveEdgeStableRequired)
```

### NativeStreamAdaptiveFetcherState · "live_edge_window"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6558)

```cpp
def_readwrite("live_edge_window",
&nsf::StreamAdaptiveFetcherState::liveEdgeWindow)
```

### NativeStreamAdaptiveFetcherState · "live_edge_period_similarity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6557)

```cpp
def_readwrite("live_edge_period_similarity",
&nsf::StreamAdaptiveFetcherState::liveEdgePeriodSimilarity)
```

### NativeStreamAdaptiveFetcherState · "live_edge_change_threshold"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6556)

```cpp
def_readwrite("live_edge_change_threshold",
&nsf::StreamAdaptiveFetcherState::liveEdgeChangeThreshold)
```

### NativeStreamAdaptiveFetcherState · "max_missing_timeout_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6555)

```cpp
def_readwrite("max_missing_timeout_ms",
&nsf::StreamAdaptiveFetcherState::maxMissingTimeoutMs)
```

### NativeStreamAdaptiveFetcherState · "min_missing_timeout_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6554)

```cpp
def_readwrite("min_missing_timeout_ms",
&nsf::StreamAdaptiveFetcherState::minMissingTimeoutMs)
```

### NativeStreamAdaptiveFetcherState · "max_interest_lifetime_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6553)

```cpp
def_readwrite("max_interest_lifetime_ms",
&nsf::StreamAdaptiveFetcherState::maxInterestLifetimeMs)
```

### NativeStreamAdaptiveFetcherState · "min_interest_lifetime_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6552)

```cpp
def_readwrite("min_interest_lifetime_ms",
&nsf::StreamAdaptiveFetcherState::minInterestLifetimeMs)
```

### NativeStreamAdaptiveFetcherState · "max_lookahead"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6551)

```cpp
def_readwrite("max_lookahead",
&nsf::StreamAdaptiveFetcherState::maxLookahead)
```

### NativeStreamAdaptiveFetcherState · "base_lookahead"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6550)

```cpp
def_readwrite("base_lookahead",
&nsf::StreamAdaptiveFetcherState::baseLookahead)
```

### NativeStreamAdaptiveFetcherState · "min_lookahead"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6549)

```cpp
def_readwrite("min_lookahead",
&nsf::StreamAdaptiveFetcherState::minLookahead)
```

### NativeStreamAdaptiveFetcherState · "max_window"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6548)

```cpp
def_readwrite("max_window",
&nsf::StreamAdaptiveFetcherState::maxWindow)
```

### NativeStreamAdaptiveFetcherState · "base_window"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6547)

```cpp
def_readwrite("base_window",
&nsf::StreamAdaptiveFetcherState::baseWindow)
```

### NativeStreamAdaptiveFetcherState · "min_window"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6546)

```cpp
def_readwrite("min_window",
&nsf::StreamAdaptiveFetcherState::minWindow)
```

### NativeStreamAdaptiveFetcherState · "backlog_pressure"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6545)

```cpp
def_readwrite("backlog_pressure",
&nsf::StreamAdaptiveFetcherState::backlogPressure)
```

### NativeStreamAdaptiveFetcherState · "duplicate_pressure"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6544)

```cpp
def_readwrite("duplicate_pressure",
&nsf::StreamAdaptiveFetcherState::duplicatePressure)
```

### NativeStreamAdaptiveFetcherState · "nack_pressure"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6543)

```cpp
def_readwrite("nack_pressure",
&nsf::StreamAdaptiveFetcherState::nackPressure)
```

### NativeStreamAdaptiveFetcherState · "timeout_pressure"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6542)

```cpp
def_readwrite("timeout_pressure",
&nsf::StreamAdaptiveFetcherState::timeoutPressure)
```

### NativeStreamAdaptiveFetcherState · "rtt_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6541)

```cpp
def_readwrite("rtt_ms",
&nsf::StreamAdaptiveFetcherState::rttMs)
```

### NativeLiveStreamFecScheme · "GF256_TWO_REPAIR"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6631)

```cpp
value("GF256_TWO_REPAIR",
nsf::LiveStreamFecScheme::Gf256TwoRepair)
```

### NativeLiveStreamFecScheme · "XOR_ONE_REPAIR"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6630)

```cpp
value("XOR_ONE_REPAIR",
nsf::LiveStreamFecScheme::XorOneRepair)
```

### NativeLiveStreamFecScheme · "NONE"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6629)

```cpp
value("NONE",
nsf::LiveStreamFecScheme::None)
```

### NativeSampleClassProfile · "validate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6647)

```cpp
def("validate",
[] (const nsf::SampleClassProfile& value) -> py::object { implementation omitted })
```

### NativeSampleClassProfile · "bounded"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6642)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6640)

```cpp
def_readwrite("safety_margin_items",
&nsf::SampleClassProfile::safetyMarginItems)
```

### NativeSampleClassProfile · "history_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6639)

```cpp
def_readwrite("history_capacity",
&nsf::SampleClassProfile::historyCapacity)
```

### NativeSampleClassProfile · "hard_max_source_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6637)

```cpp
def_readwrite("hard_max_source_items",
&nsf::SampleClassProfile::hardMaxSourceItems)
```

### NativeSampleClassProfile · "seed_source_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6636)

```cpp
def_readwrite("seed_source_items",
&nsf::SampleClassProfile::seedSourceItems)
```

### NativeSampleClassProfile · "class_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6635)

```cpp
def_readwrite("class_id",
&nsf::SampleClassProfile::classId)
```

### NativeSampleClassPredictionStatus · "overpredicted_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6663)

```cpp
def_readonly("overpredicted_items",
&nsf::SampleClassPredictionStatus::overpredictedItems)
```

### NativeSampleClassPredictionStatus · "overpredictions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6661)

```cpp
def_readonly("overpredictions",
&nsf::SampleClassPredictionStatus::overpredictions)
```

### NativeSampleClassPredictionStatus · "underpredicted_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6659)

```cpp
def_readonly("underpredicted_items",
&nsf::SampleClassPredictionStatus::underpredictedItems)
```

### NativeSampleClassPredictionStatus · "underpredictions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6657)

```cpp
def_readonly("underpredictions",
&nsf::SampleClassPredictionStatus::underpredictions)
```

### NativeSampleClassPredictionStatus · "observations"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6656)

```cpp
def_readonly("observations",
&nsf::SampleClassPredictionStatus::observations)
```

### NativeSampleClassPredictionStatus · "prediction"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6655)

```cpp
def_readonly("prediction",
&nsf::SampleClassPredictionStatus::prediction)
```

### NativeSampleClassPredictionStatus · "class_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6654)

```cpp
def_readonly("class_id",
&nsf::SampleClassPredictionStatus::classId)
```

### NativeLiveStreamSamplePredictor · "statuses"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6676)

```cpp
def("statuses",
&nsf::LiveStreamSamplePredictor::statuses)
```

### NativeLiveStreamSamplePredictor · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6675)

```cpp
def("status",
&nsf::LiveStreamSamplePredictor::status)
```

### NativeLiveStreamSamplePredictor · "observe"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6674)

```cpp
def("observe",
&nsf::LiveStreamSamplePredictor::observe)
```

### NativeLiveStreamSamplePredictor · "predict"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6670)

```cpp
def("predict",
[] (const nsf::LiveStreamSamplePredictor& predictor,
                         const std::string& classId) { implementation omitted })
```

### NativeLiveStreamSamplePredictor · "reset"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6669)

```cpp
def("reset",
&nsf::LiveStreamSamplePredictor::reset)
```

### NativeLiveStreamFecOptions · "validate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6697)

```cpp
def("validate",
[] (const nsf::LiveStreamFecOptions& value) -> py::object { implementation omitted })
```

### NativeLiveStreamFecOptions · "enabled"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6696)

```cpp
def_property_readonly("enabled",
&nsf::LiveStreamFecOptions::enabled)
```

### NativeLiveStreamFecOptions · "recovery_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6695)

```cpp
def_property_readonly("recovery_capacity",
&nsf::LiveStreamFecOptions::recoveryCapacity)
```

### NativeLiveStreamFecOptions · "gf256_two_repair"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6692)

```cpp
def_static("gf256_two_repair",
&nsf::LiveStreamFecOptions::gf256TwoRepair,
py::arg("source_items"),
py::arg("max_source_bytes"),
py::arg("recovery_budget_ms") = 500)
```

### NativeLiveStreamFecOptions · "xor_one_repair"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6689)

```cpp
def_static("xor_one_repair",
&nsf::LiveStreamFecOptions::xorOneRepair,
py::arg("source_items"),
py::arg("max_source_bytes"),
py::arg("recovery_budget_ms") = 500)
```

### NativeLiveStreamFecOptions · "none"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6688)

```cpp
def_static("none",
&nsf::LiveStreamFecOptions::none)
```

### NativeLiveStreamFecOptions · "repair_symbols"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6687)

```cpp
def_readwrite("repair_symbols",
&nsf::LiveStreamFecOptions::repairSymbols)
```

### NativeLiveStreamFecOptions · "recovery_budget_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6686)

```cpp
def_readwrite("recovery_budget_ms",
&nsf::LiveStreamFecOptions::recoveryBudgetMs)
```

### NativeLiveStreamFecOptions · "max_source_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6685)

```cpp
def_readwrite("max_source_bytes",
&nsf::LiveStreamFecOptions::maxSourceBytes)
```

### NativeLiveStreamFecOptions · "source_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6682)

```cpp
def_property("source_items",
[] (const nsf::LiveStreamFecOptions& value) { implementation omitted },
[] (nsf::LiveStreamFecOptions& value, size_t count) { implementation omitted })
```

### NativeLiveStreamFecOptions · "max_source_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6681)

```cpp
def_readwrite("max_source_items",
&nsf::LiveStreamFecOptions::maxSourceItems)
```

### NativeLiveStreamFecOptions · "scheme"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6680)

```cpp
def_readwrite("scheme",
&nsf::LiveStreamFecOptions::scheme)
```

### NativeStreamAdvancedOptions · "startup_timeout_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6716)

```cpp
def_readwrite("startup_timeout_ms",
&nsf::StreamAdvancedOptions::startupTimeoutMs)
```

### NativeStreamAdvancedOptions · "signed_wire_cap"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6714)

```cpp
def_readwrite("signed_wire_cap",
&nsf::StreamAdvancedOptions::signedWireCap)
```

### NativeStreamAdvancedOptions · "max_pending_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6712)

```cpp
def_readwrite("max_pending_interests",
&nsf::StreamAdvancedOptions::maxPendingInterests)
```

### NativeStreamAdvancedOptions · "max_name_reservations"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6710)

```cpp
def_readwrite("max_name_reservations",
&nsf::StreamAdvancedOptions::maxNameReservations)
```

### NativeStreamAdvancedOptions · "retained_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6708)

```cpp
def_readwrite("retained_items",
&nsf::StreamAdvancedOptions::retainedItems)
```

### NativeStreamAdvancedOptions · "mapping_ahead_blocks"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6706)

```cpp
def_readwrite("mapping_ahead_blocks",
&nsf::StreamAdvancedOptions::mappingAheadBlocks)
```

### NativeStreamAdvancedOptions · "mapping_block_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6704)

```cpp
def_readwrite("mapping_block_capacity",
&nsf::StreamAdvancedOptions::mappingBlockCapacity)
```

### NativeStreamConfig · "advanced"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6731)

```cpp
def_readwrite("advanced",
&nsf::StreamConfig::advanced)
```

### NativeStreamConfig · "session_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6730)

```cpp
def_readwrite("session_epoch",
&nsf::StreamConfig::sessionEpoch)
```

### NativeStreamConfig · "fec"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6729)

```cpp
def_readwrite("fec",
&nsf::StreamConfig::fec)
```

### NativeStreamConfig · "sample_classes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6728)

```cpp
def_readwrite("sample_classes",
&nsf::StreamConfig::sampleClasses)
```

### NativeStreamConfig · "sample_period_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6727)

```cpp
def_readwrite("sample_period_ms",
&nsf::StreamConfig::samplePeriodMs)
```

### NativeStreamConfig · "data_prefix"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6722)

```cpp
def_property("data_prefix",
[] (const nsf::StreamConfig& value) { implementation omitted },
[] (nsf::StreamConfig& value, const std::string& name) { implementation omitted })
```

### NativeStreamConfig · "stream_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6721)

```cpp
def_readwrite("stream_id",
&nsf::StreamConfig::streamId)
```

### NativeLiveStreamDefinition · "validate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6757)

```cpp
def("validate",
[] (const nsf::LiveStreamDefinition& value) -> py::object { implementation omitted })
```

### NativeLiveStreamDefinition · "mapping_root"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6754)

```cpp
def_property_readonly("mapping_root",
[] (const nsf::LiveStreamDefinition& value) { implementation omitted })
```

### NativeLiveStreamDefinition · "fec"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6753)

```cpp
def_readwrite("fec",
&nsf::LiveStreamDefinition::fec)
```

### NativeLiveStreamDefinition · "sample_classes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6752)

```cpp
def_readwrite("sample_classes",
&nsf::LiveStreamDefinition::sampleClasses)
```

### NativeLiveStreamDefinition · "sample_period_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6751)

```cpp
def_readwrite("sample_period_ms",
&nsf::LiveStreamDefinition::samplePeriodMs)
```

### NativeLiveStreamDefinition · "signed_wire_cap"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6750)

```cpp
def_readwrite("signed_wire_cap",
&nsf::LiveStreamDefinition::signedWireCap)
```

### NativeLiveStreamDefinition · "max_pending_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6749)

```cpp
def_readwrite("max_pending_interests",
&nsf::LiveStreamDefinition::maxPendingInterests)
```

### NativeLiveStreamDefinition · "max_name_reservations"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6748)

```cpp
def_readwrite("max_name_reservations",
&nsf::LiveStreamDefinition::maxNameReservations)
```

### NativeLiveStreamDefinition · "retained_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6747)

```cpp
def_readwrite("retained_items",
&nsf::LiveStreamDefinition::retainedItems)
```

### NativeLiveStreamDefinition · "mapping_ahead_blocks"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6746)

```cpp
def_readwrite("mapping_ahead_blocks",
&nsf::LiveStreamDefinition::mappingAheadBlocks)
```

### NativeLiveStreamDefinition · "mapping_block_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6745)

```cpp
def_readwrite("mapping_block_capacity",
&nsf::LiveStreamDefinition::mappingBlockCapacity)
```

### NativeLiveStreamDefinition · "mapping_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6744)

```cpp
def_readwrite("mapping_version",
&nsf::LiveStreamDefinition::mappingVersion)
```

### NativeLiveStreamDefinition · "session_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6743)

```cpp
def_readwrite("session_epoch",
&nsf::LiveStreamDefinition::sessionEpoch)
```

### NativeLiveStreamDefinition · "semantic_data_prefix"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6740)

```cpp
def_property("semantic_data_prefix",
[] (const nsf::LiveStreamDefinition& value) { implementation omitted },
[] (nsf::LiveStreamDefinition& value, const std::string& name) { implementation omitted })
```

### NativeLiveStreamDefinition · "provider"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6737)

```cpp
def_property("provider",
[] (const nsf::LiveStreamDefinition& value) { implementation omitted },
[] (nsf::LiveStreamDefinition& value, const std::string& name) { implementation omitted })
```

### NativeLiveStreamDefinition · "stream_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6736)

```cpp
def_readwrite("stream_id",
&nsf::LiveStreamDefinition::streamId)
```

### NativeLiveStreamDefinition · "contract_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6735)

```cpp
def_readwrite("contract_version",
&nsf::LiveStreamDefinition::contractVersion)
```

### NativeLiveStreamItemReservation · "mapping_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6769)

```cpp
def_readonly("mapping_version",
&nsf::LiveStreamItemReservation::mappingVersion)
```

### NativeLiveStreamItemReservation · "session_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6768)

```cpp
def_readonly("session_epoch",
&nsf::LiveStreamItemReservation::sessionEpoch)
```

### NativeLiveStreamItemReservation · "original_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6765)

```cpp
def_property_readonly("original_name",
[] (const nsf::LiveStreamItemReservation& value) { implementation omitted })
```

### NativeLiveStreamItemReservation · "cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6764)

```cpp
def_readonly("cursor",
&nsf::LiveStreamItemReservation::cursor)
```

### NativeLiveStreamGroupReservation · "repairs"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6776)

```cpp
def_readonly("repairs",
&nsf::LiveStreamGroupReservation::repairs)
```

### NativeLiveStreamGroupReservation · "sources"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6775)

```cpp
def_readonly("sources",
&nsf::LiveStreamGroupReservation::sources)
```

### NativeLiveStreamGroupReservation · "group_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6772)

```cpp
def_property_readonly("group_id",
[] (const nsf::LiveStreamGroupReservation& value) { implementation omitted })
```

### NativeLiveStreamItemKind · "REPAIR"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6780)

```cpp
value("REPAIR",
nsf::LiveStreamItemKind::Repair)
```

### NativeLiveStreamItemKind · "SOURCE"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6779)

```cpp
value("SOURCE",
nsf::LiveStreamItemKind::Source)
```

### NativeLiveStreamSampleReservation · "group"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6788)

```cpp
def_readonly("group",
&nsf::LiveStreamSampleReservation::group)
```

### NativeLiveStreamSampleReservation · "predicted_source_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6786)

```cpp
def_readonly("predicted_source_items",
&nsf::LiveStreamSampleReservation::predictedSourceItems)
```

### NativeLiveStreamSampleReservation · "sample_class"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6785)

```cpp
def_readonly("sample_class",
&nsf::LiveStreamSampleReservation::sampleClass)
```

### NativeLiveStreamSampleReservation · "sample_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6784)

```cpp
def_readonly("sample_id",
&nsf::LiveStreamSampleReservation::sampleId)
```

### NativeLiveStreamReadiness · "safe_join_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6793)

```cpp
def_readwrite("safe_join_cursor",
&nsf::LiveStreamReadiness::safeJoinCursor)
```

### NativeLiveStreamReadiness · "measured_sample_period_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6792)

```cpp
def_readwrite("measured_sample_period_ms",
&nsf::LiveStreamReadiness::measuredSamplePeriodMs)
```

### NativeLiveStreamDescriptor · "validate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6801)

```cpp
def("validate",
[] (const nsf::LiveStreamDescriptor& value) -> py::object { implementation omitted })
```

### NativeLiveStreamDescriptor · "safe_join_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6800)

```cpp
def_readwrite("safe_join_cursor",
&nsf::LiveStreamDescriptor::safeJoinCursor)
```

### NativeLiveStreamDescriptor · "measured_sample_period_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6799)

```cpp
def_readwrite("measured_sample_period_ms",
&nsf::LiveStreamDescriptor::measuredSamplePeriodMs)
```

### NativeLiveStreamDescriptor · "checkpoint"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6798)

```cpp
def_readwrite("checkpoint",
&nsf::LiveStreamDescriptor::checkpoint)
```

### NativeLiveStreamDescriptor · "definition"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6797)

```cpp
def_readwrite("definition",
&nsf::LiveStreamDescriptor::definition)
```

### NativeLiveStreamLifecycleState · "FAILED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6810)

```cpp
value("FAILED",
nsf::LiveStreamLifecycleState::Failed)
```

### NativeLiveStreamLifecycleState · "STOPPED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6809)

```cpp
value("STOPPED",
nsf::LiveStreamLifecycleState::Stopped)
```

### NativeLiveStreamLifecycleState · "ACTIVE"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6808)

```cpp
value("ACTIVE",
nsf::LiveStreamLifecycleState::Active)
```

### NativeLiveStreamLifecycleState · "PREPARING"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6807)

```cpp
value("PREPARING",
nsf::LiveStreamLifecycleState::Preparing)
```

### NativeLiveStreamItemProvenance · "FEC_RECOVERED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6814)

```cpp
value("FEC_RECOVERED",
nsf::LiveStreamItemProvenance::FecRecovered)
```

### NativeLiveStreamItemProvenance · "SIGNED_DATA"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6813)

```cpp
value("SIGNED_DATA",
nsf::LiveStreamItemProvenance::SignedData)
```

### NativeVerifiedLiveStreamItem · "received_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6828)

```cpp
def_readonly("received_ms",
&nsf::VerifiedLiveStreamItem::receivedMs)
```

### NativeVerifiedLiveStreamItem · "provenance"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6827)

```cpp
def_readonly("provenance",
&nsf::VerifiedLiveStreamItem::provenance)
```

### NativeVerifiedLiveStreamItem · "content"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6824)

```cpp
def_property_readonly("content",
[] (const nsf::VerifiedLiveStreamItem& value) { implementation omitted })
```

### NativeVerifiedLiveStreamItem · "verified_provider"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6821)

```cpp
def_property_readonly("verified_provider",
[] (const nsf::VerifiedLiveStreamItem& value) { implementation omitted })
```

### NativeVerifiedLiveStreamItem · "original_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6818)

```cpp
def_property_readonly("original_name",
[] (const nsf::VerifiedLiveStreamItem& value) { implementation omitted })
```

### NativeVerifiedLiveStreamItem · "cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6817)

```cpp
def_readonly("cursor",
&nsf::VerifiedLiveStreamItem::cursor)
```

### NativeLiveStreamItemAdmission · "reject_item"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6834)

```cpp
def_static("reject_item",
&nsf::LiveStreamItemAdmission::rejectItem)
```

### NativeLiveStreamItemAdmission · "accept_item"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6833)

```cpp
def_static("accept_item",
&nsf::LiveStreamItemAdmission::acceptItem)
```

### NativeLiveStreamItemAdmission · "reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6832)

```cpp
def_readonly("reason",
&nsf::LiveStreamItemAdmission::reason)
```

### NativeLiveStreamItemAdmission · "accepted"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6831)

```cpp
def_readonly("accepted",
&nsf::LiveStreamItemAdmission::accepted)
```

### NativeLiveStreamSampleObservation · "item_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6841)

```cpp
def_readwrite("item_count",
&nsf::LiveStreamSampleObservation::itemCount)
```

### NativeLiveStreamSampleObservation · "retrieval_delay_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6840)

```cpp
def_readwrite("retrieval_delay_ms",
&nsf::LiveStreamSampleObservation::retrievalDelayMs)
```

### NativeLiveStreamSampleObservation · "arrival_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6839)

```cpp
def_readwrite("arrival_ms",
&nsf::LiveStreamSampleObservation::arrivalMs)
```

### NativeLiveStreamSampleObservation · "sample_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6838)

```cpp
def_readwrite("sample_id",
&nsf::LiveStreamSampleObservation::sampleId)
```

### NativeLiveStreamStatus · "fetch_decision"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6953)

```cpp
def_readonly("fetch_decision",
&nsf::LiveStreamStatus::fetchDecision)
```

### NativeLiveStreamStatus · "reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6952)

```cpp
def_readonly("reason",
&nsf::LiveStreamStatus::reason)
```

### NativeLiveStreamStatus · "sample_class_predictions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6950)

```cpp
def_readonly("sample_class_predictions",
&nsf::LiveStreamStatus::sampleClassPredictions)
```

### NativeLiveStreamStatus · "provider_retry_future_hits"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6948)

```cpp
def_readonly("provider_retry_future_hits",
&nsf::LiveStreamStatus::providerRetryFutureHits)
```

### NativeLiveStreamStatus · "provider_retry_future_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6946)

```cpp
def_readonly("provider_retry_future_interests",
&nsf::LiveStreamStatus::providerRetryFutureInterests)
```

### NativeLiveStreamStatus · "provider_initial_future_hits"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6944)

```cpp
def_readonly("provider_initial_future_hits",
&nsf::LiveStreamStatus::providerInitialFutureHits)
```

### NativeLiveStreamStatus · "provider_initial_future_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6942)

```cpp
def_readonly("provider_initial_future_interests",
&nsf::LiveStreamStatus::providerInitialFutureInterests)
```

### NativeLiveStreamStatus · "provider_future_hits"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6941)

```cpp
def_readonly("provider_future_hits",
&nsf::LiveStreamStatus::providerFutureHits)
```

### NativeLiveStreamStatus · "provider_future_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6940)

```cpp
def_readonly("provider_future_interests",
&nsf::LiveStreamStatus::providerFutureInterests)
```

### NativeLiveStreamStatus · "mapping_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6939)

```cpp
def_readonly("mapping_bytes",
&nsf::LiveStreamStatus::mappingBytes)
```

### NativeLiveStreamStatus · "terminal_gap_superseded"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6937)

```cpp
def_readonly("terminal_gap_superseded",
&nsf::LiveStreamStatus::terminalGapSuperseded)
```

### NativeLiveStreamStatus · "stale_ready_drops"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6935)

```cpp
def_readonly("stale_ready_drops",
&nsf::LiveStreamStatus::staleReadyDrops)
```

### NativeLiveStreamStatus · "drain_wake_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6933)

```cpp
def_readonly("drain_wake_count",
&nsf::LiveStreamStatus::drainWakeCount)
```

### NativeLiveStreamStatus · "terminal_gap_queue_depth"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6931)

```cpp
def_readonly("terminal_gap_queue_depth",
&nsf::LiveStreamStatus::terminalGapQueueDepth)
```

### NativeLiveStreamStatus · "oldest_ready_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6929)

```cpp
def_readonly("oldest_ready_cursor",
&nsf::LiveStreamStatus::oldestReadyCursor)
```

### NativeLiveStreamStatus · "ready_queue_depth"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6927)

```cpp
def_readonly("ready_queue_depth",
&nsf::LiveStreamStatus::readyQueueDepth)
```

### NativeLiveStreamStatus · "next_deliver_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6925)

```cpp
def_readonly("next_deliver_cursor",
&nsf::LiveStreamStatus::nextDeliverCursor)
```

### NativeLiveStreamStatus · "recovery_metadata_cache_hits"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6923)

```cpp
def_readonly("recovery_metadata_cache_hits",
&nsf::LiveStreamStatus::recoveryMetadataCacheHits)
```

### NativeLiveStreamStatus · "recovery_coalesced_waiters"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6921)

```cpp
def_readonly("recovery_coalesced_waiters",
&nsf::LiveStreamStatus::recoveryCoalescedWaiters)
```

### NativeLiveStreamStatus · "recovery_group_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6919)

```cpp
def_readonly("recovery_group_interests",
&nsf::LiveStreamStatus::recoveryGroupInterests)
```

### NativeLiveStreamStatus · "recovery_frontier_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6917)

```cpp
def_readonly("recovery_frontier_interests",
&nsf::LiveStreamStatus::recoveryFrontierInterests)
```

### NativeLiveStreamStatus · "recovery_control_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6915)

```cpp
def_readonly("recovery_control_interests",
&nsf::LiveStreamStatus::recoveryControlInterests)
```

### NativeLiveStreamStatus · "recovery_exhaustions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6914)

```cpp
def_readonly("recovery_exhaustions",
&nsf::LiveStreamStatus::recoveryExhaustions)
```

### NativeLiveStreamStatus · "recovery_attempts"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6913)

```cpp
def_readonly("recovery_attempts",
&nsf::LiveStreamStatus::recoveryAttempts)
```

### NativeLiveStreamStatus · "recovered_groups"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6911)

```cpp
def_readonly("recovered_groups",
&nsf::LiveStreamStatus::recoveredGroups)
```

### NativeLiveStreamStatus · "recoverable_groups"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6909)

```cpp
def_readonly("recoverable_groups",
&nsf::LiveStreamStatus::recoverableGroups)
```

### NativeLiveStreamStatus · "terminal_missing_sources"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6907)

```cpp
def_readonly("terminal_missing_sources",
&nsf::LiveStreamStatus::terminalMissingSources)
```

### NativeLiveStreamStatus · "recovery_eligible_sources"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6905)

```cpp
def_readonly("recovery_eligible_sources",
&nsf::LiveStreamStatus::recoveryEligibleSources)
```

### NativeLiveStreamStatus · "declared_recovery_capacity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6903)

```cpp
def_readonly("declared_recovery_capacity",
&nsf::LiveStreamStatus::declaredRecoveryCapacity)
```

### NativeLiveStreamStatus · "retry_suppression_reasons"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6901)

```cpp
def_readonly("retry_suppression_reasons",
&nsf::LiveStreamStatus::retrySuppressionReasons)
```

### NativeLiveStreamStatus · "retry_suppressions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6900)

```cpp
def_readonly("retry_suppressions",
&nsf::LiveStreamStatus::retrySuppressions)
```

### NativeLiveStreamStatus · "retry_successes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6899)

```cpp
def_readonly("retry_successes",
&nsf::LiveStreamStatus::retrySuccesses)
```

### NativeLiveStreamStatus · "future_cursor_horizon"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6897)

```cpp
def_readonly("future_cursor_horizon",
&nsf::LiveStreamStatus::futureCursorHorizon)
```

### NativeLiveStreamStatus · "retry_future_payload_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6895)

```cpp
def_readonly("retry_future_payload_interests",
&nsf::LiveStreamStatus::retryFuturePayloadInterests)
```

### NativeLiveStreamStatus · "initial_future_payload_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6893)

```cpp
def_readonly("initial_future_payload_interests",
&nsf::LiveStreamStatus::initialFuturePayloadInterests)
```

### NativeLiveStreamStatus · "future_payload_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6892)

```cpp
def_readonly("future_payload_interests",
&nsf::LiveStreamStatus::futurePayloadInterests)
```

### NativeLiveStreamStatus · "payload_unresolved_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6890)

```cpp
def_readonly("payload_unresolved_interests",
&nsf::LiveStreamStatus::payloadUnresolvedInterests)
```

### NativeLiveStreamStatus · "payload_nonproductive_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6888)

```cpp
def_readonly("payload_nonproductive_interests",
&nsf::LiveStreamStatus::payloadNonproductiveInterests)
```

### NativeLiveStreamStatus · "payload_protection_only_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6886)

```cpp
def_readonly("payload_protection_only_interests",
&nsf::LiveStreamStatus::payloadProtectionOnlyInterests)
```

### NativeLiveStreamStatus · "payload_application_useful_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6884)

```cpp
def_readonly("payload_application_useful_interests",
&nsf::LiveStreamStatus::payloadApplicationUsefulInterests)
```

### NativeLiveStreamStatus · "payload_repair_data_consumed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6882)

```cpp
def_readonly("payload_repair_data_consumed",
&nsf::LiveStreamStatus::payloadRepairDataConsumed)
```

### NativeLiveStreamStatus · "payload_repair_data_responses"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6880)

```cpp
def_readonly("payload_repair_data_responses",
&nsf::LiveStreamStatus::payloadRepairDataResponses)
```

### NativeLiveStreamStatus · "payload_source_data_admissions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6878)

```cpp
def_readonly("payload_source_data_admissions",
&nsf::LiveStreamStatus::payloadSourceDataAdmissions)
```

### NativeLiveStreamStatus · "payload_unclassified_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6876)

```cpp
def_readonly("payload_unclassified_interests",
&nsf::LiveStreamStatus::payloadUnclassifiedInterests)
```

### NativeLiveStreamStatus · "retry_payload_repair_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6874)

```cpp
def_readonly("retry_payload_repair_interests",
&nsf::LiveStreamStatus::retryPayloadRepairInterests)
```

### NativeLiveStreamStatus · "initial_payload_repair_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6872)

```cpp
def_readonly("initial_payload_repair_interests",
&nsf::LiveStreamStatus::initialPayloadRepairInterests)
```

### NativeLiveStreamStatus · "payload_repair_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6871)

```cpp
def_readonly("payload_repair_interests",
&nsf::LiveStreamStatus::payloadRepairInterests)
```

### NativeLiveStreamStatus · "retry_payload_source_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6869)

```cpp
def_readonly("retry_payload_source_interests",
&nsf::LiveStreamStatus::retryPayloadSourceInterests)
```

### NativeLiveStreamStatus · "initial_payload_source_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6867)

```cpp
def_readonly("initial_payload_source_interests",
&nsf::LiveStreamStatus::initialPayloadSourceInterests)
```

### NativeLiveStreamStatus · "payload_source_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6866)

```cpp
def_readonly("payload_source_interests",
&nsf::LiveStreamStatus::payloadSourceInterests)
```

### NativeLiveStreamStatus · "retry_payload_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6865)

```cpp
def_readonly("retry_payload_interests",
&nsf::LiveStreamStatus::retryPayloadInterests)
```

### NativeLiveStreamStatus · "initial_payload_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6864)

```cpp
def_readonly("initial_payload_interests",
&nsf::LiveStreamStatus::initialPayloadInterests)
```

### NativeLiveStreamStatus · "payload_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6863)

```cpp
def_readonly("payload_interests",
&nsf::LiveStreamStatus::payloadInterests)
```

### NativeLiveStreamStatus · "mapping_new_data_responses"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6861)

```cpp
def_readonly("mapping_new_data_responses",
&nsf::LiveStreamStatus::mappingNewDataResponses)
```

### NativeLiveStreamStatus · "mapping_data_responses"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6860)

```cpp
def_readonly("mapping_data_responses",
&nsf::LiveStreamStatus::mappingDataResponses)
```

### NativeLiveStreamStatus · "mapping_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6859)

```cpp
def_readonly("mapping_interests",
&nsf::LiveStreamStatus::mappingInterests)
```

### NativeLiveStreamStatus · "retry_exhaustions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6858)

```cpp
def_readonly("retry_exhaustions",
&nsf::LiveStreamStatus::retryExhaustions)
```

### NativeLiveStreamStatus · "deadline_skips"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6857)

```cpp
def_readonly("deadline_skips",
&nsf::LiveStreamStatus::deadlineSkips)
```

### NativeLiveStreamStatus · "late_arrivals"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6856)

```cpp
def_readonly("late_arrivals",
&nsf::LiveStreamStatus::lateArrivals)
```

### NativeLiveStreamStatus · "retry_attempts"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6855)

```cpp
def_readonly("retry_attempts",
&nsf::LiveStreamStatus::retryAttempts)
```

### NativeLiveStreamStatus · "nacks"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6854)

```cpp
def_readonly("nacks",
&nsf::LiveStreamStatus::nacks)
```

### NativeLiveStreamStatus · "timeouts"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6853)

```cpp
def_readonly("timeouts",
&nsf::LiveStreamStatus::timeouts)
```

### NativeLiveStreamStatus · "recovered"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6852)

```cpp
def_readonly("recovered",
&nsf::LiveStreamStatus::recovered)
```

### NativeLiveStreamStatus · "rejected"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6851)

```cpp
def_readonly("rejected",
&nsf::LiveStreamStatus::rejected)
```

### NativeLiveStreamStatus · "delivered"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6850)

```cpp
def_readonly("delivered",
&nsf::LiveStreamStatus::delivered)
```

### NativeLiveStreamStatus · "in_flight"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6849)

```cpp
def_readonly("in_flight",
&nsf::LiveStreamStatus::inFlight)
```

### NativeLiveStreamStatus · "mapping_blocks"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6848)

```cpp
def_readonly("mapping_blocks",
&nsf::LiveStreamStatus::mappingBlocks)
```

### NativeLiveStreamStatus · "pending_interests"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6847)

```cpp
def_readonly("pending_interests",
&nsf::LiveStreamStatus::pendingInterests)
```

### NativeLiveStreamStatus · "retained_items"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6846)

```cpp
def_readonly("retained_items",
&nsf::LiveStreamStatus::retainedItems)
```

### NativeLiveStreamStatus · "frontiers"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6845)

```cpp
def_readonly("frontiers",
&nsf::LiveStreamStatus::frontiers)
```

### NativeLiveStreamStatus · "state"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6844)

```cpp
def_readonly("state",
&nsf::LiveStreamStatus::state)
```

### NativePublishedLiveStreamPacketKind · "REPAIR"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6958)

```cpp
value("REPAIR",
nsf::PublishedLiveStreamPacketKind::Repair)
```

### NativePublishedLiveStreamPacketKind · "SOURCE"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6957)

```cpp
value("SOURCE",
nsf::PublishedLiveStreamPacketKind::Source)
```

### NativePublishedLiveStreamPacketKind · "MAPPING"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6956)

```cpp
value("MAPPING",
nsf::PublishedLiveStreamPacketKind::Mapping)
```

### NativePublishedLiveStreamPacket · "materialized_monotonic_us"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6981)

```cpp
def_readonly("materialized_monotonic_us",
&nsf::PublishedLiveStreamPacket::materializedMonotonicUs)
```

### NativePublishedLiveStreamPacket · "wire_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6978)

```cpp
def_property_readonly("wire_digest",
[] (const nsf::PublishedLiveStreamPacket& value) { implementation omitted })
```

### NativePublishedLiveStreamPacket · "signed_data_wire"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6974)

```cpp
def_property_readonly("signed_data_wire",
[] (const nsf::PublishedLiveStreamPacket& value) { implementation omitted })
```

### NativePublishedLiveStreamPacket · "provider"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6971)

```cpp
def_property_readonly("provider",
[] (const nsf::PublishedLiveStreamPacket& value) { implementation omitted })
```

### NativePublishedLiveStreamPacket · "data_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6968)

```cpp
def_property_readonly("data_name",
[] (const nsf::PublishedLiveStreamPacket& value) { implementation omitted })
```

### NativePublishedLiveStreamPacket · "cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6965)

```cpp
def_property_readonly("cursor",
[] (const nsf::PublishedLiveStreamPacket& value) -> py::object { implementation omitted })
```

### NativePublishedLiveStreamPacket · "mapping_version"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6964)

```cpp
def_readonly("mapping_version",
&nsf::PublishedLiveStreamPacket::mappingVersion)
```

### NativePublishedLiveStreamPacket · "session_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6963)

```cpp
def_readonly("session_epoch",
&nsf::PublishedLiveStreamPacket::sessionEpoch)
```

### NativePublishedLiveStreamPacket · "stream_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6962)

```cpp
def_readonly("stream_id",
&nsf::PublishedLiveStreamPacket::streamId)
```

### NativePublishedLiveStreamPacket · "kind"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6961)

```cpp
def_readonly("kind",
&nsf::PublishedLiveStreamPacket::kind)
```

### NativePublishedPacketFeedOptions · "max_queued_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6988)

```cpp
def_readwrite("max_queued_bytes",
&nsf::PublishedPacketFeedOptions::maxQueuedBytes)
```

### NativePublishedPacketFeedOptions · "max_queued_packets"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6987)

```cpp
def_readwrite("max_queued_packets",
&nsf::PublishedPacketFeedOptions::maxQueuedPackets)
```

### NativePublishedPacketFeedOptions · "from_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6986)

```cpp
def_readwrite("from_cursor",
&nsf::PublishedPacketFeedOptions::fromCursor)
```

### NativePublishedPacketFeedStatus · "closed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6996)

```cpp
def_readonly("closed",
&nsf::PublishedPacketFeedStatus::closed)
```

### NativePublishedPacketFeedStatus · "last_dropped_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6995)

```cpp
def_readonly("last_dropped_cursor",
&nsf::PublishedPacketFeedStatus::lastDroppedCursor)
```

### NativePublishedPacketFeedStatus · "first_dropped_cursor"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6994)

```cpp
def_readonly("first_dropped_cursor",
&nsf::PublishedPacketFeedStatus::firstDroppedCursor)
```

### NativePublishedPacketFeedStatus · "dropped_packets"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6993)

```cpp
def_readonly("dropped_packets",
&nsf::PublishedPacketFeedStatus::droppedPackets)
```

### NativePublishedPacketFeedStatus · "queued_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6992)

```cpp
def_readonly("queued_bytes",
&nsf::PublishedPacketFeedStatus::queuedBytes)
```

### NativePublishedPacketFeedStatus · "queued_packets"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L6991)

```cpp
def_readonly("queued_packets",
&nsf::PublishedPacketFeedStatus::queuedPackets)
```

### NativePublishedPacketFeed · "close"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7002)

```cpp
def("close",
&nsf::PublishedPacketFeed::close)
```

### NativePublishedPacketFeed · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7001)

```cpp
def("status",
&nsf::PublishedPacketFeed::status)
```

### NativePublishedPacketFeed · "take_available"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7000)

```cpp
def("take_available",
&nsf::PublishedPacketFeed::takeAvailable)
```

### NativeLiveStreamPublisher · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7069)

```cpp
def("stop",
&nsf::LiveStreamPublisher::stop)
```

### NativeLiveStreamPublisher · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7068)

```cpp
def("status",
&nsf::LiveStreamPublisher::status)
```

### NativeLiveStreamPublisher · "open_published_packet_feed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7067)

```cpp
def("open_published_packet_feed",
&nsf::LiveStreamPublisher::openPublishedPacketFeed)
```

### NativeLiveStreamPublisher · "activate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7066)

```cpp
def("activate",
&nsf::LiveStreamPublisher::activate)
```

### NativeLiveStreamPublisher · "publish_sample"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7056)

```cpp
def("publish_sample",
[] (nsf::LiveStreamPublisher& publisher,
                                 const nsf::LiveStreamSampleReservation& reservation,
                                 const std::vector<py::bytes>& contents) { implementation omitted })
```

### NativeLiveStreamPublisher · "publish_group"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7046)

```cpp
def("publish_group",
[] (nsf::LiveStreamPublisher& publisher,
                               const nsf::LiveStreamGroupReservation& reservation,
                               const std::vector<py::bytes>& contents) { implementation omitted })
```

### NativeLiveStreamPublisher · "publish"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7040)

```cpp
def("publish",
[] (nsf::LiveStreamPublisher& publisher,
                         const nsf::LiveStreamItemReservation& reservation,
                         const py::bytes& content) { implementation omitted })
```

### NativeLiveStreamPublisher · "prepare_sample_extent"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7038)

```cpp
def("prepare_sample_extent",
&nsf::LiveStreamPublisher::prepareSampleExtent,
py::arg("reservation"),
py::arg("actual_source_items"))
```

### NativeLiveStreamPublisher · "announce_sample"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7025)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7015)

```cpp
def("reserve_group",
[] (nsf::LiveStreamPublisher& publisher,
                               const std::string& groupId,
                               const std::vector<std::string>& sourceNames,
                               const std::vector<std::string>& repairNames) { implementation omitted })
```

### NativeLiveStreamPublisher · "reserve_many_ahead"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7009)

```cpp
def("reserve_many_ahead",
[] (nsf::LiveStreamPublisher& publisher,
                                    const std::vector<std::string>& names) { implementation omitted })
```

### NativeLiveStreamPublisher · "reserve_ahead"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7006)

```cpp
def("reserve_ahead",
[] (nsf::LiveStreamPublisher& publisher, const std::string& name) { implementation omitted })
```

### NativeStreamPublisher · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7086)

```cpp
def("stop",
&nsf::StreamPublisher::stop)
```

### NativeStreamPublisher · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7085)

```cpp
def("status",
&nsf::StreamPublisher::status)
```

### NativeStreamPublisher · "flush"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7084)

```cpp
def("flush",
&nsf::StreamPublisher::flush)
```

### NativeStreamPublisher · "push"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7077)

```cpp
def("push",
[] (nsf::StreamPublisher& publisher, const py::bytes& data) { implementation omitted },
py::arg("signed_data"))
```

### NativeStreamPublisher · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7073)

```cpp
def("start",
[] (nsf::StreamPublisher& publisher) { implementation omitted })
```

### NativePredictiveStreamCheckpoint · "next_expected_sample_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7097)

```cpp
def_readwrite("next_expected_sample_id",
&nsf::PredictiveStreamCheckpoint::nextExpectedSampleId)
```

### NativePredictiveStreamCheckpoint · "latest_produced_sample_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7095)

```cpp
def_readwrite("latest_produced_sample_id",
&nsf::PredictiveStreamCheckpoint::latestProducedSampleId)
```

### NativePredictiveStreamCheckpoint · "oldest_retained_sample_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7093)

```cpp
def_readwrite("oldest_retained_sample_id",
&nsf::PredictiveStreamCheckpoint::oldestRetainedSampleId)
```

### NativePredictiveStreamCheckpoint · "initial_sample_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7091)

```cpp
def_readwrite("initial_sample_id",
&nsf::PredictiveStreamCheckpoint::initialSampleId)
```

### NativePredictiveStreamDescriptor · "measured_sample_period_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7131)

```cpp
def_property_readonly("measured_sample_period_ms",
[] (const nsf::PredictiveStreamDescriptor& d) { implementation omitted })
```

### NativePredictiveStreamDescriptor · "frontier_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7127)

```cpp
def_property_readonly("frontier_name",
[] (const nsf::PredictiveStreamDescriptor& d) { implementation omitted })
```

### NativePredictiveStreamDescriptor · "checkpoint"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7123)

```cpp
def_property_readonly("checkpoint",
[] (const nsf::PredictiveStreamDescriptor& d) { implementation omitted })
```

### NativePredictiveStreamDescriptor · "definition"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7119)

```cpp
def_property_readonly("definition",
[] (const nsf::PredictiveStreamDescriptor& d) { implementation omitted })
```

### NativeLiveStreamConsumerHandle · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7141)

```cpp
def("stop",
&nsf::LiveStreamConsumerHandle::stop)
```

### NativeLiveStreamConsumerHandle · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7140)

```cpp
def("status",
&nsf::LiveStreamConsumerHandle::status)
```

### NativeLiveStreamConsumerHandle · "observe_accepted_sample"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7139)

```cpp
def("observe_accepted_sample",
&nsf::LiveStreamConsumerHandle::observeAcceptedSample)
```

### NativeLiveStreamConsumerHandle · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7138)

```cpp
def("start",
&nsf::LiveStreamConsumerHandle::start)
```

### NativePredictiveStreamSubscriber · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7148)

```cpp
def("stop",
&nsf::PredictiveStreamSubscriber::stop)
```

### NativePredictiveStreamSubscriber · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7147)

```cpp
def("status",
&nsf::PredictiveStreamSubscriber::status)
```

### NativePredictiveStreamSubscriber · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7146)

```cpp
def("start",
&nsf::PredictiveStreamSubscriber::start)
```

### ExecutionLeaseState · "EXPIRED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7156)

```cpp
value("EXPIRED",
nsf::ExecutionLeaseState::Expired)
```

### ExecutionLeaseState · "RELEASED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7155)

```cpp
value("RELEASED",
nsf::ExecutionLeaseState::Released)
```

### ExecutionLeaseState · "ABORTED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7154)

```cpp
value("ABORTED",
nsf::ExecutionLeaseState::Aborted)
```

### ExecutionLeaseState · "EXECUTING"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7153)

```cpp
value("EXECUTING",
nsf::ExecutionLeaseState::Executing)
```

### ExecutionLeaseState · "COMMITTED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7152)

```cpp
value("COMMITTED",
nsf::ExecutionLeaseState::Committed)
```

### ExecutionLeaseState · "PREPARED"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7151)

```cpp
value("PREPARED",
nsf::ExecutionLeaseState::Prepared)
```

### GenericExecutionLease · "idempotency_key"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7182)

```cpp
def_readwrite("idempotency_key",
&nsf::GenericExecutionLease::idempotencyKey)
```

### GenericExecutionLease · "execution_deadline_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7180)

```cpp
def_readwrite("execution_deadline_ms",
&nsf::GenericExecutionLease::executionDeadlineMs)
```

### GenericExecutionLease · "expires_at_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7179)

```cpp
def_readwrite("expires_at_ms",
&nsf::GenericExecutionLease::expiresAtMs)
```

### GenericExecutionLease · "state"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7178)

```cpp
def_readwrite("state",
&nsf::GenericExecutionLease::state)
```

### GenericExecutionLease · "conflict_keys"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7177)

```cpp
def_readwrite("conflict_keys",
&nsf::GenericExecutionLease::conflictKeys)
```

### GenericExecutionLease · "resource_binding_proof"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7170)

```cpp
def_property("resource_binding_proof",
[] (const nsf::GenericExecutionLease& lease) { implementation omitted },
[] (nsf::GenericExecutionLease& lease, const py::bytes& value) { implementation omitted })
```

### GenericExecutionLease · "resource_binding_schema"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7168)

```cpp
def_readwrite("resource_binding_schema",
&nsf::GenericExecutionLease::resourceBindingSchema)
```

### GenericExecutionLease · "plan_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7167)

```cpp
def_readwrite("plan_digest",
&nsf::GenericExecutionLease::planDigest)
```

### GenericExecutionLease · "service_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7166)

```cpp
def_readwrite("service_name",
&nsf::GenericExecutionLease::serviceName)
```

### GenericExecutionLease · "request_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7165)

```cpp
def_readwrite("request_id",
&nsf::GenericExecutionLease::requestId)
```

### GenericExecutionLease · "requester_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7164)

```cpp
def_readwrite("requester_name",
&nsf::GenericExecutionLease::requesterName)
```

### GenericExecutionLease · "provider_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7163)

```cpp
def_readwrite("provider_epoch",
&nsf::GenericExecutionLease::providerEpoch)
```

### GenericExecutionLease · "provider_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7162)

```cpp
def_readwrite("provider_name",
&nsf::GenericExecutionLease::providerName)
```

### GenericExecutionLease · "lease_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7161)

```cpp
def_readwrite("lease_id",
&nsf::GenericExecutionLease::leaseId)
```

### GenericExecutionLease · "schema"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7160)

```cpp
def_readwrite("schema",
&nsf::GenericExecutionLease::schema)
```

### ExecutionLeaseBinding · "resource_binding_proof"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7192)

```cpp
def_property("resource_binding_proof",
[] (const nsf::ExecutionLeaseBinding& binding) { implementation omitted },
[] (nsf::ExecutionLeaseBinding& binding, const py::bytes& value) { implementation omitted })
```

### ExecutionLeaseBinding · "resource_binding_schema"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7190)

```cpp
def_readwrite("resource_binding_schema",
&nsf::ExecutionLeaseBinding::resourceBindingSchema)
```

### ExecutionLeaseBinding · "plan_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7189)

```cpp
def_readwrite("plan_digest",
&nsf::ExecutionLeaseBinding::planDigest)
```

### ExecutionLeaseBinding · "service_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7188)

```cpp
def_readwrite("service_name",
&nsf::ExecutionLeaseBinding::serviceName)
```

### ExecutionLeaseBinding · "request_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7187)

```cpp
def_readwrite("request_id",
&nsf::ExecutionLeaseBinding::requestId)
```

### ExecutionLeaseBinding · "requester_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7186)

```cpp
def_readwrite("requester_name",
&nsf::ExecutionLeaseBinding::requesterName)
```

### ExecutionLeaseResult · "idempotent_replay"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7206)

```cpp
def_readonly("idempotent_replay",
&nsf::ExecutionLeaseResult::idempotentReplay)
```

### ExecutionLeaseResult · "retry_after_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7205)

```cpp
def_readonly("retry_after_ms",
&nsf::ExecutionLeaseResult::retryAfterMs)
```

### ExecutionLeaseResult · "lease"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7204)

```cpp
def_readonly("lease",
&nsf::ExecutionLeaseResult::lease)
```

### ExecutionLeaseResult · "reason_code"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7203)

```cpp
def_readonly("reason_code",
&nsf::ExecutionLeaseResult::reasonCode)
```

### ExecutionLeaseResult · "operation"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7202)

```cpp
def_readonly("operation",
&nsf::ExecutionLeaseResult::operation)
```

### ExecutionLeaseResult · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7201)

```cpp
def_readonly("status",
&nsf::ExecutionLeaseResult::status)
```

### ExecutionLeaseCounters · "active_executing"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7226)

```cpp
def_readonly("active_executing",
&nsf::ExecutionLeaseCounters::activeExecuting)
```

### ExecutionLeaseCounters · "active_committed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7225)

```cpp
def_readonly("active_committed",
&nsf::ExecutionLeaseCounters::activeCommitted)
```

### ExecutionLeaseCounters · "active_prepared"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7224)

```cpp
def_readonly("active_prepared",
&nsf::ExecutionLeaseCounters::activePrepared)
```

### ExecutionLeaseCounters · "rejected_by_reason"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7222)

```cpp
def_readonly("rejected_by_reason",
&nsf::ExecutionLeaseCounters::rejectedByReason)
```

### ExecutionLeaseCounters · "cleanup_timeout"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7221)

```cpp
def_readonly("cleanup_timeout",
&nsf::ExecutionLeaseCounters::cleanupTimeout)
```

### ExecutionLeaseCounters · "stale_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7220)

```cpp
def_readonly("stale_epoch",
&nsf::ExecutionLeaseCounters::staleEpoch)
```

### ExecutionLeaseCounters · "conflict"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7219)

```cpp
def_readonly("conflict",
&nsf::ExecutionLeaseCounters::conflict)
```

### ExecutionLeaseCounters · "idempotent_replay"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7217)

```cpp
def_readonly("idempotent_replay",
&nsf::ExecutionLeaseCounters::idempotentReplay)
```

### ExecutionLeaseCounters · "renewed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7216)

```cpp
def_readonly("renewed",
&nsf::ExecutionLeaseCounters::renewed)
```

### ExecutionLeaseCounters · "expired"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7215)

```cpp
def_readonly("expired",
&nsf::ExecutionLeaseCounters::expired)
```

### ExecutionLeaseCounters · "released"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7214)

```cpp
def_readonly("released",
&nsf::ExecutionLeaseCounters::released)
```

### ExecutionLeaseCounters · "aborted"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7213)

```cpp
def_readonly("aborted",
&nsf::ExecutionLeaseCounters::aborted)
```

### ExecutionLeaseCounters · "activated"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7212)

```cpp
def_readonly("activated",
&nsf::ExecutionLeaseCounters::activated)
```

### ExecutionLeaseCounters · "committed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7211)

```cpp
def_readonly("committed",
&nsf::ExecutionLeaseCounters::committed)
```

### ExecutionLeaseCounters · "prepared"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7210)

```cpp
def_readonly("prepared",
&nsf::ExecutionLeaseCounters::prepared)
```

### ProviderExecutionLeaseTable · "counters"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7272)

```cpp
def("counters",
&nsf::ProviderExecutionLeaseTable::counters,
py::arg("now_ms"))
```

### ProviderExecutionLeaseTable · "has_pinned_binding_proof"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7266)

```cpp
def("has_pinned_binding_proof",
[] (nsf::ProviderExecutionLeaseTable& table,
             const py::bytes& proof, uint64_t nowMs) { implementation omitted },
py::arg("resource_binding_proof"),
py::arg("now_ms"))
```

### ProviderExecutionLeaseTable · "has_active_conflict_key"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7263)

```cpp
def("has_active_conflict_key",
&nsf::ProviderExecutionLeaseTable::hasActiveConflictKey,
py::arg("conflict_key"),
py::arg("now_ms"))
```

### ProviderExecutionLeaseTable · "find"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7261)

```cpp
def("find",
&nsf::ProviderExecutionLeaseTable::find,
py::arg("lease_id"))
```

### ProviderExecutionLeaseTable · "cleanup_expired"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7259)

```cpp
def("cleanup_expired",
&nsf::ProviderExecutionLeaseTable::cleanupExpired,
py::arg("now_ms"))
```

### ProviderExecutionLeaseTable · "release"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7255)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7250)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7246)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7243)

```cpp
def("validate",
&nsf::ProviderExecutionLeaseTable::validate,
py::arg("lease_id"),
py::arg("provider_epoch"),
py::arg("binding"),
py::arg("now_ms"))
```

### ProviderExecutionLeaseTable · "validate_and_activate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7238)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7234)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7232)

```cpp
def("prepare",
&nsf::ProviderExecutionLeaseTable::prepare,
py::arg("lease"),
py::arg("now_ms"))
```

### ProviderExecutionLeaseTable · "provider_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7230)

```cpp
def_property_readonly("provider_epoch",
&nsf::ProviderExecutionLeaseTable::providerEpoch)
```

### m · "encode_large_data_reference_payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7275)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7299)

```cpp
def("parse_large_data_reference_payload",
[](const py::bytes& payload) -> py::object { implementation omitted },
py::arg("payload"))
```

### ServiceResponse · "wire_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7317)

```cpp
def_readwrite("wire_digest",
&PyServiceResponse::wireDigest)
```

### ServiceResponse · "signer_certificate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7316)

```cpp
def_readwrite("signer_certificate",
&PyServiceResponse::signerCertificate)
```

### ServiceResponse · "data_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7315)

```cpp
def_readwrite("data_name",
&PyServiceResponse::dataName)
```

### ServiceResponse · "request_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7314)

```cpp
def_readwrite("request_id",
&PyServiceResponse::requestId)
```

### ServiceResponse · "error"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7313)

```cpp
def_readwrite("error",
&PyServiceResponse::error)
```

### ServiceResponse · "payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7312)

```cpp
def_readwrite("payload",
&PyServiceResponse::payload)
```

### ServiceResponse · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7311)

```cpp
def_readwrite("status",
&PyServiceResponse::status)
```

### AckDecision · "pending_state_ttl_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7327)

```cpp
def_readwrite("pending_state_ttl_ms",
&PyAckDecision::pendingStateTtlMs)
```

### AckDecision · "selection_input_key_offer"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7326)

```cpp
def_readwrite("selection_input_key_offer",
&PyAckDecision::selectionInputKeyOffer)
```

### AckDecision · "reservation_lease"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7325)

```cpp
def_readwrite("reservation_lease",
&PyAckDecision::reservationLease)
```

### AckDecision · "suppress"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7324)

```cpp
def_readwrite("suppress",
&PyAckDecision::suppress)
```

### AckDecision · "message"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7323)

```cpp
def_readwrite("message",
&PyAckDecision::message)
```

### AckDecision · "payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7322)

```cpp
def_readwrite("payload",
&PyAckDecision::payload)
```

### AckDecision · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7321)

```cpp
def_readwrite("status",
&PyAckDecision::status)
```

### AckCandidate · "trust_schema_validated"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7343)

```cpp
def_readwrite("trust_schema_validated",
&PyAckCandidate::trustSchemaValidated)
```

### AckCandidate · "validated_wire_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7342)

```cpp
def_readwrite("validated_wire_digest",
&PyAckCandidate::validatedWireDigest)
```

### AckCandidate · "signer_key_locator"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7341)

```cpp
def_readwrite("signer_key_locator",
&PyAckCandidate::signerKeyLocator)
```

### AckCandidate · "signer_identity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7340)

```cpp
def_readwrite("signer_identity",
&PyAckCandidate::signerIdentity)
```

### AckCandidate · "selection_input_key_offer"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7338)

```cpp
def_readwrite("selection_input_key_offer",
&PyAckCandidate::selectionInputKeyOffer)
```

### AckCandidate · "telemetry"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7337)

```cpp
def_readwrite("telemetry",
&PyAckCandidate::telemetry)
```

### AckCandidate · "payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7336)

```cpp
def_readwrite("payload",
&PyAckCandidate::payload)
```

### AckCandidate · "message"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7335)

```cpp
def_readwrite("message",
&PyAckCandidate::message)
```

### AckCandidate · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7334)

```cpp
def_readwrite("status",
&PyAckCandidate::status)
```

### AckCandidate · "request_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7333)

```cpp
def_readwrite("request_id",
&PyAckCandidate::requestId)
```

### AckCandidate · "service_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7332)

```cpp
def_readwrite("service_name",
&PyAckCandidate::serviceName)
```

### AckCandidate · "provider_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7331)

```cpp
def_readwrite("provider_name",
&PyAckCandidate::providerName)
```

### CollaborationAckClosure · "request_deadline_us"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7351)

```cpp
def_readwrite("request_deadline_us",
&PyCollaborationAckClosure::requestDeadlineUs)
```

### CollaborationAckClosure · "closed_at_us"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7350)

```cpp
def_readwrite("closed_at_us",
&PyCollaborationAckClosure::closedAtUs)
```

### CollaborationAckClosure · "digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7349)

```cpp
def_readwrite("digest",
&PyCollaborationAckClosure::digest)
```

### CollaborationAckClosure · "candidates"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7348)

```cpp
def_readwrite("candidates",
&PyCollaborationAckClosure::candidates)
```

### CollaborationAckClosure · "request_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7347)

```cpp
def_readwrite("request_id",
&PyCollaborationAckClosure::requestId)
```

### LargeDataPublishResult · "error"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7365)

```cpp
def_readwrite("error",
&PyLargeDataPublishResult::error)
```

### LargeDataPublishResult · "encrypted"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7364)

```cpp
def_readwrite("encrypted",
&PyLargeDataPublishResult::encrypted)
```

### LargeDataPublishResult · "protection_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7363)

```cpp
def_readwrite("protection_epoch",
&PyLargeDataPublishResult::protectionEpoch)
```

### LargeDataPublishResult · "authorization_scope"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7362)

```cpp
def_readwrite("authorization_scope",
&PyLargeDataPublishResult::authorizationScope)
```

### LargeDataPublishResult · "manifest_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7361)

```cpp
def_readwrite("manifest_digest",
&PyLargeDataPublishResult::manifestDigest)
```

### LargeDataPublishResult · "content_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7360)

```cpp
def_readwrite("content_digest",
&PyLargeDataPublishResult::contentDigest)
```

### LargeDataPublishResult · "plaintext_size"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7359)

```cpp
def_readwrite("plaintext_size",
&PyLargeDataPublishResult::plaintextSize)
```

### LargeDataPublishResult · "object_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7358)

```cpp
def_readwrite("object_id",
&PyLargeDataPublishResult::objectId)
```

### LargeDataPublishResult · "encrypted_data_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7357)

```cpp
def_readwrite("encrypted_data_name",
&PyLargeDataPublishResult::encryptedDataName)
```

### LargeDataPublishResult · "success"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7356)

```cpp
def_readwrite("success",
&PyLargeDataPublishResult::success)
```

### SignedAppDataResult · "error"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7373)

```cpp
def_readwrite("error",
&PySignedAppDataResult::error)
```

### SignedAppDataResult · "payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7372)

```cpp
def_readwrite("payload",
&PySignedAppDataResult::payload)
```

### SignedAppDataResult · "signer_certificate"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7371)

```cpp
def_readwrite("signer_certificate",
&PySignedAppDataResult::signerCertificate)
```

### SignedAppDataResult · "data_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7370)

```cpp
def_readwrite("data_name",
&PySignedAppDataResult::dataName)
```

### SignedAppDataResult · "success"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7369)

```cpp
def_readwrite("success",
&PySignedAppDataResult::success)
```

### CollaborationAssignment · "role_providers"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7385)

```cpp
def_readwrite("role_providers",
&PyCollaborationAssignment::roleProviders)
```

### CollaborationAssignment · "assignment_payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7384)

```cpp
def_readwrite("assignment_payload",
&PyCollaborationAssignment::assignmentPayload)
```

### CollaborationAssignment · "selection_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7383)

```cpp
def_readwrite("selection_digest",
&PyCollaborationAssignment::selectionDigest)
```

### CollaborationAssignment · "provisioning_timeout_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7382)

```cpp
def_readwrite("provisioning_timeout_ms",
&PyCollaborationAssignment::provisioningTimeoutMs)
```

### CollaborationAssignment · "requires_provisioning"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7381)

```cpp
def_readwrite("requires_provisioning",
&PyCollaborationAssignment::requiresProvisioning)
```

### CollaborationAssignment · "artifact_data_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7380)

```cpp
def_readwrite("artifact_data_name",
&PyCollaborationAssignment::artifactDataName)
```

### CollaborationAssignment · "assigned_artifact"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7379)

```cpp
def_readwrite("assigned_artifact",
&PyCollaborationAssignment::assignedArtifact)
```

### CollaborationAssignment · "service"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7378)

```cpp
def_readwrite("service",
&PyCollaborationAssignment::service)
```

### CollaborationAssignment · "role"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7377)

```cpp
def_readwrite("role",
&PyCollaborationAssignment::role)
```

### CollaborationData · "payload"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7395)

```cpp
def_readwrite("payload",
&PyCollaborationData::payload)
```

### CollaborationData · "sequence"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7394)

```cpp
def_readwrite("sequence",
&PyCollaborationData::sequence)
```

### CollaborationData · "producer_role"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7393)

```cpp
def_readwrite("producer_role",
&PyCollaborationData::producerRole)
```

### CollaborationData · "producer"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7392)

```cpp
def_readwrite("producer",
&PyCollaborationData::producer)
```

### CollaborationData · "topic"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7391)

```cpp
def_readwrite("topic",
&PyCollaborationData::topic)
```

### CollaborationData · "key_scope"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7390)

```cpp
def_readwrite("key_scope",
&PyCollaborationData::keyScope)
```

### CollaborationData · "session_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7389)

```cpp
def_readwrite("session_id",
&PyCollaborationData::sessionId)
```

### SegmentedObjectProducer · "error"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7413)

```cpp
def_property_readonly("error",
&NativeSegmentedObjectProducer::error)
```

### SegmentedObjectProducer · "segment_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7412)

```cpp
def_property_readonly("segment_count",
&NativeSegmentedObjectProducer::segmentCount)
```

### SegmentedObjectProducer · "versioned_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7411)

```cpp
def_property_readonly("versioned_name",
&NativeSegmentedObjectProducer::versionedName)
```

### SegmentedObjectProducer · "base_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7410)

```cpp
def_property_readonly("base_name",
&NativeSegmentedObjectProducer::baseName)
```

### SegmentedObjectProducer · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7409)

```cpp
def("stop",
&NativeSegmentedObjectProducer::stop)
```

### SegmentedObjectProducer · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7408)

```cpp
def("start",
&NativeSegmentedObjectProducer::start)
```

### FileSegmentedObjectProducer · "error"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7438)

```cpp
def_property_readonly("error",
&NativeFileSegmentedObjectProducer::error)
```

### FileSegmentedObjectProducer · "public_key_der"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7437)

```cpp
def_property_readonly("public_key_der",
&NativeFileSegmentedObjectProducer::publicKeyDer)
```

### FileSegmentedObjectProducer · "signing_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7436)

```cpp
def_property_readonly("signing_ms",
&NativeFileSegmentedObjectProducer::signingMs)
```

### FileSegmentedObjectProducer · "wire_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7435)

```cpp
def_property_readonly("wire_bytes",
&NativeFileSegmentedObjectProducer::wireBytes)
```

### FileSegmentedObjectProducer · "data_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7434)

```cpp
def_property_readonly("data_count",
&NativeFileSegmentedObjectProducer::dataCount)
```

### FileSegmentedObjectProducer · "file_size"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7433)

```cpp
def_property_readonly("file_size",
&NativeFileSegmentedObjectProducer::fileSize)
```

### FileSegmentedObjectProducer · "segment_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7432)

```cpp
def_property_readonly("segment_count",
&NativeFileSegmentedObjectProducer::segmentCount)
```

### FileSegmentedObjectProducer · "versioned_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7431)

```cpp
def_property_readonly("versioned_name",
&NativeFileSegmentedObjectProducer::versionedName)
```

### FileSegmentedObjectProducer · "base_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7430)

```cpp
def_property_readonly("base_name",
&NativeFileSegmentedObjectProducer::baseName)
```

### FileSegmentedObjectProducer · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7429)

```cpp
def("stop",
&NativeFileSegmentedObjectProducer::stop)
```

### FileSegmentedObjectProducer · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7428)

```cpp
def("start",
&NativeFileSegmentedObjectProducer::start)
```

### DataPacket · "content"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7445)

```cpp
def_readwrite("content",
&PyDataPacket::content)
```

### DataPacket · "wire"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7444)

```cpp
def_readwrite("wire",
&PyDataPacket::wire)
```

### DataPacket · "segment"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7443)

```cpp
def_readwrite("segment",
&PyDataPacket::segment)
```

### DataPacket · "name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7442)

```cpp
def_readwrite("name",
&PyDataPacket::name)
```

### m · "verify_data_packet_signature"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7447)

```cpp
def("verify_data_packet_signature",
&verifyDataPacketSignature,
py::arg("wire"),
py::arg("public_key_der"))
```

### m · "verify_detached_sha256_signature"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7449)

```cpp
def("verify_detached_sha256_signature",
&verifyDetachedSha256Signature,
py::arg("payload"),
py::arg("signature"),
py::arg("public_key_der"))
```

### m · "verify_data_packet_digest"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7451)

```cpp
def("verify_data_packet_digest",
&verifyDataPacketDigest,
py::arg("wire"))
```

### SegmentHintRange · "forwarding_hints"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7458)

```cpp
def_readwrite("forwarding_hints",
&PySegmentHintRange::forwardingHints)
```

### SegmentHintRange · "end"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7457)

```cpp
def_readwrite("end",
&PySegmentHintRange::end)
```

### SegmentHintRange · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7456)

```cpp
def_readwrite("start",
&PySegmentHintRange::start)
```

### StoredDataProducer · "error"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7472)

```cpp
def_property_readonly("error",
&NativeWireDataProducer::error)
```

### StoredDataProducer · "segment_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7471)

```cpp
def_property_readonly("segment_count",
&NativeWireDataProducer::segmentCount)
```

### StoredDataProducer · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7470)

```cpp
def("stop",
&NativeWireDataProducer::stop)
```

### StoredDataProducer · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7469)

```cpp
def("start",
&NativeWireDataProducer::start)
```

### RepoDataPlaneProducer · "error"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7491)

```cpp
def_property_readonly("error",
&NativeRepoDataPlaneProducer::error)
```

### RepoDataPlaneProducer · "thread_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7490)

```cpp
def_property_readonly("thread_count",
&NativeRepoDataPlaneProducer::threadCount)
```

### RepoDataPlaneProducer · "miss_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7489)

```cpp
def_property_readonly("miss_count",
&NativeRepoDataPlaneProducer::missCount)
```

### RepoDataPlaneProducer · "hit_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7488)

```cpp
def_property_readonly("hit_count",
&NativeRepoDataPlaneProducer::hitCount)
```

### RepoDataPlaneProducer · "interest_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7486)

```cpp
def_property_readonly("interest_count",
&NativeRepoDataPlaneProducer::interestCount)
```

### RepoDataPlaneProducer · "active_prefix_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7484)

```cpp
def_property_readonly("active_prefix_count",
&NativeRepoDataPlaneProducer::activePrefixCount)
```

### RepoDataPlaneProducer · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7483)

```cpp
def("stop",
&NativeRepoDataPlaneProducer::stop)
```

### RepoDataPlaneProducer · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7482)

```cpp
def("start",
&NativeRepoDataPlaneProducer::start)
```

### RepoDataPlaneProducer · "activate_prefix"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7481)

```cpp
def("activate_prefix",
&NativeRepoDataPlaneProducer::activatePrefix)
```

### m · "make_segmented_data_packets"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7493)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7501)

```cpp
def("make_signed_data",
&makeSignedData,
py::arg("name"),
py::arg("content"),
py::arg("signing_identity") = "",
py::arg("freshness_ms") = 300)
```

### m · "make_predictive_data_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7508)

```cpp
def("make_predictive_data_name",
&makePredictiveDataNameUri,
py::arg("mapping_root"),
py::arg("mapping_version"),
py::arg("sequence"))
```

### m · "wrap_selection_gated_input_key"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7514)

```cpp
def("wrap_selection_gated_input_key",
[] (const py::bytes& key, const py::bytes& recipientPublicKey) { implementation omitted },
py::arg("key"),
py::arg("recipient_public_key"))
```

### m · "decode_data_packet"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7522)

```cpp
def("decode_data_packet",
&decodeDataPacket,
py::arg("wire"))
```

### m · "fetch_segmented_data_packets"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7526)

```cpp
def("fetch_segmented_data_packets",
&fetchSegmentedDataPackets,
py::arg("base_name"),
py::arg("timeout_ms") = 30000,
py::arg("interest_lifetime_ms") = 10000,
py::arg("forwarding_hints") = std::vector<std::string>{})
```

### AdaptiveSegmentFetchResult · "final_window"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7557)

```cpp
def_readonly("final_window",
&PyAdaptiveSegmentFetchResult::finalWindow)
```

### AdaptiveSegmentFetchResult · "maximum_in_flight"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7555)

```cpp
def_readonly("maximum_in_flight",
&PyAdaptiveSegmentFetchResult::maximumInFlight)
```

### AdaptiveSegmentFetchResult · "retransmitted_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7553)

```cpp
def_readonly("retransmitted_bytes",
&PyAdaptiveSegmentFetchResult::retransmittedBytes)
```

### AdaptiveSegmentFetchResult · "wire_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7552)

```cpp
def_readonly("wire_bytes",
&PyAdaptiveSegmentFetchResult::wireBytes)
```

### AdaptiveSegmentFetchResult · "interest_wire_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7550)

```cpp
def_readonly("interest_wire_bytes",
&PyAdaptiveSegmentFetchResult::interestWireBytes)
```

### AdaptiveSegmentFetchResult · "data_wire_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7548)

```cpp
def_readonly("data_wire_bytes",
&PyAdaptiveSegmentFetchResult::dataWireBytes)
```

### AdaptiveSegmentFetchResult · "logical_bytes"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7546)

```cpp
def_readonly("logical_bytes",
&PyAdaptiveSegmentFetchResult::logicalBytes)
```

### AdaptiveSegmentFetchResult · "timeout_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7544)

```cpp
def_readonly("timeout_count",
&PyAdaptiveSegmentFetchResult::timeoutCount)
```

### AdaptiveSegmentFetchResult · "duplicate_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7542)

```cpp
def_readonly("duplicate_count",
&PyAdaptiveSegmentFetchResult::duplicateCount)
```

### AdaptiveSegmentFetchResult · "retransmission_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7540)

```cpp
def_readonly("retransmission_count",
&PyAdaptiveSegmentFetchResult::retransmissionCount)
```

### AdaptiveSegmentFetchResult · "interest_count"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7538)

```cpp
def_readonly("interest_count",
&PyAdaptiveSegmentFetchResult::interestCount)
```

### AdaptiveSegmentFetchResult · "delivered_segments"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7536)

```cpp
def_readonly("delivered_segments",
&PyAdaptiveSegmentFetchResult::deliveredSegments)
```

### AdaptiveSegmentFetchResult · "total_segments"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7534)

```cpp
def_readonly("total_segments",
&PyAdaptiveSegmentFetchResult::totalSegments)
```

### m · "fetch_adaptive_segmented_data_packets"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7560)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7572)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7584)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7630)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7639)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7646)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7719)

```cpp
def_property_readonly("stream_cancelled",
&PyCollaborationContext::streamCancelled)
```

### CollaborationContext · "fail_stream"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7717)

```cpp
def("fail_stream",
&PyCollaborationContext::failStream,
py::arg("code"),
py::arg("message"))
```

### CollaborationContext · "finish_stream"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7715)

```cpp
def("finish_stream",
&PyCollaborationContext::finishStream,
py::arg("payload"),
py::arg("reason") = 4)
```

### CollaborationContext · "publish_stream_event"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7713)

```cpp
def("publish_stream_event",
&PyCollaborationContext::publishStreamEvent,
py::arg("payload"))
```

### CollaborationContext · "is_streamed"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7712)

```cpp
def_property_readonly("is_streamed",
&PyCollaborationContext::isStreamed)
```

### CollaborationContext · "publish_final_response"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7710)

```cpp
def("publish_final_response",
&PyCollaborationContext::publishFinalResponse,
py::arg("payload"))
```

### CollaborationContext · "report_operation_status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7708)

```cpp
def("report_operation_status",
&PyCollaborationContext::reportOperationStatus,
py::arg("status"))
```

### CollaborationContext · "wait_for"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7703)

```cpp
def("wait_for",
&PyCollaborationContext::waitFor,
py::arg("key_scope"),
py::arg("topic_prefix"),
py::arg("min_count"),
py::arg("timeout_ms") = 5000)
```

### CollaborationContext · "wait_one"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7699)

```cpp
def("wait_one",
&PyCollaborationContext::waitOne,
py::arg("key_scope"),
py::arg("topic_prefix"),
py::arg("timeout_ms") = 5000)
```

### CollaborationContext · "fetch_large_exact"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7694)

```cpp
def("fetch_large_exact",
&PyCollaborationContext::fetchLargeExact,
py::arg("data_name"),
py::arg("key_scope"),
py::arg("timeout_ms") = 5000,
py::arg("expected_segments"))
```

### CollaborationContext · "fetch_large"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7690)

```cpp
def("fetch_large",
&PyCollaborationContext::fetchLarge,
py::arg("data_name"),
py::arg("key_scope"),
py::arg("timeout_ms") = 5000)
```

### CollaborationContext · "publish_large_named"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7684)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7678)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7674)

```cpp
def("publish",
&PyCollaborationContext::publish,
py::arg("key_scope"),
py::arg("topic"),
py::arg("payload"))
```

### CollaborationContext · "allow_data"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7671)

```cpp
def("allow_data",
&PyCollaborationContext::allowData,
py::arg("key_scope"),
py::arg("topic_prefix"))
```

### CollaborationContext · "fail"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7669)

```cpp
def("fail",
&PyCollaborationContext::fail,
py::arg("reason"))
```

### CollaborationContext · "fetch_encrypted_large_data"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7666)

```cpp
def("fetch_encrypted_large_data",
&PyCollaborationContext::fetchEncryptedLargeData,
py::arg("data_name"),
py::arg("service") = "")
```

### CollaborationContext · "get_artifact"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7664)

```cpp
def("get_artifact",
&PyCollaborationContext::getArtifact,
py::arg("artifact_name"))
```

### CollaborationContext · "fetch_artifact"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7661)

```cpp
def("fetch_artifact",
&PyCollaborationContext::fetchArtifact,
py::arg("artifact_name"),
py::arg("timeout_ms") = 5000)
```

### CollaborationContext · "assignment"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7660)

```cpp
def_property_readonly("assignment",
&PyCollaborationContext::assignment)
```

### CollaborationContext · "local_provider"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7659)

```cpp
def_property_readonly("local_provider",
&PyCollaborationContext::localProvider)
```

### CollaborationContext · "requester_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7658)

```cpp
def_property_readonly("requester_name",
&PyCollaborationContext::requesterName)
```

### CollaborationContext · "role"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7657)

```cpp
def_property_readonly("role",
&PyCollaborationContext::role)
```

### CollaborationContext · "session_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7656)

```cpp
def_property_readonly("session_id",
&PyCollaborationContext::sessionId)
```

### NativeServiceController · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7739)

```cpp
def("stop",
&NativeServiceController::stop)
```

### NativeServiceController · "run"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7738)

```cpp
def("run",
&NativeServiceController::run,
py::call_guard<py::gil_scoped_release>())
```

### NativeServiceController · "wait_until_ready"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7735)

```cpp
def("wait_until_ready",
&NativeServiceController::waitUntilReady,
py::call_guard<py::gil_scoped_release>(),
py::arg("timeout_ms") = 10000)
```

### NativeServiceController · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7734)

```cpp
def("start",
&NativeServiceController::start)
```

### StreamWriter · "remaining_deadline_ms"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7748)

```cpp
def_property_readonly("remaining_deadline_ms",
&PyStreamWriter::remaining_deadline_ms)
```

### StreamWriter · "cancelled"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7747)

```cpp
def_property_readonly("cancelled",
&PyStreamWriter::cancelled)
```

### StreamWriter · "fail"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7745)

```cpp
def("fail",
&PyStreamWriter::fail,
py::arg("code"),
py::arg("message"))
```

### StreamWriter · "finish_stream"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7743)

```cpp
def("finish_stream",
&PyStreamWriter::finish,
py::arg("payload") = py::bytes(),
py::arg("reason") = 4)
```

### StreamWriter · "publish_event"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7742)

```cpp
def("publish_event",
&PyStreamWriter::publish,
py::arg("payload"))
```

### NativeStreamedInvocationHandle · "cancel"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7755)

```cpp
def("cancel",
&PyStreamedInvocationHandle::cancel)
```

### NativeStreamedInvocationHandle · "metrics"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7754)

```cpp
def_property_readonly("metrics",
&PyStreamedInvocationHandle::metrics)
```

### NativeStreamedInvocationHandle · "status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7753)

```cpp
def_property_readonly("status",
&PyStreamedInvocationHandle::status)
```

### NativeStreamedInvocationHandle · "request_id"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7752)

```cpp
def_property_readonly("request_id",
&PyStreamedInvocationHandle::requestId)
```

### NativeServiceProvider · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7844)

```cpp
def("stop",
&NativeServiceProvider::stop)
```

### NativeServiceProvider · "wait_until_ready"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7842)

```cpp
def("wait_until_ready",
&NativeServiceProvider::waitUntilReady,
py::arg("timeout_ms") = 15000)
```

### NativeServiceProvider · "run"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7841)

```cpp
def("run",
&NativeServiceProvider::run,
py::call_guard<py::gil_scoped_release>())
```

### NativeServiceProvider · "create_stream"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7839)

```cpp
def("create_stream",
&NativeServiceProvider::createStream,
py::arg("config"))
```

### NativeServiceProvider · "create_live_stream"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7837)

```cpp
def("create_live_stream",
&NativeServiceProvider::createLiveStream,
py::arg("definition"))
```

### NativeServiceProvider · "start_ndnsd_periodic_publish"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7835)

```cpp
def("start_ndnsd_periodic_publish",
&NativeServiceProvider::startNdnsdPeriodicPublish,
py::arg("interval_seconds"))
```

### NativeServiceProvider · "set_ndnsd_meta"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7833)

```cpp
def("set_ndnsd_meta",
&NativeServiceProvider::setNdnsdMeta,
py::arg("meta"))
```

### NativeServiceProvider · "update_ndnsd_meta"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7831)

```cpp
def("update_ndnsd_meta",
&NativeServiceProvider::updateNdnsdMeta,
py::arg("key"),
py::arg("value"))
```

### NativeServiceProvider · "publish_service_info"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7829)

```cpp
def("publish_service_info",
&NativeServiceProvider::publishServiceInfo,
py::arg("service_name"),
py::arg("service_lifetime_seconds"),
py::arg("meta_info") = py::dict())
```

### NativeServiceProvider · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7828)

```cpp
def("start",
&NativeServiceProvider::start)
```

### NativeServiceProvider · "add_collaboration_service"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7822)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7819)

```cpp
def("set_r1_reservation_terminal_handler",
&NativeServiceProvider::setR1ReservationTerminalHandler,
py::arg("service"),
py::arg("handler"))
```

### NativeServiceProvider · "set_r1_selection_decision_handler"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7816)

```cpp
def("set_r1_selection_decision_handler",
&NativeServiceProvider::setR1SelectionDecisionHandler,
py::arg("service"),
py::arg("handler"))
```

### NativeServiceProvider · "register_opaque_selection_participant"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7811)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7807)

```cpp
def("configure_opaque_selection_store",
&NativeServiceProvider::configureOpaqueSelectionStore,
py::arg("wal_path"),
py::arg("storage_key"),
py::arg("storage_key_epoch"),
py::arg("max_prepare_ms") = 1000)
```

### NativeServiceProvider · "publish_stream_packet_for_test"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7804)

```cpp
def("publish_stream_packet_for_test",
&NativeServiceProvider::publishStreamPacketForTest,
py::arg("wire"))
```

### NativeServiceProvider · "set_stream_retention_interceptor_for_test"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7801)

```cpp
def("set_stream_retention_interceptor_for_test",
&NativeServiceProvider::setStreamRetentionInterceptorForTest,
py::arg("callback"))
```

### NativeServiceProvider · "set_stream_publication_interceptor_for_test"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7798)

```cpp
def("set_stream_publication_interceptor_for_test",
&NativeServiceProvider::setStreamPublicationInterceptorForTest,
py::arg("callback"))
```

### NativeServiceProvider · "provider_signing_certificate_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7796)

```cpp
def_property_readonly("provider_signing_certificate_name",
&NativeServiceProvider::providerSigningCertificateName)
```

### NativeServiceProvider · "provider_signing_key_name"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7794)

```cpp
def_property_readonly("provider_signing_key_name",
&NativeServiceProvider::providerSigningKeyName)
```

### NativeServiceProvider · "provider_identity"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7792)

```cpp
def_property_readonly("provider_identity",
&NativeServiceProvider::providerIdentity)
```

### NativeServiceProvider · "provider_boot_epoch"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7790)

```cpp
def_property_readonly("provider_boot_epoch",
&NativeServiceProvider::providerBootEpoch)
```

### NativeServiceProvider · "set_deployment_prepare_handler"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7787)

```cpp
def("set_deployment_prepare_handler",
&NativeServiceProvider::setDeploymentPrepareHandler,
py::arg("handler"))
```

### NativeServiceProvider · "add_streaming_context_service"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7784)

```cpp
def("add_streaming_context_service",
&NativeServiceProvider::addStreamingContextService,
py::arg("service"),
py::arg("handler"))
```

### NativeServiceProvider · "add_streaming_service"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7782)

```cpp
def("add_streaming_service",
&NativeServiceProvider::addStreamingService,
py::arg("service"),
py::arg("handler"))
```

### NativeServiceProvider · "add_service"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7776)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8026)

```cpp
def("pump",
&NativeServiceUser::pump)
```

### NativeServiceUser · "get_ndnsd_services"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8025)

```cpp
def("get_ndnsd_services",
&NativeServiceUser::getNdnsdServices)
```

### NativeServiceUser · "refresh_permissions"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8024)

```cpp
def("refresh_permissions",
&NativeServiceUser::refreshPermissions)
```

### NativeServiceUser · "get_allowed_services"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8023)

```cpp
def("get_allowed_services",
&NativeServiceUser::getAllowedServices)
```

### NativeServiceUser · "stop"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8021)

```cpp
def("stop",
&NativeServiceUser::stop,
py::call_guard<py::gil_scoped_release>())
```

### NativeServiceUser · "start"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8016)

```cpp
def("start",
&NativeServiceUser::start)
```

### NativeServiceUser · "get_collaboration_status_snapshot"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8013)

```cpp
def("get_collaboration_status_snapshot",
&NativeServiceUser::getCollaborationStatusSnapshot,
py::arg("request_id"),
py::arg("timeout_ms") = 500)
```

### NativeServiceUser · "query_collaboration_status"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L8010)

```cpp
def("query_collaboration_status",
&NativeServiceUser::queryCollaborationStatus,
py::arg("provider"),
py::arg("service"),
py::arg("selection_digest"),
py::arg("timeout_ms") = 500)
```

### NativeServiceUser · "request_collaboration_async"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7995)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7991)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7988)

```cpp
def("clear_verified_collaboration_data",
&NativeServiceUser::clearVerifiedCollaborationData,
py::arg("request_id"),
py::arg("key_scope"))
```

### NativeServiceUser · "wait_for_verified_collaboration_data"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7983)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7973)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7962)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7948)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7945)

```cpp
def("fetch_signed_app_data",
&NativeServiceUser::fetchSignedAppData,
py::arg("data_name"),
py::arg("expected_signer"),
py::arg("timeout_ms") = 5000)
```

### NativeServiceUser · "publish_signed_app_data"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7942)

```cpp
def("publish_signed_app_data",
&NativeServiceUser::publishSignedAppData,
py::arg("data_name"),
py::arg("payload"),
py::arg("freshness_ms") = 60000)
```

### NativeServiceUser · "publish_encrypted_large_data"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7937)

```cpp
def("publish_encrypted_large_data",
&NativeServiceUser::publishEncryptedLargeData,
py::arg("service"),
py::arg("payload"),
py::arg("object_label") = "",
py::arg("freshness_ms") = 60000)
```

### NativeServiceUser · "stream_metrics_for_test"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7935)

```cpp
def("stream_metrics_for_test",
&NativeServiceUser::streamMetricsForTest,
py::arg("request_id"))
```

### NativeServiceUser · "cancel_stream_request"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7933)

```cpp
def("cancel_stream_request",
&NativeServiceUser::cancelStreamRequest,
py::arg("request_id"))
```

### NativeServiceUser · "request_service_streaming_handle"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7928)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7924)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7917)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7909)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7900)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7895)

```cpp
def("request_service_targeted",
&NativeServiceUser::requestServiceTargeted,
py::arg("provider"),
py::arg("service"),
py::arg("payload"),
py::arg("timeout_ms") = 5000)
```

### NativeServiceUser · "request_service"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7886)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7877)

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

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7869)

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

### NativeServiceUser · "native_inference_client"

[源码](../../pythonWrapper/src/ndnsf/_ndnsf.cpp#L7867)

```cpp
def("native_inference_client",
&NativeServiceUser::nativeInferenceClient,
py::arg("adapters"),
py::keep_alive<0, 1>())
```

## pythonWrapper/src/ndnsf/di_bindings.cpp

SHA-256：`58847625fc73fca2760ec96205743b1a0ce5fc131fc2fd91b4ee5f2a650e92c3`。

### NativeRequestStatus · "CANCELLED"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L38)

```cpp
value("CANCELLED",
di::NativeRequestStatus::Cancelled)
```

### NativeRequestStatus · "FAILED"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L37)

```cpp
value("FAILED",
di::NativeRequestStatus::Failed)
```

### NativeRequestStatus · "SUCCEEDED"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L36)

```cpp
value("SUCCEEDED",
di::NativeRequestStatus::Succeeded)
```

### NativeRequestStatus · "PENDING"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L35)

```cpp
value("PENDING",
di::NativeRequestStatus::Pending)
```

### NativeInputTransportMode · "REPOSITORY_REFERENCE"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L43)

```cpp
value("REPOSITORY_REFERENCE",
di::NativeInputTransportMode::RepositoryReference)
```

### NativeInputTransportMode · "INLINE"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L42)

```cpp
value("INLINE",
di::NativeInputTransportMode::Inline)
```

### NativeAdapterDescriptor · "descriptor_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L66)

```cpp
def_property_readonly("descriptor_digest",
&di::NativeAdapterDescriptor::descriptorDigest)
```

### NativeAdapterDescriptor · "canonical_json"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L65)

```cpp
def("canonical_json",
&di::NativeAdapterDescriptor::canonicalJson)
```

### NativeAdapterDescriptor · "deterministic_analysis"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L64)

```cpp
def_readwrite("deterministic_analysis",
&di::NativeAdapterDescriptor::deterministicAnalysis)
```

### NativeAdapterDescriptor · "splittable"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L63)

```cpp
def_readwrite("splittable",
&di::NativeAdapterDescriptor::splittable)
```

### NativeAdapterDescriptor · "graph_inspectable"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L62)

```cpp
def_readwrite("graph_inspectable",
&di::NativeAdapterDescriptor::graphInspectable)
```

### NativeAdapterDescriptor · "state_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L61)

```cpp
def_readwrite("state_schema_digest",
&di::NativeAdapterDescriptor::stateSchemaDigest)
```

### NativeAdapterDescriptor · "split_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L60)

```cpp
def_readwrite("split_schema_digest",
&di::NativeAdapterDescriptor::splitSchemaDigest)
```

### NativeAdapterDescriptor · "graph_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L59)

```cpp
def_readwrite("graph_schema_digest",
&di::NativeAdapterDescriptor::graphSchemaDigest)
```

### NativeAdapterDescriptor · "result_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L58)

```cpp
def_readwrite("result_schema_digest",
&di::NativeAdapterDescriptor::resultSchemaDigest)
```

### NativeAdapterDescriptor · "options_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L57)

```cpp
def_readwrite("options_schema_digest",
&di::NativeAdapterDescriptor::optionsSchemaDigest)
```

### NativeAdapterDescriptor · "input_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L56)

```cpp
def_readwrite("input_schema_digest",
&di::NativeAdapterDescriptor::inputSchemaDigest)
```

### NativeAdapterDescriptor · "precisions"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L55)

```cpp
def_readwrite("precisions",
&di::NativeAdapterDescriptor::precisions)
```

### NativeAdapterDescriptor · "backends"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L54)

```cpp
def_readwrite("backends",
&di::NativeAdapterDescriptor::backends)
```

### NativeAdapterDescriptor · "tasks"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L53)

```cpp
def_readwrite("tasks",
&di::NativeAdapterDescriptor::tasks)
```

### NativeAdapterDescriptor · "model_formats"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L52)

```cpp
def_readwrite("model_formats",
&di::NativeAdapterDescriptor::modelFormats)
```

### NativeAdapterDescriptor · "abi"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L51)

```cpp
def_readwrite("abi",
&di::NativeAdapterDescriptor::abi)
```

### NativeAdapterDescriptor · "state_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L50)

```cpp
def_readwrite("state_digest",
&di::NativeAdapterDescriptor::stateDigest)
```

### NativeAdapterDescriptor · "version"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L49)

```cpp
def_readwrite("version",
&di::NativeAdapterDescriptor::version)
```

### NativeAdapterDescriptor · "name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L48)

```cpp
def_readwrite("name",
&di::NativeAdapterDescriptor::name)
```

### NativeModelDescriptor · "model_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L81)

```cpp
def_property_readonly("model_digest",
&di::NativeModelDescriptor::modelDigest)
```

### NativeModelDescriptor · "canonical_json"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L80)

```cpp
def("canonical_json",
&di::NativeModelDescriptor::canonicalJson)
```

### NativeModelDescriptor · "source_revision"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L79)

```cpp
def_readwrite("source_revision",
&di::NativeModelDescriptor::sourceRevision)
```

### NativeModelDescriptor · "adapter"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L78)

```cpp
def_readwrite("adapter",
&di::NativeModelDescriptor::adapter)
```

### NativeModelDescriptor · "adapter_version"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L77)

```cpp
def_readwrite("adapter_version",
&di::NativeModelDescriptor::adapterVersion)
```

### NativeModelDescriptor · "adapter_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L76)

```cpp
def_readwrite("adapter_id",
&di::NativeModelDescriptor::adapterId)
```

### NativeModelDescriptor · "precision"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L75)

```cpp
def_readwrite("precision",
&di::NativeModelDescriptor::precision)
```

### NativeModelDescriptor · "model_format"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L74)

```cpp
def_readwrite("model_format",
&di::NativeModelDescriptor::modelFormat)
```

### NativeModelDescriptor · "graph_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L73)

```cpp
def_readwrite("graph_digest",
&di::NativeModelDescriptor::graphDigest)
```

### NativeModelDescriptor · "semantics_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L72)

```cpp
def_readwrite("semantics_digest",
&di::NativeModelDescriptor::semanticsDigest)
```

### NativeModelDescriptor · "content_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L71)

```cpp
def_readwrite("content_digest",
&di::NativeModelDescriptor::contentDigest)
```

### NativeModelDescriptor · "model_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L70)

```cpp
def_readwrite("model_name",
&di::NativeModelDescriptor::modelName)
```

### NativeApplicationInput · "repository_reference"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L94)

```cpp
def_readwrite("repository_reference",
&di::NativeApplicationInput::repositoryReference)
```

### NativeApplicationInput · "transport_mode"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L93)

```cpp
def_readwrite("transport_mode",
&di::NativeApplicationInput::transportMode)
```

### NativeApplicationInput · "options"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L92)

```cpp
def_readwrite("options",
&di::NativeApplicationInput::options)
```

### NativeApplicationInput · "payload"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L91)

```cpp
def_readwrite("payload",
&di::NativeApplicationInput::payload)
```

### NativeApplicationInput · "options_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L90)

```cpp
def_readwrite("options_schema_digest",
&di::NativeApplicationInput::optionsSchemaDigest)
```

### NativeApplicationInput · "input_schema_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L89)

```cpp
def_readwrite("input_schema_digest",
&di::NativeApplicationInput::inputSchemaDigest)
```

### NativeApplicationInput · "task_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L88)

```cpp
def_readwrite("task_name",
&di::NativeApplicationInput::taskName)
```

### NativeRequestOptions · "output_mode"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L101)

```cpp
def_readwrite("output_mode",
&di::NativeRequestOptions::outputMode)
```

### NativeRequestOptions · "task_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L100)

```cpp
def_readwrite("task_name",
&di::NativeRequestOptions::taskName)
```

### NativeRequestOptions · "ack_timeout_ms"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L99)

```cpp
def_readwrite("ack_timeout_ms",
&di::NativeRequestOptions::ackTimeoutMs)
```

### NativeRequestOptions · "timeout_ms"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L98)

```cpp
def_readwrite("timeout_ms",
&di::NativeRequestOptions::timeoutMs)
```

### NativeInferenceResult · "plan_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L107)

```cpp
def_readonly("plan_digest",
&di::NativeInferenceResult::planDigest)
```

### NativeInferenceResult · "model_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L106)

```cpp
def_readonly("model_digest",
&di::NativeInferenceResult::modelDigest)
```

### NativeInferenceResult · "payload"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L105)

```cpp
def_readonly("payload",
&di::NativeInferenceResult::payload)
```

### NativeInferenceEvent · "terminal"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L112)

```cpp
def_readonly("terminal",
&di::NativeInferenceEvent::terminal)
```

### NativeInferenceEvent · "payload"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L111)

```cpp
def_readonly("payload",
&di::NativeInferenceEvent::payload)
```

### NativeInferenceEvent · "request_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L110)

```cpp
def_readonly("request_id",
&di::NativeInferenceEvent::requestId)
```

### NativeAdapterRegistry · "frozen"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L119)

```cpp
def_property_readonly("frozen",
&di::NativeAdapterRegistry::frozen)
```

### NativeAdapterRegistry · "find"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L118)

```cpp
def("find",
&di::NativeAdapterRegistry::find)
```

### NativeAdapterRegistry · "freeze"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L117)

```cpp
def("freeze",
&di::NativeAdapterRegistry::freeze)
```

### NativeYoloComponentSpec · "candidate_digest"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L157)

```cpp
def_readwrite("candidate_digest",
&di::yolo::NativeYoloComponentSpec::candidateDigest)
```

### NativeYoloComponentSpec · "merge_kind"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L156)

```cpp
def_readwrite("merge_kind",
&di::yolo::NativeYoloComponentSpec::mergeKind)
```

### NativeYoloComponentSpec · "result_egress_role"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L154)

```cpp
def_readwrite("result_egress_role",
&di::yolo::NativeYoloComponentSpec::resultEgressRole)
```

### NativeYoloComponentSpec · "input_ingress_role"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L152)

```cpp
def_readwrite("input_ingress_role",
&di::yolo::NativeYoloComponentSpec::inputIngressRole)
```

### NativeYoloComponentSpec · "node_names_by_role"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L150)

```cpp
def_readwrite("node_names_by_role",
&di::yolo::NativeYoloComponentSpec::nodeNamesByRole)
```

### NativeYoloComponentSpec · "roles"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L149)

```cpp
def_readwrite("roles",
&di::yolo::NativeYoloComponentSpec::roles)
```

### NativeYoloComponentSpec · "priority"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L148)

```cpp
def_readwrite("priority",
&di::yolo::NativeYoloComponentSpec::priority)
```

### NativeYoloComponentSpec · "candidate_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L147)

```cpp
def_readwrite("candidate_id",
&di::yolo::NativeYoloComponentSpec::candidateId)
```

### NativeInferenceHandle · "status_name"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L180)

```cpp
def_property_readonly("status_name",
[](const di::NativeInferenceHandle& handle) { implementation omitted })
```

### NativeInferenceHandle · "cancel"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L179)

```cpp
def("cancel",
&di::NativeInferenceHandle::cancel)
```

### NativeInferenceHandle · "result"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L175)

```cpp
def("result",
[](const di::NativeInferenceHandle& handle,
                       std::uint64_t wait_timeout_ms) { implementation omitted },
py::arg("wait_timeout_ms") = 0)
```

### NativeInferenceHandle · "status"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L174)

```cpp
def_property_readonly("status",
&di::NativeInferenceHandle::status)
```

### NativeInferenceHandle · "request_id"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L173)

```cpp
def_property_readonly("request_id",
&di::NativeInferenceHandle::requestId)
```

### NativeInferenceClient · "request"

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L187)

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

[源码](../../pythonWrapper/src/ndnsf/di_bindings.cpp#L186)

```cpp
def("close",
&di::NativeInferenceClient::close)
```
