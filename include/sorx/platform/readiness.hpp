#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>

namespace sorx::platform {

inline constexpr std::uint32_t kReadinessProfileSchemaVersion = 1U;
inline constexpr std::size_t kMaximumReadinessCpus = 256U;
inline constexpr std::size_t kMaximumReadinessLinks = 8U;
inline constexpr std::size_t kMaximumReadinessFindings = 64U;

using Digest256 = std::array<std::byte, 32U>;

[[nodiscard]] bool digest_present(const Digest256& digest) noexcept;

class CpuSet final {
public:
    [[nodiscard]] bool try_add(std::uint16_t cpu) noexcept;
    [[nodiscard]] bool contains(std::uint16_t cpu) const noexcept;
    [[nodiscard]] bool contains_all(const CpuSet& required) const noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::size_t count() const noexcept;

    auto operator<=>(const CpuSet&) const = default;

private:
    std::array<std::uint64_t, kMaximumReadinessCpus / 64U> words_{};
};

enum class ReadinessMode : std::uint8_t {
    Certification,
    Production,
};

enum class SignatureVerification : std::uint8_t {
    NotChecked,
    Verified,
    Invalid,
};

enum class PtpState : std::uint8_t {
    Unknown,
    Unlocked,
    Acquiring,
    Locked,
};

enum class CpuGovernor : std::uint8_t {
    Unknown,
    Performance,
    Powersave,
    Schedutil,
};

enum class RecorderDurability : std::uint8_t {
    Unknown,
    VolatileSoftwareReference,
    HardwarePersistent,
};

struct TopologyExpectation final {
    std::uint16_t logical_cpu_count{};
    std::uint16_t socket_count{};
    std::uint16_t numa_node_count{};
    std::uint16_t realtime_numa_node{};
    CpuSet realtime_cpus;
    bool require_smt_disabled{true};
    bool require_realtime_memory_local{true};
};

struct FirmwareExpectation final {
    Digest256 system_firmware_digest{};
    Digest256 nic_firmware_bundle_digest{};
    std::uint64_t minimum_microcode_revision{};
    bool require_rollback_protection{true};
};

struct PtpExpectation final {
    std::uint64_t grandmaster_identity{};
    std::uint64_t maximum_absolute_offset_ns{};
    std::uint64_t minimum_lock_duration_ns{};
    bool require_hardware_timestamping{true};
};

struct MemoryExpectation final {
    std::uint64_t minimum_memlock_bytes{};
    std::uint32_t hugepage_size_kib{2'048U};
    std::uint32_t minimum_hugepages{};
    bool require_unlimited_memlock{true};
};

struct LinkExpectation final {
    std::uint64_t interface_id{};
    std::uint32_t minimum_speed_mbps{};
    std::uint16_t numa_node{};
    std::uint16_t minimum_rx_queues{};
    std::uint16_t minimum_tx_queues{};
    bool require_hardware_timestamping{true};
    bool require_kernel_bypass{true};
};

struct RecorderExpectation final {
    RecorderDurability minimum_durability{
        RecorderDurability::HardwarePersistent};
    std::uint32_t minimum_capacity_batches{};
    std::uint32_t minimum_cancel_reservation{};
    std::uint32_t minimum_reconciliation_reservation{};
    std::uint64_t minimum_usable_bytes{};
    std::uint64_t minimum_append_bytes_per_second{};
    std::uint64_t minimum_append_records_per_second{};
    std::uint64_t maximum_p99_999_completion_ns{};
    bool require_mirrors_healthy{true};
    bool require_peer_to_peer_qualified{true};
    bool require_power_loss_qualified{true};
};

struct NvmeExpectation final {
    bool required{true};
    std::uint64_t namespace_id{};
    std::uint64_t minimum_capacity_bytes{};
    std::uint16_t numa_node{};
    std::uint16_t minimum_io_queues{};
    Digest256 firmware_digest{};
    bool require_power_loss_protection{true};
};

struct BootExpectation final {
    Digest256 required_pcr_policy_digest{};
    bool require_secure_boot{true};
    bool require_measured_boot{true};
    bool require_tpm{true};
};

struct PowerExpectation final {
    CpuGovernor governor{CpuGovernor::Performance};
    std::uint8_t maximum_enabled_c_state{};
};

struct SignedReadinessProfile final {
    std::uint32_t schema_version{kReadinessProfileSchemaVersion};
    std::uint64_t profile_revision{};
    Digest256 signed_payload_digest{};
    Digest256 signer_key_digest{};
    SignatureVerification signature_verification{
        SignatureVerification::NotChecked};
    // A bit for FindingCode N allows that finding only in certification.
    // Signature/profile-integrity failures and finding overflow are never
    // relaxable. Production ignores this mask and fails on every finding.
    std::uint64_t certification_relaxation_mask{};
    TopologyExpectation topology;
    FirmwareExpectation firmware;
    PtpExpectation ptp;
    MemoryExpectation memory;
    std::array<LinkExpectation, kMaximumReadinessLinks> links{};
    std::uint8_t link_count{};
    RecorderExpectation recorder;
    NvmeExpectation nvme;
    BootExpectation boot;
    PowerExpectation power;
};

struct TopologyFacts final {
    std::uint16_t logical_cpu_count{};
    std::uint16_t socket_count{};
    std::uint16_t numa_node_count{};
    std::uint16_t realtime_numa_node{};
    CpuSet online_cpus;
    CpuSet isolated_cpus;
    CpuSet nohz_full_cpus;
    CpuSet rcu_nocbs_cpus;
    CpuSet irq_excluded_cpus;
    bool smt_enabled{};
    bool realtime_memory_local{};
};

struct FirmwareFacts final {
    Digest256 system_firmware_digest{};
    Digest256 nic_firmware_bundle_digest{};
    std::uint64_t microcode_revision{};
    bool rollback_protection_enabled{};
};

struct PtpFacts final {
    PtpState state{PtpState::Unknown};
    std::uint64_t grandmaster_identity{};
    std::uint64_t maximum_observed_absolute_offset_ns{};
    std::uint64_t continuous_lock_duration_ns{};
    bool hardware_timestamping_active{};
};

struct MemoryFacts final {
    std::uint64_t memlock_limit_bytes{};
    std::uint32_t hugepage_size_kib{};
    std::uint32_t reserved_hugepages{};
    std::uint32_t free_hugepages{};
    bool memlock_unlimited{};
};

struct LinkFacts final {
    std::uint64_t interface_id{};
    std::uint32_t speed_mbps{};
    std::uint16_t numa_node{};
    std::uint16_t rx_queues{};
    std::uint16_t tx_queues{};
    bool present{};
    bool up{};
    bool hardware_timestamping_active{};
    bool kernel_bypass_active{};
};

struct RecorderFacts final {
    RecorderDurability durability{RecorderDurability::Unknown};
    std::uint32_t capacity_batches{};
    std::uint32_t cancel_reservation{};
    std::uint32_t reconciliation_reservation{};
    std::uint64_t usable_bytes{};
    std::uint64_t qualified_append_bytes_per_second{};
    std::uint64_t qualified_append_records_per_second{};
    std::uint64_t measured_p99_999_completion_ns{};
    bool initialized{};
    bool mirrors_healthy{};
    bool peer_to_peer_qualified{};
    bool power_loss_qualified{};
};

struct NvmeFacts final {
    std::uint64_t namespace_id{};
    std::uint64_t capacity_bytes{};
    std::uint16_t numa_node{};
    std::uint16_t io_queues{};
    Digest256 firmware_digest{};
    bool present{};
    bool healthy{};
    bool power_loss_protection_active{};
};

struct BootFacts final {
    Digest256 pcr_policy_digest{};
    bool secure_boot_enabled{};
    bool measured_boot_valid{};
    bool tpm_present{};
};

struct PowerFacts final {
    CpuGovernor governor{CpuGovernor::Unknown};
    std::uint8_t maximum_enabled_c_state{0xffU};
};

struct ObservedPlatformFacts final {
    Digest256 applied_profile_digest{};
    TopologyFacts topology;
    FirmwareFacts firmware;
    PtpFacts ptp;
    MemoryFacts memory;
    std::array<LinkFacts, kMaximumReadinessLinks> links{};
    std::uint8_t link_count{};
    RecorderFacts recorder;
    NvmeFacts nvme;
    BootFacts boot;
    PowerFacts power;
    bool collection_complete{};
};

enum class FindingCode : std::uint8_t {
    ProfileShapeInvalid,
    ProfileSignatureNotVerified,
    ProfileDigestMismatch,
    FactsIncomplete,
    LogicalCpuCountMismatch,
    SocketCountMismatch,
    NumaCountMismatch,
    SmtEnabled,
    RealtimeCpuOffline,
    CpuIsolationMismatch,
    NoHzFullMismatch,
    RcuNocbsMismatch,
    IrqAffinityMismatch,
    RealtimeNumaMismatch,
    RealtimeMemoryNonlocal,
    SystemFirmwareMismatch,
    NicFirmwareMismatch,
    MicrocodeTooOld,
    FirmwareRollbackProtectionDisabled,
    PtpNotLocked,
    PtpGrandmasterMismatch,
    PtpOffsetExceeded,
    PtpLockDurationInsufficient,
    PtpHardwareTimestampingInactive,
    MemlockInsufficient,
    HugepageSizeMismatch,
    HugepageReservationInsufficient,
    HugepageAvailabilityInsufficient,
    LinkMissing,
    LinkDown,
    LinkSpeedInsufficient,
    LinkNumaMismatch,
    LinkQueueInsufficient,
    LinkHardwareTimestampingInactive,
    LinkKernelBypassInactive,
    RecorderNotInitialized,
    RecorderDurabilityInsufficient,
    RecorderMirrorUnhealthy,
    RecorderCapacityInsufficient,
    RecorderCancelReservationInsufficient,
    RecorderReconciliationReservationInsufficient,
    RecorderUsableBytesInsufficient,
    RecorderAppendBandwidthInsufficient,
    RecorderAppendRateInsufficient,
    RecorderCompletionLatencyExceeded,
    RecorderPeerToPeerUnqualified,
    RecorderPowerLossUnqualified,
    NvmeMissing,
    NvmeUnhealthy,
    NvmeNamespaceMismatch,
    NvmeCapacityInsufficient,
    NvmeNumaMismatch,
    NvmeQueueInsufficient,
    NvmeFirmwareMismatch,
    NvmePowerLossProtectionInactive,
    SecureBootDisabled,
    MeasuredBootInvalid,
    TpmMissing,
    PcrPolicyMismatch,
    GovernorMismatch,
    CStateLimitExceeded,
    FindingCapacityExceeded,
};

[[nodiscard]] constexpr std::uint64_t finding_mask(
    const FindingCode code) noexcept {
    const auto bit = static_cast<std::uint8_t>(code);
    return bit < 64U ? (std::uint64_t{1U} << bit) : 0U;
}

struct ReadinessFinding final {
    FindingCode code{FindingCode::ProfileShapeInvalid};
    std::uint64_t subject_id{};
    std::uint64_t expected{};
    std::uint64_t observed{};
    bool certification_blocking{true};
};

struct ReadinessReport final {
    ReadinessMode evaluated_mode{ReadinessMode::Production};
    std::array<ReadinessFinding, kMaximumReadinessFindings> findings{};
    std::uint16_t finding_count{};
    std::uint16_t total_findings{};
    bool findings_truncated{};
    bool certification_ready{true};
    bool production_ready{true};

    [[nodiscard]] bool eligible() const noexcept {
        return evaluated_mode == ReadinessMode::Production
                   ? production_ready
                   : certification_ready;
    }
};

// Pure, allocation-free evaluation. Signature cryptography and OS fact
// collection occur outside this boundary; their results are explicit inputs.
[[nodiscard]] ReadinessReport evaluate_readiness(
    const SignedReadinessProfile& profile,
    const ObservedPlatformFacts& facts,
    ReadinessMode mode) noexcept;

}  // namespace sorx::platform
