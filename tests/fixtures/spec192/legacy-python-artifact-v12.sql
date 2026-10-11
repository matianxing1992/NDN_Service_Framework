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
CREATE INDEX idx_artifact_lifecycle_operation
            ON artifact_lifecycle_journal(operation_id, sequence)
        ;
INSERT INTO artifact_schema_state(singleton,schema_generation,format_version,migrated_at_ms) VALUES(1,12,'artifact-manifest-v2',0);
