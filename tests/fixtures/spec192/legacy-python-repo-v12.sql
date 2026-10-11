-- Exact generation-12 schema fixture captured from the checked-in Python initializer.
PRAGMA foreign_keys=ON;
CREATE TABLE artifact_active_catalog (
                logical_name TEXT NOT NULL,
                policy_epoch TEXT NOT NULL,
                format_version TEXT NOT NULL DEFAULT 'artifact-manifest-v2',
                digest_algorithm TEXT NOT NULL DEFAULT 'sha256',
                artifact_digest TEXT NOT NULL,
                generation INTEGER NOT NULL,
                operation_id TEXT NOT NULL,
                artifact_json TEXT NOT NULL,
                activated_at_ms INTEGER NOT NULL,
                PRIMARY KEY (logical_name, policy_epoch)
            );
CREATE TABLE artifact_capacity_reservations (
                operation_id TEXT PRIMARY KEY,
                artifact_digest TEXT NOT NULL,
                generation INTEGER NOT NULL,
                lease_id TEXT NOT NULL,
                owner_id TEXT NOT NULL,
                reserved_bytes INTEGER NOT NULL,
                actual_bytes INTEGER NOT NULL DEFAULT 0,
                expires_at_ms INTEGER NOT NULL,
                state TEXT NOT NULL,
                updated_at_ms INTEGER NOT NULL
            );
CREATE TABLE artifact_finalization_journal (
                operation_id TEXT PRIMARY KEY,
                artifact_digest TEXT NOT NULL,
                generation INTEGER NOT NULL,
                phase TEXT NOT NULL,
                detail_json TEXT NOT NULL,
                updated_at_ms INTEGER NOT NULL,
                error TEXT NOT NULL DEFAULT ''
            );
CREATE TABLE artifact_gc_claims (
                format_version TEXT NOT NULL DEFAULT 'artifact-manifest-v2',
                digest_algorithm TEXT NOT NULL DEFAULT 'sha256',
                artifact_digest TEXT NOT NULL,
                generation INTEGER NOT NULL,
                operation_id TEXT NOT NULL,
                gc_owner TEXT NOT NULL,
                claimed_at_ms INTEGER NOT NULL,
                deadline_ms INTEGER NOT NULL,
                state TEXT NOT NULL,
                PRIMARY KEY (artifact_digest, generation)
            );
CREATE TABLE artifact_lifecycle_journal (
                sequence INTEGER PRIMARY KEY AUTOINCREMENT,
                event_id TEXT NOT NULL UNIQUE,
                operation_id TEXT NOT NULL,
                artifact_digest TEXT NOT NULL,
                generation INTEGER NOT NULL,
                from_state TEXT NOT NULL,
                to_state TEXT NOT NULL,
                event_time_ms INTEGER NOT NULL,
                accepted INTEGER NOT NULL,
                detail_json TEXT NOT NULL,
                error TEXT NOT NULL DEFAULT ''
            );
CREATE TABLE artifact_replica_receipts (
                receipt_id TEXT PRIMARY KEY,
                operation_id TEXT NOT NULL UNIQUE,
                format_version TEXT NOT NULL DEFAULT 'artifact-manifest-v2',
                digest_algorithm TEXT NOT NULL DEFAULT 'sha256',
                artifact_digest TEXT NOT NULL,
                repo_node TEXT NOT NULL,
                generation INTEGER NOT NULL,
                receipt_json TEXT NOT NULL,
                signer_key_id TEXT NOT NULL,
                authentication_algorithm TEXT NOT NULL,
                signature_hex TEXT NOT NULL,
                committed_at_ms INTEGER NOT NULL
            );
CREATE TABLE artifact_schema_state (
                singleton INTEGER PRIMARY KEY CHECK(singleton=1),
                schema_generation INTEGER NOT NULL,
                format_version TEXT NOT NULL,
                migrated_at_ms INTEGER NOT NULL
            );
