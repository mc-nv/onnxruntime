// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.
#include "core/common/cpuid_info.h"
#include "core/common/logging/logging.h"
#include "core/common/logging/severity.h"

#ifdef __linux__

#include <unistd.h>
#include <sys/syscall.h>
#if !defined(__NR_getcpu)
#include <asm-generic/unistd.h>
#endif

#if defined(CPUIDINFO_ARCH_ARM)

#include <sys/auxv.h>
#include <asm/hwcap.h>
// N.B. Support building with older versions of asm/hwcap.h that do not define
// this capability bit.
#ifndef HWCAP_ASIMDDP
#define HWCAP_ASIMDDP (1 << 20)
#endif

#ifndef HWCAP2_I8MM
#define HWCAP2_I8MM (1 << 13)
#endif

#ifndef HWCAP2_SVEI8MM
#define HWCAP2_SVEI8MM (1 << 9)
#endif

#ifndef HWCAP2_BF16
#define HWCAP2_BF16 (1 << 14)
#endif

#endif  // ARM

#endif  // Linux

#if _WIN32

#include <Windows.h>

#define HAS_WINDOWS_DESKTOP WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)

#ifndef PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE
#define PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE 43
#endif

#endif  // _WIN32

#if defined(CPUINFO_SUPPORTED)
#include <cpuinfo.h>
#if defined(CPUIDINFO_ARCH_ARM)
namespace onnxruntime {
// The following function is declared in "core/common/cpuid_uarch.h" but we cannot include the whole header file because
//  some of its symbols are conflict with <cpuinfo.h>
void decodeMIDR(uint32_t midr, uint32_t uarch[1]);
}  // namespace onnxruntime
#endif
#else
#include "core/common/cpuid_uarch.h"
#endif  // CPUINFO_SUPPORTED

#if defined(CPUIDINFO_ARCH_X86)
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__)
#include <cpuid.h>
#endif
#endif  // defined(CPUIDINFO_ARCH_X86)

