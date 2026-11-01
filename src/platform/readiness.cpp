#include "sorx/platform/readiness.hpp"

#include <algorithm>
#include <bit>
#include <limits>

namespace sorx::platform {
namespace {

[[nodiscard]] constexpr std::uint64_t number(const bool value) noexcept {
    return value ? 1U : 0U;
}

[[nodiscard]] constexpr bool certification_relaxable(
    const FindingCode code) noexcept {
    return code != FindingCode::ProfileShapeInvalid &&
           code != FindingCode::ProfileSignatureNotVerified &&
           code != FindingCode::ProfileDigestMismatch &&
           code != FindingCode::FactsIncomplete &&
           code != FindingCode::FindingCapacityExceeded;
}

class ReportBuilder final {
public:
    ReportBuilder(
        const SignedReadinessProfile& supplied_profile,
        const ReadinessMode mode) noexcept
        : profile_(supplied_profile) {
        report_.evaluated_mode = mode;
    }

    void add(
        const FindingCode code,
        const std::uint64_t subject_id = 0U,
        const std::uint64_t expected = 0U,
        const std::uint64_t observed = 0U) noexcept {
        if (report_.total_findings !=
            std::numeric_limits<std::uint16_t>::max()) {
            ++report_.total_findings;
        }
        report_.production_ready = false;
        const bool relaxable = certification_relaxable(code);
        const bool relaxed =
            relaxable &&
            (profile_.certification_relaxation_mask & finding_mask(code)) !=
                0U;
        if (!relaxed) {
            report_.certification_ready = false;
        }

        if (report_.findings_truncated) {
            return;
        }
        if (report_.finding_count < kMaximumReadinessFindings - 1U) {
            report_.findings[report_.finding_count++] =
                ReadinessFinding{code, subject_id, expected, observed,
                                 !relaxed};
            return;
        }
        report_.findings[report_.finding_count++] = ReadinessFinding{
            FindingCode::FindingCapacityExceeded, 0U,
            kMaximumReadinessFindings, report_.total_findings, true};
        report_.findings_truncated = true;
        report_.certification_ready = false;
    }