CREATE TABLE artifact_transfer_sessions (
                operation_id TEXT PRIMARY KEY,
                artifact_digest TEXT NOT NULL,
                generation INTEGER NOT NULL,
                identity_json TEXT NOT NULL,
                lease_json TEXT NOT NULL,
                state TEXT NOT NULL,
                preserves_progress INTEGER NOT NULL,
                verified_chunks INTEGER NOT NULL,
                newly_verified_bytes INTEGER NOT NULL,
                avoided_retransmission_bytes INTEGER NOT NULL,
                updated_at_ms INTEGER NOT NULL
            );
CREATE TABLE capacity_reservations (
                    reservation_id TEXT PRIMARY KEY,
                    operation_id TEXT NOT NULL,
                    reserved_bytes INTEGER NOT NULL,
                    state TEXT NOT NULL,
                    expires_at_ms INTEGER NOT NULL
                );
CREATE TABLE catalog_journal (
                    source_repo TEXT NOT NULL,
                    source_boot_id TEXT NOT NULL,
                    source_sequence INTEGER NOT NULL,
                    object_name TEXT NOT NULL,
                    generation INTEGER NOT NULL DEFAULT 0,
                    state TEXT NOT NULL,
                    digest TEXT NOT NULL,
                    entry_json TEXT NOT NULL,
                    PRIMARY KEY (source_repo, source_boot_id, source_sequence)
                );
CREATE TABLE catalog_tombstones (
                    object_name TEXT NOT NULL,
                    generation INTEGER NOT NULL,
                    source_repo TEXT NOT NULL,
                    source_boot_id TEXT NOT NULL,
                    source_sequence INTEGER NOT NULL,
                    entry_json TEXT NOT NULL,
                    PRIMARY KEY (object_name, generation)
                );
CREATE TABLE data_packets (
                    data_name TEXT PRIMARY KEY,
                    wire BLOB NOT NULL,
                    wire_sha256 TEXT NOT NULL,
                    wire_size INTEGER NOT NULL,
                    updated_at REAL NOT NULL,
                    hit_count INTEGER NOT NULL DEFAULT 0
                );
CREATE TABLE data_segments (
                    object_name TEXT NOT NULL,
                    segment_no INTEGER NOT NULL,
                    data_name TEXT NOT NULL,
                    wire BLOB NOT NULL,
                    wire_size INTEGER NOT NULL,
                    updated_at REAL NOT NULL,
                    hit_count INTEGER NOT NULL DEFAULT 0,
                    PRIMARY KEY (object_name, segment_no)
                );
CREATE TABLE object_packet_refs (
                    object_name TEXT NOT NULL,
                    ordinal INTEGER NOT NULL,
                    segment_no INTEGER NOT NULL,
                    data_name TEXT NOT NULL,
                    PRIMARY KEY (object_name, ordinal),
                    UNIQUE (object_name, data_name),
                    FOREIGN KEY (data_name) REFERENCES data_packets(data_name)
                );
CREATE TABLE objects (
                    object_name TEXT PRIMARY KEY,
                    manifest_json TEXT NOT NULL,
                    payload BLOB,
                    payload_size INTEGER NOT NULL DEFAULT 0,
                    sha256 TEXT NOT NULL,
                    object_type TEXT NOT NULL,
                    updated_at REAL NOT NULL,
                    hit_count INTEGER NOT NULL DEFAULT 0
                );
CREATE TABLE peer_watermarks (
                    peer_repo TEXT PRIMARY KEY,
                    peer_boot_id TEXT NOT NULL DEFAULT '',
                    source_sequence INTEGER NOT NULL DEFAULT 0,
                    updated_at_ms INTEGER NOT NULL
                );