namespace onnxruntime {

#if defined(CPUIDINFO_ARCH_X86)

static inline void GetCPUID(int function_id, int data[4]) {  // NOLINT
#if defined(_MSC_VER)
  __cpuid(reinterpret_cast<int*>(data), function_id);
#elif defined(__GNUC__)
  __cpuid(function_id, data[0], data[1], data[2], data[3]);
#endif
}

static inline void GetCPUID(int function_id, int sub_leaf, int data[4]) {  // NOLINT
#if defined(_MSC_VER)
  __cpuidex(reinterpret_cast<int*>(data), function_id, sub_leaf);
#elif defined(__GNUC__)
  __cpuid_count(function_id, sub_leaf, data[0], data[1], data[2], data[3]);
#endif
}

static inline int XGETBV() {
#if defined(_MSC_VER)
  return static_cast<int>(_xgetbv(0));
#elif defined(__GNUC__)
  int eax, edx;
  __asm__ volatile("xgetbv"
                   : "=a"(eax), "=d"(edx)
                   : "c"(0));
  return eax;
#endif
}

void CPUIDInfo::X86Init() {
  int data[4] = {-1};
  GetCPUID(0, data);

  vendor_ = GetX86Vendor(data);

  int num_IDs = data[0];
  if (num_IDs >= 1) {
    GetCPUID(1, data);
    if (data[2] & (1 << 27)) {
      constexpr int AVX_MASK = 0x6;
      constexpr int AVX512_MASK = 0xE6;
      int value = XGETBV();
      bool has_sse2 = (data[3] & (1 << 26));
      has_sse3_ = (data[2] & 0x1);
      has_sse4_1_ = (data[2] & (1 << 19));
      bool has_ssse3 = (data[2] & (1 << 9));
      has_avx_ = has_sse2 && has_ssse3 && (data[2] & (1 << 28)) && ((value & AVX_MASK) == AVX_MASK);
      bool has_avx512 = (value & AVX512_MASK) == AVX512_MASK;
      has_f16c_ = has_avx_ && (data[2] & (1 << 29)) && (data[3] & (1 << 26));

      if (num_IDs >= 7) {
        GetCPUID(7, data);
        const uint32_t max_SubLeaves = data[0];
        has_amx_bf16_ = (data[3] & (1 << 22));
        has_avx2_ = has_avx_ && (data[1] & (1 << 5));
        has_avx512f_ = has_avx512 && (data[1] & (1 << 16));
        // Add check for AVX512 Skylake since tensorization GEMM need intrinsics from avx512bw/avx512dq.
        // avx512_skylake = avx512f | avx512vl | avx512cd | avx512bw | avx512dq
        has_avx512_skylake_ = has_avx512 && (data[1] & ((1 << 16) | (1 << 17) | (1 << 28) | (1 << 30) | (1 << 31)));
        is_hybrid_ = (data[3] & (1 << 15));
        if (max_SubLeaves >= 1) {
          GetCPUID(7, 1, data);
          has_avx512_bf16_ = has_avx512 && (data[0] & (1 << 5));
        }
      }
    }
  }
}

std::string CPUIDInfo::GetX86Vendor(int32_t* data) {
  char vendor[sizeof(int32_t) * 3 + 1]{};
  *reinterpret_cast<int*>(vendor + 0) = data[1];
  *reinterpret_cast<int*>(vendor + 4) = data[3];
  *reinterpret_cast<int*>(vendor + 8) = data[2];
  return vendor;
}

#endif  // defined(CPUIDINFO_ARCH_X86)

#if defined(CPUIDINFO_ARCH_ARM)

#if defined(__linux__)

void CPUIDInfo::ArmLinuxInit() {
  // Assuming no hyper-threading, no NUMA groups
#if defined(CPUINFO_SUPPORTED)
  if (pytorch_cpuinfo_init_) {
    is_hybrid_ = cpuinfo_get_uarchs_count() > 1;
    has_arm_neon_dot_ = cpuinfo_has_arm_neon_dot();
    has_fp16_ = cpuinfo_has_arm_neon_fp16_arith();
    has_arm_neon_i8mm_ = cpuinfo_has_arm_i8mm();
    has_arm_sve_i8mm_ = cpuinfo_has_arm_sve() && cpuinfo_has_arm_i8mm();
    has_arm_neon_bf16_ = cpuinfo_has_arm_neon_bf16();

    const uint32_t core_cnt = cpuinfo_get_cores_count();
    core_uarchs_.resize(core_cnt, cpuinfo_uarch_unknown);
    is_armv8_narrow_ld_.resize(core_cnt, false);
    for (uint32_t c = 0; c < core_cnt; c++) {
      const struct cpuinfo_processor* proc = cpuinfo_get_processor(c);
      if (proc == nullptr) {
        continue;
      }
      const struct cpuinfo_core* corep = proc->core;
      if (corep == nullptr) {
        continue;
      }
      auto coreid = proc->linux_id;
      auto uarch = corep->uarch;
      core_uarchs_[coreid] = uarch;
      if (uarch == cpuinfo_uarch_cortex_a53 || uarch == cpuinfo_uarch_cortex_a55r0 ||
          uarch == cpuinfo_uarch_cortex_a55) {
        is_armv8_narrow_ld_[coreid] = true;
      }
    }
  } else
#endif  // defined(CPUINFO_SUPPORTED)
  {
    has_arm_neon_dot_ = ((getauxval(AT_HWCAP) & HWCAP_ASIMDDP) != 0);
    has_fp16_ |= has_arm_neon_dot_;

    has_arm_neon_i8mm_ = ((getauxval(AT_HWCAP2) & HWCAP2_I8MM) != 0);
    has_arm_sve_i8mm_ = ((getauxval(AT_HWCAP2) & HWCAP2_SVEI8MM) != 0);

    has_arm_neon_bf16_ = ((getauxval(AT_HWCAP2) & HWCAP2_BF16) != 0);
  }
}

#elif defined(_WIN32)  // ^ defined(__linux__)

void CPUIDInfo::ArmWindowsInit() {
  // Get the ARM vendor string from the registry
  vendor_ = GetArmWindowsVendor();

// ARM32 certainly doesn't have fp16, so we will skip the logic to avoid using RegGetValueA Windows API
#if !defined(_M_ARM)
#pragma region Application Family or OneCore Family
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP | WINAPI_PARTITION_SYSTEM)
  // Read MIDR from windows registry
  // TODO!! Don't support multiple processor group yet!!
  constexpr int MAX_CORES = 64;
  constexpr int MAX_VALUE_NAME = 4096;

