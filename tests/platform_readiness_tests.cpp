#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

#include "sorx/platform/readiness.hpp"

#include <cstddef>
#include <cstdint>

namespace {

using namespace sorx::platform;

[[nodiscard]] Digest256 digest(const std::uint8_t seed) noexcept {
    Digest256 result{};
    for (std::size_t index = 0U; index < result.size(); ++index) {
        result[index] = static_cast<std::byte>(seed + index);
    }
    return result;
}

[[nodiscard]] SignedReadinessProfile profile() noexcept {
    SignedReadinessProfile result{};
    result.profile_revision = 17U;
    result.signed_payload_digest = digest(1U);
    result.signer_key_digest = digest(33U);
    result.signature_verification = SignatureVerification::Verified;

    result.topology.logical_cpu_count = 8U;
    result.topology.socket_count = 1U;
    result.topology.numa_node_count = 1U;
    result.topology.realtime_numa_node = 0U;
    assert(result.topology.realtime_cpus.try_add(2U));
    assert(result.topology.realtime_cpus.try_add(3U));

    result.firmware.system_firmware_digest = digest(65U);
    result.firmware.nic_firmware_bundle_digest = digest(97U);
    result.firmware.minimum_microcode_revision = 200U;

    result.ptp.grandmaster_identity = 0x1122334455667788ULL;
    result.ptp.maximum_absolute_offset_ns = 500U;
    result.ptp.minimum_lock_duration_ns = 10'000U;

    result.memory.minimum_hugepages = 1'024U;
    result.memory.hugepage_size_kib = 2'048U;
    result.memory.require_unlimited_memlock = true;

    result.link_count = 1U;
    result.links[0].interface_id = 42U;
    result.links[0].minimum_speed_mbps = 100'000U;
    result.links[0].numa_node = 0U;
    result.links[0].minimum_rx_queues = 4U;
    result.links[0].minimum_tx_queues = 4U;

    result.recorder.minimum_durability =
        RecorderDurability::HardwarePersistent;
    result.recorder.minimum_capacity_batches = 4'096U;
    result.recorder.minimum_cancel_reservation = 128U;
    result.recorder.minimum_reconciliation_reservation = 128U;
    result.recorder.minimum_usable_bytes = 4ULL * 1'024U * 1'024U * 1'024U;
    result.recorder.minimum_append_bytes_per_second = 500'000'000U;
    result.recorder.minimum_append_records_per_second = 1'000'000U;
    result.recorder.maximum_p99_999_completion_ns = 2'000U;

    result.nvme.namespace_id = 7U;
    result.nvme.minimum_capacity_bytes = 1'000'000'000U;
    result.nvme.numa_node = 0U;
    result.nvme.minimum_io_queues = 4U;
    result.nvme.firmware_digest = digest(129U);

    result.boot.required_pcr_policy_digest = digest(161U);
    result.power.governor = CpuGovernor::Performance;
    result.power.maximum_enabled_c_state = 1U;
    return result;
}

[[nodiscard]] ObservedPlatformFacts facts(
    const SignedReadinessProfile& expected) noexcept {
    ObservedPlatformFacts result{};
    result.applied_profile_digest = expected.signed_payload_digest;
    result.collection_complete = true;

    result.topology.logical_cpu_count = expected.topology.logical_cpu_count;
    result.topology.socket_count = expected.topology.socket_count;
    result.topology.numa_node_count = expected.topology.numa_node_count;
    result.topology.realtime_numa_node =
        expected.topology.realtime_numa_node;
    for (std::uint16_t cpu = 0U; cpu < 8U; ++cpu) {
        assert(result.topology.online_cpus.try_add(cpu));
    }
    result.topology.isolated_cpus = expected.topology.realtime_cpus;
    result.topology.nohz_full_cpus = expected.topology.realtime_cpus;
    result.topology.rcu_nocbs_cpus = expected.topology.realtime_cpus;
    result.topology.irq_excluded_cpus = expected.topology.realtime_cpus;
    result.topology.smt_enabled = false;
    result.topology.realtime_memory_local = true;

    result.firmware.system_firmware_digest =
        expected.firmware.system_firmware_digest;
    result.firmware.nic_firmware_bundle_digest =
        expected.firmware.nic_firmware_bundle_digest;
    result.firmware.microcode_revision =
        expected.firmware.minimum_microcode_revision;
    result.firmware.rollback_protection_enabled = true;

    result.ptp.state = PtpState::Locked;
    result.ptp.grandmaster_identity = expected.ptp.grandmaster_identity;
    result.ptp.maximum_observed_absolute_offset_ns = 100U;
    result.ptp.continuous_lock_duration_ns =
        expected.ptp.minimum_lock_duration_ns;
    result.ptp.hardware_timestamping_active = true;

    result.memory.memlock_unlimited = true;
    result.memory.hugepage_size_kib = expected.memory.hugepage_size_kib;
    result.memory.reserved_hugepages = expected.memory.minimum_hugepages;
    result.memory.free_hugepages = expected.memory.minimum_hugepages;

    result.link_count = expected.link_count;
    result.links[0].interface_id = expected.links[0].interface_id;
    result.links[0].speed_mbps = expected.links[0].minimum_speed_mbps;
    result.links[0].numa_node = expected.links[0].numa_node;
    result.links[0].rx_queues = expected.links[0].minimum_rx_queues;
    result.links[0].tx_queues = expected.links[0].minimum_tx_queues;
    result.links[0].present = true;
    result.links[0].up = true;
    result.links[0].hardware_timestamping_active = true;
    result.links[0].kernel_bypass_active = true;

    result.recorder.durability = expected.recorder.minimum_durability;
    result.recorder.capacity_batches =
        expected.recorder.minimum_capacity_batches;
    result.recorder.cancel_reservation =
        expected.recorder.minimum_cancel_reservation;
    result.recorder.reconciliation_reservation =
        expected.recorder.minimum_reconciliation_reservation;
    result.recorder.usable_bytes = expected.recorder.minimum_usable_bytes;
    result.recorder.qualified_append_bytes_per_second =
        expected.recorder.minimum_append_bytes_per_second;
    result.recorder.qualified_append_records_per_second =
        expected.recorder.minimum_append_records_per_second;
    result.recorder.measured_p99_999_completion_ns =
        expected.recorder.maximum_p99_999_completion_ns;
    result.recorder.initialized = true;
    result.recorder.mirrors_healthy = true;
    result.recorder.peer_to_peer_qualified = true;
    result.recorder.power_loss_qualified = true;

    result.nvme.namespace_id = expected.nvme.namespace_id;
    result.nvme.capacity_bytes = expected.nvme.minimum_capacity_bytes;
    result.nvme.numa_node = expected.nvme.numa_node;
    result.nvme.io_queues = expected.nvme.minimum_io_queues;
    result.nvme.firmware_digest = expected.nvme.firmware_digest;
    result.nvme.present = true;
    result.nvme.healthy = true;
    result.nvme.power_loss_protection_active = true;

    result.boot.pcr_policy_digest = expected.boot.required_pcr_policy_digest;
    result.boot.secure_boot_enabled = true;
    result.boot.measured_boot_valid = true;
    result.boot.tpm_present = true;

    result.power.governor = expected.power.governor;
    result.power.maximum_enabled_c_state =
        expected.power.maximum_enabled_c_state;
    return result;
}

[[nodiscard]] bool has(
    const ReadinessReport& report,
    const FindingCode code) noexcept {
    for (std::size_t index = 0U; index < report.finding_count; ++index) {
        if (report.findings[index].code == code) {
            return true;
        }
    }
    return false;
}

void test_matching_profile_is_ready() {
    const SignedReadinessProfile expected = profile();
    const ObservedPlatformFacts observed = facts(expected);
    const ReadinessReport production = evaluate_readiness(
        expected, observed, ReadinessMode::Production);
    assert(production.eligible());
    assert(production.production_ready);
    assert(production.certification_ready);
    assert(production.finding_count == 0U);

    const ReadinessReport certification = evaluate_readiness(
        expected, observed, ReadinessMode::Certification);
    assert(certification.eligible());
    assert(certification.finding_count == 0U);
}

void test_production_fails_closed_across_domains() {
    const SignedReadinessProfile expected = profile();
    ObservedPlatformFacts observed = facts(expected);
    observed.topology.isolated_cpus = {};
    observed.firmware.system_firmware_digest[0] ^= std::byte{0x01U};
    observed.ptp.state = PtpState::Unlocked;
    observed.memory.memlock_unlimited = false;
    observed.memory.free_hugepages = 1U;
    observed.links[0].up = false;
    observed.links[0].kernel_bypass_active = false;
    observed.recorder.durability =
        RecorderDurability::VolatileSoftwareReference;
    observed.recorder.qualified_append_bytes_per_second = 1U;
    observed.recorder.qualified_append_records_per_second = 1U;
    observed.recorder.measured_p99_999_completion_ns = 9'000U;
    observed.recorder.peer_to_peer_qualified = false;
    observed.recorder.power_loss_qualified = false;
    observed.nvme.power_loss_protection_active = false;
    observed.boot.secure_boot_enabled = false;
    observed.boot.measured_boot_valid = false;
    observed.power.governor = CpuGovernor::Powersave;
    observed.power.maximum_enabled_c_state = 6U;

    const ReadinessReport report = evaluate_readiness(
        expected, observed, ReadinessMode::Production);
    assert(!report.eligible());
    assert(!report.production_ready);
    assert(!report.certification_ready);
    assert(has(report, FindingCode::CpuIsolationMismatch));
    assert(has(report, FindingCode::SystemFirmwareMismatch));
    assert(has(report, FindingCode::PtpNotLocked));
    assert(has(report, FindingCode::MemlockInsufficient));
    assert(has(report, FindingCode::HugepageAvailabilityInsufficient));
    assert(has(report, FindingCode::LinkDown));
    assert(has(report, FindingCode::LinkKernelBypassInactive));
    assert(has(report, FindingCode::RecorderDurabilityInsufficient));
    assert(has(report, FindingCode::RecorderAppendBandwidthInsufficient));
    assert(has(report, FindingCode::RecorderAppendRateInsufficient));
    assert(has(report, FindingCode::RecorderCompletionLatencyExceeded));
    assert(has(report, FindingCode::RecorderPeerToPeerUnqualified));
    assert(has(report, FindingCode::RecorderPowerLossUnqualified));
    assert(has(report, FindingCode::NvmePowerLossProtectionInactive));
    assert(has(report, FindingCode::SecureBootDisabled));
    assert(has(report, FindingCode::MeasuredBootInvalid));
    assert(has(report, FindingCode::GovernorMismatch));
    assert(has(report, FindingCode::CStateLimitExceeded));
}

void test_signed_profile_cannot_disable_production_baseline() {
    SignedReadinessProfile weakened = profile();
    weakened.topology.require_smt_disabled = false;
    weakened.topology.require_realtime_memory_local = false;
    weakened.firmware.require_rollback_protection = false;
    weakened.ptp.require_hardware_timestamping = false;
    weakened.links[0].require_hardware_timestamping = false;
    weakened.links[0].require_kernel_bypass = false;
    weakened.recorder.minimum_durability =
        RecorderDurability::VolatileSoftwareReference;
    weakened.recorder.minimum_cancel_reservation = 0U;
    weakened.recorder.minimum_reconciliation_reservation = 0U;
    weakened.recorder.require_mirrors_healthy = false;
    weakened.recorder.require_peer_to_peer_qualified = false;
    weakened.recorder.require_power_loss_qualified = false;
    weakened.nvme.required = false;
    weakened.nvme.require_power_loss_protection = false;
    weakened.boot.require_secure_boot = false;
    weakened.boot.require_measured_boot = false;
    weakened.boot.require_tpm = false;

    ObservedPlatformFacts observed = facts(weakened);
    observed.topology.smt_enabled = true;
    observed.topology.realtime_memory_local = false;
    observed.firmware.rollback_protection_enabled = false;
    observed.ptp.hardware_timestamping_active = false;
    observed.links[0].hardware_timestamping_active = false;
    observed.links[0].kernel_bypass_active = false;
    observed.recorder.durability =
        RecorderDurability::VolatileSoftwareReference;
    observed.recorder.cancel_reservation = 0U;
    observed.recorder.reconciliation_reservation = 0U;
    observed.recorder.mirrors_healthy = false;
    observed.recorder.peer_to_peer_qualified = false;
    observed.recorder.power_loss_qualified = false;
    observed.nvme.present = false;
    observed.nvme.power_loss_protection_active = false;
    observed.boot.secure_boot_enabled = false;
    observed.boot.measured_boot_valid = false;
    observed.boot.tpm_present = false;

    const ReadinessReport report = evaluate_readiness(
        weakened, observed, ReadinessMode::Production);
    assert(!report.eligible());
    assert(has(report, FindingCode::SmtEnabled));
    assert(has(report, FindingCode::RealtimeMemoryNonlocal));
    assert(has(report, FindingCode::FirmwareRollbackProtectionDisabled));
    assert(has(report, FindingCode::PtpHardwareTimestampingInactive));
    assert(has(report, FindingCode::LinkHardwareTimestampingInactive));
    assert(has(report, FindingCode::LinkKernelBypassInactive));
    assert(has(report, FindingCode::RecorderDurabilityInsufficient));
    assert(has(report, FindingCode::RecorderMirrorUnhealthy));
    assert(has(report, FindingCode::RecorderCancelReservationInsufficient));
    assert(has(
        report, FindingCode::RecorderReconciliationReservationInsufficient));
    assert(has(report, FindingCode::RecorderPeerToPeerUnqualified));
    assert(has(report, FindingCode::RecorderPowerLossUnqualified));
    assert(has(report, FindingCode::NvmeMissing));
    assert(has(report, FindingCode::SecureBootDisabled));
    assert(has(report, FindingCode::MeasuredBootInvalid));
    assert(has(report, FindingCode::TpmMissing));
}

void test_certification_relaxation_never_weakens_production() {
    SignedReadinessProfile expected = profile();
    expected.certification_relaxation_mask =
        finding_mask(FindingCode::LinkKernelBypassInactive);
    ObservedPlatformFacts observed = facts(expected);
    observed.links[0].kernel_bypass_active = false;

    const ReadinessReport certification = evaluate_readiness(
        expected, observed, ReadinessMode::Certification);
    assert(certification.eligible());
    assert(certification.certification_ready);
    assert(!certification.production_ready);
    assert(certification.finding_count == 1U);
    assert(!certification.findings[0].certification_blocking);

    const ReadinessReport production = evaluate_readiness(
        expected, observed, ReadinessMode::Production);
    assert(!production.eligible());
    assert(!production.production_ready);
}

void test_profile_trust_cannot_be_relaxed() {
    SignedReadinessProfile expected = profile();
    expected.signature_verification = SignatureVerification::Invalid;
    expected.certification_relaxation_mask =
        finding_mask(FindingCode::ProfileSignatureNotVerified) |
        finding_mask(FindingCode::ProfileDigestMismatch);
    ObservedPlatformFacts observed = facts(expected);
    observed.applied_profile_digest[0] ^= std::byte{0x01U};
    const ReadinessReport report = evaluate_readiness(
        expected, observed, ReadinessMode::Certification);
    assert(!report.eligible());
    assert(has(report, FindingCode::ProfileSignatureNotVerified));
    assert(has(report, FindingCode::ProfileDigestMismatch));
}

void test_incomplete_and_malformed_inputs_fail_closed() {
    SignedReadinessProfile expected = profile();
    expected.profile_revision = 0U;
    ObservedPlatformFacts observed = facts(expected);
    observed.collection_complete = false;
    const ReadinessReport report = evaluate_readiness(
        expected, observed, ReadinessMode::Production);
    assert(!report.eligible());
    assert(has(report, FindingCode::ProfileShapeInvalid));
    assert(has(report, FindingCode::FactsIncomplete));

    CpuSet cpus;
    assert(cpus.try_add(255U));
    assert(cpus.contains(255U));
    assert(!cpus.try_add(256U));
}

void test_finding_overflow_is_itself_blocking() {
    SignedReadinessProfile expected = profile();
    expected.link_count =
        static_cast<std::uint8_t>(kMaximumReadinessLinks);
    for (std::size_t index = 0U; index < expected.link_count; ++index) {
        expected.links[index] = expected.links[0];
        expected.links[index].interface_id = 100U + index;
    }
    ObservedPlatformFacts observed{};
    observed.link_count =
        static_cast<std::uint8_t>(kMaximumReadinessLinks);
    for (std::size_t index = 0U; index < observed.link_count; ++index) {
        observed.links[index].interface_id = expected.links[index].interface_id;
        observed.links[index].present = true;
    }
    const ReadinessReport report = evaluate_readiness(
        expected, observed, ReadinessMode::Certification);
    assert(!report.eligible());
    assert(report.findings_truncated);
    assert(report.finding_count == kMaximumReadinessFindings);
    assert(report.findings[report.finding_count - 1U].code ==
           FindingCode::FindingCapacityExceeded);
    assert(report.total_findings > report.finding_count);
}

static_assert(noexcept(evaluate_readiness(
    SignedReadinessProfile{}, ObservedPlatformFacts{},
    ReadinessMode::Production)));

}  // namespace

int main() {
    test_matching_profile_is_ready();
    test_production_fails_closed_across_domains();
    test_signed_profile_cannot_disable_production_baseline();
    test_certification_relaxation_never_weakens_production();
    test_profile_trust_cannot_be_relaxed();
    test_incomplete_and_malformed_inputs_fail_closed();
    test_finding_overflow_is_itself_blocking();
    return 0;
}