CREATE TABLE repair_jobs (
                    repair_id TEXT PRIMARY KEY,
                    object_name TEXT NOT NULL,
                    generation INTEGER NOT NULL DEFAULT 0,
                    source_repo TEXT NOT NULL,
                    target_repo TEXT NOT NULL,
                    state TEXT NOT NULL,
                    attempts INTEGER NOT NULL DEFAULT 0,
                    next_attempt_ms INTEGER NOT NULL DEFAULT 0,
                    lease_owner TEXT NOT NULL DEFAULT '',
                    lease_deadline_ms INTEGER NOT NULL DEFAULT 0,
                    result_json TEXT NOT NULL DEFAULT '{}',
                    available_replicas INTEGER NOT NULL DEFAULT 0,
                    missing_replicas INTEGER NOT NULL DEFAULT 1,
                    object_priority INTEGER NOT NULL DEFAULT 0,
                    object_updated_at_ms INTEGER NOT NULL DEFAULT 0
                );
CREATE TABLE repo_membership (
                    repo_node TEXT PRIMARY KEY,
                    boot_id TEXT NOT NULL,
                    last_sequence INTEGER NOT NULL DEFAULT 0,
                    last_seen_ms INTEGER NOT NULL,
                    status_json TEXT NOT NULL
                );
CREATE TABLE repo_meta (
                    key TEXT PRIMARY KEY,
                    value TEXT NOT NULL
                );
CREATE TABLE serving_packets (
                    data_name TEXT PRIMARY KEY,
                    object_name TEXT NOT NULL,
                    generation INTEGER NOT NULL DEFAULT 0,
                    wire BLOB NOT NULL,
                    wire_sha256 TEXT NOT NULL,
                    wire_size INTEGER NOT NULL
                );
CREATE TABLE serving_prefixes (
                    prefix TEXT PRIMARY KEY,
                    object_name TEXT NOT NULL,
                    generation INTEGER NOT NULL DEFAULT 0,
                    active INTEGER NOT NULL DEFAULT 1
                );
CREATE TABLE write_operations (
                    operation_id TEXT PRIMARY KEY,
                    object_name TEXT NOT NULL,
                    generation INTEGER NOT NULL,
                    expected_generation INTEGER NOT NULL DEFAULT -1,
                    digest TEXT NOT NULL,
                    replication_factor INTEGER NOT NULL,
                    required_acks INTEGER NOT NULL,
                    consistency TEXT NOT NULL,
                    selected_replicas_json TEXT NOT NULL DEFAULT '[]',
                    state TEXT NOT NULL,
                    error TEXT NOT NULL DEFAULT '',
                    created_at_ms INTEGER NOT NULL,
                    updated_at_ms INTEGER NOT NULL
                );
CREATE TABLE write_receipts (
                    operation_id TEXT NOT NULL,
                    repo_node TEXT NOT NULL,
                    object_name TEXT NOT NULL,
                    generation INTEGER NOT NULL,
                    digest TEXT NOT NULL,
                    state TEXT NOT NULL,
                    persisted_bytes INTEGER NOT NULL,
                    completed_at_ms INTEGER NOT NULL,
                    PRIMARY KEY (operation_id, repo_node),
                    FOREIGN KEY (operation_id) REFERENCES write_operations(operation_id)
                );
CREATE INDEX idx_artifact_lifecycle_operation
            ON artifact_lifecycle_journal(operation_id, sequence)
        ;
CREATE INDEX idx_catalog_journal_object
                ON catalog_journal(object_name, generation)
            ;
CREATE INDEX idx_data_segments_data_name
                ON data_segments(data_name)
            ;
CREATE INDEX idx_object_packet_refs_data_name
                ON object_packet_refs(data_name)
            ;
CREATE INDEX idx_repair_jobs_ready
                ON repair_jobs(state, next_attempt_ms)
            ;
CREATE INDEX idx_repair_jobs_schedule
                ON repair_jobs(
                  target_repo, state, next_attempt_ms, available_replicas,
                  object_priority, object_updated_at_ms)
            ;
CREATE INDEX idx_serving_packets_object
                ON serving_packets(object_name, generation)
            ;
INSERT INTO artifact_schema_state(singleton,schema_generation,format_version,migrated_at_ms) VALUES(1,12,'artifact-manifest-v2',0);
INSERT INTO repo_meta(key,value) VALUES('schema_version','12'),('persistence_owner','fixture'),('used_bytes','0');