    [[nodiscard]] ReadinessReport finish() const noexcept { return report_; }

private:
    const SignedReadinessProfile& profile_;
    ReadinessReport report_{};
};

[[nodiscard]] bool profile_shape_valid(
    const SignedReadinessProfile& profile) noexcept {
    if (profile.schema_version != kReadinessProfileSchemaVersion ||
        profile.profile_revision == 0U ||
        !digest_present(profile.signed_payload_digest) ||
        !digest_present(profile.signer_key_digest) ||
        profile.topology.logical_cpu_count == 0U ||
        profile.topology.socket_count == 0U ||
        profile.topology.numa_node_count == 0U ||
        profile.topology.realtime_cpus.empty() ||
        profile.topology.realtime_numa_node >=
            profile.topology.numa_node_count ||
        !digest_present(profile.firmware.system_firmware_digest) ||
        !digest_present(profile.firmware.nic_firmware_bundle_digest) ||
        profile.firmware.minimum_microcode_revision == 0U ||
        profile.ptp.maximum_absolute_offset_ns == 0U ||
        profile.ptp.minimum_lock_duration_ns == 0U ||
        profile.memory.minimum_hugepages == 0U ||
        profile.memory.hugepage_size_kib == 0U ||
        profile.link_count == 0U ||
        profile.link_count > kMaximumReadinessLinks ||
        profile.recorder.minimum_capacity_batches == 0U ||
        profile.recorder.minimum_usable_bytes == 0U ||
        profile.recorder.minimum_append_bytes_per_second == 0U ||
        profile.recorder.minimum_append_records_per_second == 0U ||
        profile.recorder.maximum_p99_999_completion_ns == 0U ||
        profile.recorder.minimum_durability == RecorderDurability::Unknown ||
        profile.power.governor == CpuGovernor::Unknown) {
        return false;
    }
    if (!profile.memory.require_unlimited_memlock &&
        profile.memory.minimum_memlock_bytes == 0U) {
        return false;
    }
    if (!digest_present(profile.boot.required_pcr_policy_digest)) {
        return false;
    }
    if (profile.nvme.namespace_id == 0U ||
        profile.nvme.minimum_capacity_bytes == 0U ||
        profile.nvme.minimum_io_queues == 0U ||
        !digest_present(profile.nvme.firmware_digest)) {
        return false;
    }
    for (std::size_t index = 0U; index < profile.link_count; ++index) {
        const LinkExpectation& link = profile.links[index];
        if (link.interface_id == 0U || link.minimum_speed_mbps == 0U ||
            link.minimum_rx_queues == 0U || link.minimum_tx_queues == 0U) {
            return false;
        }
        for (std::size_t prior = 0U; prior < index; ++prior) {
            if (profile.links[prior].interface_id == link.interface_id) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] const LinkFacts* find_link(
    const ObservedPlatformFacts& facts,
    const std::uint64_t interface_id) noexcept {
    const std::size_t count =
        std::min<std::size_t>(facts.link_count, facts.links.size());
    for (std::size_t index = 0U; index < count; ++index) {
        if (facts.links[index].interface_id == interface_id) {
            return &facts.links[index];
        }
    }
    return nullptr;
}

void evaluate_topology(
    ReportBuilder& report,
    const TopologyExpectation& expected,
    const TopologyFacts& observed) noexcept {
    if (observed.logical_cpu_count != expected.logical_cpu_count) {
        report.add(FindingCode::LogicalCpuCountMismatch, 0U,
                   expected.logical_cpu_count, observed.logical_cpu_count);
    }
    if (observed.socket_count != expected.socket_count) {
        report.add(FindingCode::SocketCountMismatch, 0U,
                   expected.socket_count, observed.socket_count);
    }
    if (observed.numa_node_count != expected.numa_node_count) {
        report.add(FindingCode::NumaCountMismatch, 0U,
                   expected.numa_node_count, observed.numa_node_count);
    }
    // These are appliance production invariants, not profile-selectable
    // preferences. Certification may relax the resulting finding explicitly.
    if (observed.smt_enabled) {
        report.add(FindingCode::SmtEnabled, 0U, 0U, 1U);
    }
    if (!observed.online_cpus.contains_all(expected.realtime_cpus)) {
        report.add(FindingCode::RealtimeCpuOffline, 0U,
                   expected.realtime_cpus.count(),
                   observed.online_cpus.count());
    }
    if (!observed.isolated_cpus.contains_all(expected.realtime_cpus)) {
        report.add(FindingCode::CpuIsolationMismatch, 0U,
                   expected.realtime_cpus.count(),
                   observed.isolated_cpus.count());
    }
    if (!observed.nohz_full_cpus.contains_all(expected.realtime_cpus)) {
        report.add(FindingCode::NoHzFullMismatch, 0U,
                   expected.realtime_cpus.count(),
                   observed.nohz_full_cpus.count());
    }
    if (!observed.rcu_nocbs_cpus.contains_all(expected.realtime_cpus)) {
        report.add(FindingCode::RcuNocbsMismatch, 0U,
                   expected.realtime_cpus.count(),
                   observed.rcu_nocbs_cpus.count());
    }
    if (!observed.irq_excluded_cpus.contains_all(expected.realtime_cpus)) {
        report.add(FindingCode::IrqAffinityMismatch, 0U,
                   expected.realtime_cpus.count(),
                   observed.irq_excluded_cpus.count());
    }
    if (observed.realtime_numa_node != expected.realtime_numa_node) {
        report.add(FindingCode::RealtimeNumaMismatch, 0U,
                   expected.realtime_numa_node,
                   observed.realtime_numa_node);
    }
    if (!observed.realtime_memory_local) {
        report.add(FindingCode::RealtimeMemoryNonlocal, 0U, 1U, 0U);
    }
}

void evaluate_firmware(
    ReportBuilder& report,
    const FirmwareExpectation& expected,
    const FirmwareFacts& observed) noexcept {
    if (observed.system_firmware_digest !=
        expected.system_firmware_digest) {
        report.add(FindingCode::SystemFirmwareMismatch, 0U, 1U, 0U);
    }
    if (observed.nic_firmware_bundle_digest !=
        expected.nic_firmware_bundle_digest) {
        report.add(FindingCode::NicFirmwareMismatch, 0U, 1U, 0U);
    }
    if (observed.microcode_revision < expected.minimum_microcode_revision) {
        report.add(FindingCode::MicrocodeTooOld, 0U,
                   expected.minimum_microcode_revision,
                   observed.microcode_revision);
    }
    if (!observed.rollback_protection_enabled) {
        report.add(FindingCode::FirmwareRollbackProtectionDisabled, 0U, 1U,
                   0U);
    }
}

void evaluate_ptp(
    ReportBuilder& report,
    const PtpExpectation& expected,
    const PtpFacts& observed) noexcept {
    if (observed.state != PtpState::Locked) {
        report.add(FindingCode::PtpNotLocked, 0U,
                   static_cast<std::uint8_t>(PtpState::Locked),
                   static_cast<std::uint8_t>(observed.state));
    }
    if (expected.grandmaster_identity != 0U &&
        observed.grandmaster_identity != expected.grandmaster_identity) {
        report.add(FindingCode::PtpGrandmasterMismatch, 0U,
                   expected.grandmaster_identity,
                   observed.grandmaster_identity);
    }
    if (observed.maximum_observed_absolute_offset_ns >
        expected.maximum_absolute_offset_ns) {
        report.add(FindingCode::PtpOffsetExceeded, 0U,
                   expected.maximum_absolute_offset_ns,
                   observed.maximum_observed_absolute_offset_ns);
    }
    if (observed.continuous_lock_duration_ns <
        expected.minimum_lock_duration_ns) {
        report.add(FindingCode::PtpLockDurationInsufficient, 0U,
                   expected.minimum_lock_duration_ns,
                   observed.continuous_lock_duration_ns);
    }
    if (!observed.hardware_timestamping_active) {
        report.add(FindingCode::PtpHardwareTimestampingInactive, 0U, 1U, 0U);
    }
}

void evaluate_memory(
    ReportBuilder& report,
    const MemoryExpectation& expected,
    const MemoryFacts& observed) noexcept {
    if (expected.require_unlimited_memlock) {
        if (!observed.memlock_unlimited) {
            report.add(FindingCode::MemlockInsufficient, 0U, 1U, 0U);
        }
    } else if (!observed.memlock_unlimited &&
               observed.memlock_limit_bytes < expected.minimum_memlock_bytes) {
        report.add(FindingCode::MemlockInsufficient, 0U,
                   expected.minimum_memlock_bytes,
                   observed.memlock_limit_bytes);
    }
    if (observed.hugepage_size_kib != expected.hugepage_size_kib) {
        report.add(FindingCode::HugepageSizeMismatch, 0U,
                   expected.hugepage_size_kib, observed.hugepage_size_kib);
    }
    if (observed.reserved_hugepages < expected.minimum_hugepages) {
        report.add(FindingCode::HugepageReservationInsufficient, 0U,
                   expected.minimum_hugepages,
                   observed.reserved_hugepages);
    }
    if (observed.free_hugepages < expected.minimum_hugepages) {
        report.add(FindingCode::HugepageAvailabilityInsufficient, 0U,
                   expected.minimum_hugepages, observed.free_hugepages);
    }
}

void evaluate_links(
    ReportBuilder& report,
    const SignedReadinessProfile& profile,
    const ObservedPlatformFacts& facts) noexcept {
    const std::size_t count =
        std::min<std::size_t>(profile.link_count, profile.links.size());
    for (std::size_t index = 0U; index < count; ++index) {
        const LinkExpectation& expected = profile.links[index];
        const LinkFacts* const observed = find_link(facts, expected.interface_id);
        if (observed == nullptr || !observed->present) {
            report.add(FindingCode::LinkMissing, expected.interface_id, 1U, 0U);
            continue;
        }
        if (!observed->up) {
            report.add(FindingCode::LinkDown, expected.interface_id, 1U, 0U);
        }
        if (observed->speed_mbps < expected.minimum_speed_mbps) {
            report.add(FindingCode::LinkSpeedInsufficient,
                       expected.interface_id, expected.minimum_speed_mbps,
                       observed->speed_mbps);
        }
        if (observed->numa_node != expected.numa_node) {
            report.add(FindingCode::LinkNumaMismatch, expected.interface_id,
                       expected.numa_node, observed->numa_node);
        }
        if (observed->rx_queues < expected.minimum_rx_queues ||
            observed->tx_queues < expected.minimum_tx_queues) {
            const std::uint64_t expected_queues =
                (static_cast<std::uint64_t>(expected.minimum_rx_queues)
                 << 32U) |
                expected.minimum_tx_queues;
            const std::uint64_t observed_queues =
                (static_cast<std::uint64_t>(observed->rx_queues) << 32U) |
                observed->tx_queues;
            report.add(FindingCode::LinkQueueInsufficient,
                       expected.interface_id, expected_queues,
                       observed_queues);
        }
        if (!observed->hardware_timestamping_active) {
            report.add(FindingCode::LinkHardwareTimestampingInactive,
                       expected.interface_id, 1U, 0U);
        }
        if (!observed->kernel_bypass_active) {
            report.add(FindingCode::LinkKernelBypassInactive,
                       expected.interface_id, 1U, 0U);
        }
    }
}

void evaluate_recorder(
    ReportBuilder& report,
    const RecorderExpectation& expected,
    const RecorderFacts& observed) noexcept {
    if (!observed.initialized) {
        report.add(FindingCode::RecorderNotInitialized, 0U, 1U, 0U);
    }
    const RecorderDurability required_durability =
        static_cast<std::uint8_t>(expected.minimum_durability) >
                static_cast<std::uint8_t>(
                    RecorderDurability::HardwarePersistent)
            ? expected.minimum_durability
            : RecorderDurability::HardwarePersistent;
    if (static_cast<std::uint8_t>(observed.durability) <
        static_cast<std::uint8_t>(required_durability)) {
        report.add(FindingCode::RecorderDurabilityInsufficient, 0U,
                   static_cast<std::uint8_t>(required_durability),
                   static_cast<std::uint8_t>(observed.durability));
    }
    if (!observed.mirrors_healthy) {
        report.add(FindingCode::RecorderMirrorUnhealthy, 0U, 1U, 0U);
    }
    if (observed.capacity_batches < expected.minimum_capacity_batches) {
        report.add(FindingCode::RecorderCapacityInsufficient, 0U,
                   expected.minimum_capacity_batches,
                   observed.capacity_batches);
    }
    const std::uint32_t required_cancel =
        std::max<std::uint32_t>(1U, expected.minimum_cancel_reservation);
    if (observed.cancel_reservation < required_cancel) {
        report.add(FindingCode::RecorderCancelReservationInsufficient, 0U,
                   required_cancel,
                   observed.cancel_reservation);
    }
    const std::uint32_t required_reconciliation = std::max<std::uint32_t>(
        1U, expected.minimum_reconciliation_reservation);
    if (observed.reconciliation_reservation < required_reconciliation) {
        report.add(
            FindingCode::RecorderReconciliationReservationInsufficient, 0U,
            required_reconciliation,
            observed.reconciliation_reservation);
    }
    if (observed.usable_bytes < expected.minimum_usable_bytes) {
        report.add(FindingCode::RecorderUsableBytesInsufficient, 0U,
                   expected.minimum_usable_bytes, observed.usable_bytes);
    }
    if (observed.qualified_append_bytes_per_second <
        expected.minimum_append_bytes_per_second) {
        report.add(FindingCode::RecorderAppendBandwidthInsufficient, 0U,
                   expected.minimum_append_bytes_per_second,
                   observed.qualified_append_bytes_per_second);
    }
    if (observed.qualified_append_records_per_second <
        expected.minimum_append_records_per_second) {
        report.add(FindingCode::RecorderAppendRateInsufficient, 0U,
                   expected.minimum_append_records_per_second,
                   observed.qualified_append_records_per_second);
    }
    if (observed.measured_p99_999_completion_ns == 0U ||
        observed.measured_p99_999_completion_ns >
            expected.maximum_p99_999_completion_ns) {
        report.add(FindingCode::RecorderCompletionLatencyExceeded, 0U,
                   expected.maximum_p99_999_completion_ns,
                   observed.measured_p99_999_completion_ns);
    }
    if (!observed.peer_to_peer_qualified) {
        report.add(
            FindingCode::RecorderPeerToPeerUnqualified, 0U, 1U, 0U);
    }
    if (!observed.power_loss_qualified) {
        report.add(FindingCode::RecorderPowerLossUnqualified, 0U, 1U, 0U);
    }
}

void evaluate_nvme(
    ReportBuilder& report,
    const NvmeExpectation& expected,
    const NvmeFacts& observed) noexcept {
    if (!observed.present) {
        report.add(FindingCode::NvmeMissing, 0U, 1U, 0U);
        return;
    }
    if (!observed.healthy) {
        report.add(FindingCode::NvmeUnhealthy, observed.namespace_id, 1U, 0U);
    }
    if (observed.namespace_id != expected.namespace_id) {
        report.add(FindingCode::NvmeNamespaceMismatch, 0U,
                   expected.namespace_id, observed.namespace_id);
    }
    if (observed.capacity_bytes < expected.minimum_capacity_bytes) {
        report.add(FindingCode::NvmeCapacityInsufficient,
                   observed.namespace_id, expected.minimum_capacity_bytes,
                   observed.capacity_bytes);
    }
    if (observed.numa_node != expected.numa_node) {
        report.add(FindingCode::NvmeNumaMismatch, observed.namespace_id,
                   expected.numa_node, observed.numa_node);
    }
    if (observed.io_queues < expected.minimum_io_queues) {
        report.add(FindingCode::NvmeQueueInsufficient, observed.namespace_id,
                   expected.minimum_io_queues, observed.io_queues);
    }
    if (observed.firmware_digest != expected.firmware_digest) {
        report.add(FindingCode::NvmeFirmwareMismatch,
                   observed.namespace_id, 1U, 0U);
    }
    if (!observed.power_loss_protection_active) {
        report.add(FindingCode::NvmePowerLossProtectionInactive,
                   observed.namespace_id, 1U, 0U);
    }
}

void evaluate_boot(
    ReportBuilder& report,
    const BootExpectation& expected,
    const BootFacts& observed) noexcept {
    if (!observed.secure_boot_enabled) {
        report.add(FindingCode::SecureBootDisabled, 0U, 1U, 0U);
    }
    if (!observed.measured_boot_valid) {
        report.add(FindingCode::MeasuredBootInvalid, 0U, 1U, 0U);
    }
    if (!observed.tpm_present) {
        report.add(FindingCode::TpmMissing, 0U, 1U, 0U);
    }
    if (observed.pcr_policy_digest != expected.required_pcr_policy_digest) {
        report.add(FindingCode::PcrPolicyMismatch, 0U, 1U, 0U);
    }
}

void evaluate_power(
    ReportBuilder& report,
    const PowerExpectation& expected,
    const PowerFacts& observed) noexcept {
    if (observed.governor != expected.governor) {
        report.add(FindingCode::GovernorMismatch, 0U,
                   static_cast<std::uint8_t>(expected.governor),
                   static_cast<std::uint8_t>(observed.governor));
    }
    if (observed.maximum_enabled_c_state >
        expected.maximum_enabled_c_state) {
        report.add(FindingCode::CStateLimitExceeded, 0U,
                   expected.maximum_enabled_c_state,
                   observed.maximum_enabled_c_state);
    }
}

}  // namespace

bool digest_present(const Digest256& digest) noexcept {
    return std::any_of(digest.begin(), digest.end(), [](const std::byte value) {
        return value != std::byte{};
    });
}

bool CpuSet::try_add(const std::uint16_t cpu) noexcept {
    if (cpu >= kMaximumReadinessCpus) {
        return false;
    }
    words_[cpu / 64U] |= std::uint64_t{1U} << (cpu % 64U);
    return true;
}

bool CpuSet::contains(const std::uint16_t cpu) const noexcept {
    return cpu < kMaximumReadinessCpus &&
           (words_[cpu / 64U] &
            (std::uint64_t{1U} << (cpu % 64U))) != 0U;
}

bool CpuSet::contains_all(const CpuSet& required) const noexcept {
    for (std::size_t index = 0U; index < words_.size(); ++index) {
        if ((words_[index] & required.words_[index]) !=
            required.words_[index]) {
            return false;
        }
    }
    return true;
}

bool CpuSet::empty() const noexcept {
    return std::all_of(words_.begin(), words_.end(), [](const auto word) {
        return word == 0U;
    });
}

std::size_t CpuSet::count() const noexcept {
    std::size_t result = 0U;
    for (const std::uint64_t word : words_) {
        result += std::popcount(word);
    }
    return result;
}

ReadinessReport evaluate_readiness(
    const SignedReadinessProfile& profile,
    const ObservedPlatformFacts& facts,
    const ReadinessMode mode) noexcept {
    ReportBuilder report{profile, mode};
    if (!profile_shape_valid(profile)) {
        report.add(FindingCode::ProfileShapeInvalid);
    }
    if (profile.signature_verification != SignatureVerification::Verified) {
        report.add(FindingCode::ProfileSignatureNotVerified, 0U,
                   static_cast<std::uint8_t>(SignatureVerification::Verified),
                   static_cast<std::uint8_t>(
                       profile.signature_verification));
    }
    if (!digest_present(facts.applied_profile_digest) ||
        facts.applied_profile_digest != profile.signed_payload_digest) {
        report.add(FindingCode::ProfileDigestMismatch, 0U, 1U, 0U);
    }
    if (!facts.collection_complete ||
        facts.link_count > kMaximumReadinessLinks) {
        report.add(FindingCode::FactsIncomplete, 0U, 1U,
                   number(facts.collection_complete));
    }

    evaluate_topology(report, profile.topology, facts.topology);
    evaluate_firmware(report, profile.firmware, facts.firmware);
    evaluate_ptp(report, profile.ptp, facts.ptp);
    evaluate_memory(report, profile.memory, facts.memory);
    evaluate_links(report, profile, facts);
    evaluate_recorder(report, profile.recorder, facts.recorder);
    evaluate_nvme(report, profile.nvme, facts.nvme);
    evaluate_boot(report, profile.boot, facts.boot);
    evaluate_power(report, profile.power, facts.power);
    return report.finish();
}

}  // namespace sorx::platform