  CHAR midrKey[MAX_VALUE_NAME] = "";  // buffer for processor registry name
  uint32_t lastUarch = cpuinfo_uarch_unknown;
  for (int i = 0; i < MAX_CORES - 1; i++) {
    snprintf(midrKey, MAX_VALUE_NAME, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\%d", i);
    uint64_t midrVal;
    unsigned long midrSize = sizeof(uint64_t);

    /*
     * ARM lists for each coprocessor register 5 fields: op0/op1/CRn/CRm/op2.
     * You need to put those numbers through the ARM64_SYSREG macro:
     *
     * #define ARM64_SYSREG(op0, op1, crn, crm, op2) \
     *    (((op0 & 1) << 14) |                       \
     *     ((op1 & 7) << 11) |                       \
     *     ((crn & 15) << 7) |                       \
     *     ((crm & 15) << 3) |                       \
     *     ((op2 & 7) << 0))
     *
     * For the CP value of MIDR, op0 = 3 and the others are all = 0, so we come up with 0x4000,
     */
    auto retCode = ::RegGetValueA(HKEY_LOCAL_MACHINE, midrKey, "CP 4000", RRF_RT_REG_QWORD, nullptr, &midrVal, &midrSize);
    if (retCode != ERROR_SUCCESS) {
      break;
    }
    uint32_t uarch = cpuinfo_uarch_unknown;
    decodeMIDR((uint32_t)midrVal, &uarch);
    core_uarchs_.push_back(uarch);
    if (uarch == cpuinfo_uarch_cortex_a53 || uarch == cpuinfo_uarch_cortex_a55r0 ||
        uarch == cpuinfo_uarch_cortex_a55) {
      is_armv8_narrow_ld_.push_back(true);
    } else {
      is_armv8_narrow_ld_.push_back(false);
    }

    if (i == 0) {
      lastUarch = uarch;
    } else if (lastUarch != uarch) {
      is_hybrid_ = true;
      lastUarch = uarch;
    }
  }
#endif  // WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_APP | WINAPI_PARTITION_SYSTEM)

  has_arm_neon_dot_ = (IsProcessorFeaturePresent(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE) != 0);
#else   // ^ !defined(_M_ARM) / v defined(_M_ARM)
  has_arm_neon_dot_ = false;
#endif  // defined(_M_ARM)

#if defined(CPUINFO_SUPPORTED)
  if (pytorch_cpuinfo_init_) {
    has_fp16_ = cpuinfo_has_arm_neon_fp16_arith();
    has_arm_neon_i8mm_ = cpuinfo_has_arm_i8mm();
    has_arm_sve_i8mm_ = cpuinfo_has_arm_sve() && cpuinfo_has_arm_i8mm();
    has_arm_neon_bf16_ = cpuinfo_has_arm_neon_bf16();
  } else
#endif  // defined(CPUINFO_SUPPORTED)
  {
    has_fp16_ = false;
    has_arm_neon_i8mm_ = false;
    has_arm_sve_i8mm_ = false;
    has_arm_neon_bf16_ = false;
  }
}

std::string CPUIDInfo::GetArmWindowsVendor() {
  const int MAX_VALUE_NAME = 256;
  const CHAR vendorKey[] = "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0";
  CHAR vendorVal[MAX_VALUE_NAME] = "";
  unsigned long vendorSize = sizeof(char) * MAX_VALUE_NAME;
  ::RegGetValueA(HKEY_LOCAL_MACHINE, vendorKey, "Vendor Identifier", RRF_RT_REG_SZ | RRF_ZEROONFAILURE, nullptr, &vendorVal, &vendorSize);
  return vendorVal;
}

#elif defined(__APPLE__)  // ^ defined(_WIN32)

void CPUIDInfo::ArmAppleInit() {
#if defined(CPUINFO_SUPPORTED)
  if (pytorch_cpuinfo_init_) {
    is_hybrid_ = cpuinfo_get_uarchs_count() > 1;
    has_arm_neon_dot_ = cpuinfo_has_arm_neon_dot();
    has_fp16_ = cpuinfo_has_arm_neon_fp16_arith();
    has_arm_neon_i8mm_ = cpuinfo_has_arm_i8mm();
    has_arm_sve_i8mm_ = cpuinfo_has_arm_sve() && cpuinfo_has_arm_i8mm();
    has_arm_neon_bf16_ = cpuinfo_has_arm_neon_bf16();

    // Note: We leave is_armv8_narrow_ld_ unset because it only applies to a limited set of uarchs that we don't expect
    // to encounter on Apple platforms.

    // TODO figure out how to set core_uarchs_
  } else
#endif  // defined(CPUINFO_SUPPORTED)
  {
    // No fallback detection attempted now. Add if needed.
  }
}

#endif  // defined(__APPLE__)

#endif  // defined(CPUIDINFO_ARCH_ARM)

uint32_t CPUIDInfo::GetCurrentCoreIdx() const {
#ifdef _WIN32
  return GetCurrentProcessorNumber();
#elif defined(__linux__)
  uint32_t coreIdx = 0xFFFFFFFF;
  if (syscall(__NR_getcpu, &coreIdx, NULL, NULL) != 0) {
    // failed to detect current core id. give up
    return 0xFFFFFFFF;
  }
  return coreIdx;
#else
  return 0xFFFFFFFF;  // don't know how to get core index
#endif
}

CPUIDInfo::CPUIDInfo() {
#ifdef CPUIDINFO_ARCH_X86
  X86Init();
#elif defined(CPUIDINFO_ARCH_ARM)
#if defined(CPUINFO_SUPPORTED)
  pytorch_cpuinfo_init_ = cpuinfo_initialize();
  if (!pytorch_cpuinfo_init_) {
    LOGS_DEFAULT(WARNING) << "Failed to initialize PyTorch cpuinfo library. May cause CPU EP performance degradation "
                             "due to undetected CPU features.";
  }
#endif  // defined(CPUINFO_SUPPORTED)
#if defined(__linux__)
  ArmLinuxInit();
#elif defined(_WIN32)
  ArmWindowsInit();
#elif defined(__APPLE__)
  ArmAppleInit();
#endif
#endif  // defined(CPUIDINFO_ARCH_ARM)
}
}  // namespace onnxruntime
