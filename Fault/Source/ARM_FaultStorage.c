/*
 * Copyright (c) 2022-2023 Arm Limited. All rights reserved.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the License); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an AS IS BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//lint -e46  "Suppress: field type should be _Bool, unsigned int or signed int [MISRA 2012 Rule 6.1, required]"
//lint -e451 "Suppress: repeatedly included but does not have a standard include guard [MISRA 2012 Directive 4.10, required]"
//lint -e537 "Suppress: Repeated include file 'stddef.h'"
//lint -esym(9058, cmse_address_info) "Suppress: tag 'cmse_address_info' unused outside of typedefs [MISRA 2012 Rule 2.4, advisory]"

#include "ARM_Fault.h"

#include <stddef.h>
#include <string.h>

// Compiler-specific defines
#if !defined(__NAKED)
  //lint -esym(9071, __NAKED) "Suppress: defined macro is reserved to the compiler"
  #define __NAKED __attribute__((naked))
#endif
#if !defined(__WEAK)
  //lint -esym(9071, __WEAK) "Suppress: defined macro is reserved to the compiler"
  #define __WEAK __attribute__((weak))
#endif
#if !defined(__NO_INIT)
  //lint -esym(9071, __NO_INIT) "Suppress: defined macro is reserved to the compiler"
  #if   defined (__CC_ARM)                                           /* ARM Compiler 4/5 */
    #define __NO_INIT __attribute__ ((section (".bss.noinit"), zero_init))
  #elif defined (__ARMCC_VERSION) && (__ARMCC_VERSION >= 6010050)    /* ARM Compiler 6 */
    #define __NO_INIT __attribute__ ((section (".bss.noinit")))
  #elif defined (__GNUC__)                                           /* GNU Compiler */
    #define __NO_INIT __attribute__ ((section (".noinit")))
  #elif defined (__ICCARM__)                                         /* IAR Compiler */
    #define __NO_INIT __attribute__ ((section (".noinit")))
  #else
    #warning "No compiler specific solution for __NO_INIT. __NO_INIT is ignored."
    #define __NO_INIT
  #endif
#endif

// Armv8/8.1-M architecture related defines
#if    (ARM_FAULT_ARCH_ARMV8x_M != 0)
#define ARM_FAULT_ASC_INTEGRITY_SIG    (0xFEFA125AU)    // Additional State Context Integrity Signature
#endif

#if    (ARM_FAULT_FAULT_REGS_EXIST != 0)
// Define CFSR mask for detecting state context stacking failure
#ifndef SCB_CFSR_Stack_Err_Msk
#ifdef  SCB_CFSR_STKOF_Msk
#define SCB_CFSR_Stack_Err_Msk         (SCB_CFSR_STKERR_Msk | SCB_CFSR_MSTKERR_Msk | SCB_CFSR_STKOF_Msk)
#else
#define SCB_CFSR_Stack_Err_Msk         (SCB_CFSR_STKERR_Msk | SCB_CFSR_MSTKERR_Msk)
#endif
#endif
#endif

// Fault information definitions
#define ARM_FAULT_MAGIC_NUMBER         (0x52746C46U)    // ARM Fault Magic number (ASCII "FltR")
#define ARM_FAULT_CRC32_INIT_VAL       (0xFFFFFFFFU)    // ARM Fault CRC-32 initial value
#define ARM_FAULT_CRC32_POLYNOM        (0x04C11DB7U)    // ARM Fault CRC-32 polynom

// Fault component version information
const char ARM_FaultVersion[] __USED = ARM_FAULT_VERSION;

// Fault information
ARM_FaultInfo_t ARM_FaultInfo __USED __NO_INIT;

// Local function prototype
static uint32_t CalcCRC32 (      uint32_t init_val,
                           const uint8_t *data_ptr,
                                 uint32_t data_len,
                                 uint32_t polynom);

// ARM Fault Storage functions -------------------------------------------------

/**
  Clear the saved fault information.
*/
void ARM_FaultClear (void) {
  memset(&ARM_FaultInfo, 0, sizeof(ARM_FaultInfo));
}

/**
  Check if the fault occurred and if the fault information was saved properly.
  \return       status (1 = fault occurred and valid fault information exists,
                        0 = no fault information saved yet or fault information is invalid)
*/
uint32_t ARM_FaultOccurred (void) {
  uint32_t fault_info_valid = 1U;

  // Check if magic number is valid
  if (ARM_FaultInfo.magic_number != ARM_FAULT_MAGIC_NUMBER) {
    fault_info_valid = 0U;
  }

  // Check if CRC of the ARM_FaultInfo structure is valid
  if (fault_info_valid != 0U) {
    if (ARM_FaultInfo.crc32 != CalcCRC32(ARM_FAULT_CRC32_INIT_VAL,
                                        (const uint8_t *)&ARM_FaultInfo.count,
                                        (sizeof(ARM_FaultInfo) - (sizeof(ARM_FaultInfo.magic_number) + sizeof(ARM_FaultInfo.crc32))),
                                         ARM_FAULT_CRC32_POLYNOM)) {
      fault_info_valid = 0U;
    }
  }

  return fault_info_valid;
}

/**
  Save the fault information.
  Must be called from fault handler with preserved Link Register value and unchanged
  Stack Pointer, typically by branching to this function.
*/
__NAKED void ARM_FaultSave (void) {
  //lint -efunc(10,  ARM_FaultSave) "Suppress: expecting ';'"
  //lint -efunc(522, ARM_FaultSave) "Suppress: Warning 522: Highest operation, a 'constant', lacks side-effects [MISRA 2012 Rule 2.2, required]"

  /* This function contains 3 ASM blocks because of the GCC limitation that the 'asm' supports up to 30 operands.
     Also there is ARM Compiler 6 warning if string literal exceeds maximum length 4095 that ISO C99 compilers are required to support. */
  __ASM volatile (
#ifndef __ICCARM__
    ".syntax unified\n"
#endif

 /* --- Read current ARM_FaultInfo.count value --- */
    "ldr   r2,  =%c[count_addr]\n"          // Load count address
    "ldr   r3,  [r2]\n"                     // Load count value

 /* --- Clear ARM_FaultInfo structure --- */
    "movs  r0,  #0\n"                       // R0 = 0
    "ldr   r1,  =%c[ARM_FaultInfo_addr]\n"  // R1 = &ARM_FaultInfo
    "movs  r2,  %[ARM_FaultInfo_size]\n"    // R2 = sizeof(ARM_FaultInfo)/4
    "b     is_clear_done\n"
  "clear_uint32:\n"
    "stm   r1!, {r0}\n"
    "subs  r2,  r2, #1\n"
  "is_clear_done:\n"
    "bne   clear_uint32\n"

 /* --- Increment and store new ARM_FaultInfo.count value --- */
    "ldr   r2,  =%c[count_addr]\n"          // Load count address
    "adds  r3,  r3, #1\n"                   // Increment count value
    "stm   r2!, {r3}\n"                     // Store new count value

 /* --- Store ARM_FaultInfo.info: version and compile-time content information --- */
    "ldr   r0,  =%c[info_val]\n"            // Load  info value
    "str   r0,  [r2]\n"                     // Store info value

 /* --- Store current values of registers R4 .. R11 into ARM_FaultInfo --- */
 /* After R4 .. R11 are stored they can be used in the code since they will be restored at the end of this function */
    "ldr   r2,  =%c[r4_addr]\n"             // R2 = &ARM_FaultInfo.R4
    "stm   r2!, {r4-r7}\n"                  // Store R4 .. R7
    "mov   r4,  r8\n"                       // R4 = R8
    "mov   r5,  r9\n"                       // R5 = R9
    "mov   r6,  r10\n"                      // R6 = R10
    "mov   r7,  r11\n"                      // R7 = R11
    "stm   r2!, {r4-r7}\n"                  // Store R8 .. R11

 /* --- Determine stack used, and for TrustZone devices Secure or Non-Secure registers used --- */

 /* Determine the beginning of the state context or the additional state context
    (for device with TruztZone) that was stacked upon exception entry and put that
    address into R6 register.
    For device with TrustZone, also determine if state context was pushed from
    Non-Secure World but the exception handling is happening in the Secure World
    and if so, mark it by setting bit [0] of the R7 register to value 1, thus indicating
    usage of Non-Secure aliases.

    after this section:
      R6          == start of state context or additional state context if that was pushed also
      R7 bit [0]: == 0 - no access to Non-Secure aliases or device without TrustZone
                  == 1 -    access to Non-Secure aliases

    Determination is done by analyzing EXC_RETURN (Link Register):
    EXC_RETURN:
      - bit [6] (S):            only on device with TrustZone
                         == 0 - Non-Secure stack was used
                         == 1 - Secure     stack was used
      - bit [5] (DCRS):         only on device with TrustZone
                         == 0 - additional state context was also stacked
                         == 1 - only basic state context was stacked
      - bit [2] (SPSEL): == 0 - Main    Stack Pointer (MSP) was used for stacking on exception entry
                         == 1 - Process Stack Pointer (PSP) was used for stacking on exception entry */
    "movs  r6,  #0\n"                       // R6 = 0
    "movs  r7,  #0\n"                       // R7 = 0

    "mov   r0,  lr\n"                       // R0 = LR (EXC_RETURN)
    "lsrs  r0,  r0, #3\n"                   // Shift bit [2] (SPSEL) into Carry flag
    "bcc   msp_used\n"                      // If    bit [2] (SPSEL) == 0, MSP or MSP_NS was used
                                            // If    bit [2] (SPSEL) == 1, PSP or PSP_NS was used
  "psp_used:\n"
#if (ARM_FAULT_TZ_SECURE != 0)              // If code was compiled for and is running in Secure World
    "mov   r0,  lr\n"                       // R0 = LR (EXC_RETURN)
    "lsrs  r0,  r0, #7\n"                   // Shift   bit [6] (S) into Carry flag
    "bcs   load_psp\n"                      // If      bit [6] (S) == 1, jump to load PSP
  "load_psp_ns:\n"                          // else if bit [6] (S) == 0, load PSP_NS
    "mrs   r6,  psp_ns\n"                   // R6 = PSP_NS
    "movs  r7,  #1\n"                       // R7 = 1
    "b     r6_points_to_stack\n"            // PSP_NS loaded to R6, exit section
  "load_psp:\n"
#endif
    "mrs   r6,  psp\n"                      // R6 = PSP
    "b     r6_points_to_stack\n"            // PSP loaded to R6, exit section

  "msp_used:\n"
#if (ARM_FAULT_TZ_SECURE != 0)              // If code was compiled for and is running in Secure World
    "mov   r0,  lr\n"                       // R0 = LR (EXC_RETURN)
    "lsrs  r0,  r0, #7\n"                   // Shift   bit [6] (S) into Carry flag
    "bcs   load_msp\n"                      // If      bit [6] (S) == 1, jump to load MSP
  "load_msp_ns:\n"                          // else if bit [6] (S) == 0, load MSP_NS
    "mrs   r6,  msp_ns\n"                   // R6 = MSP_NS
    "movs  r7,  #1\n"                       // R7 = 1
    "b     r6_points_to_stack\n"            // MSP_NS loaded to R6, exit section
  "load_msp:\n"
#endif
    "mrs   r6,  msp\n"                      // R6 = MSP
    "b     r6_points_to_stack\n"            // MSP loaded to R6, exit section

  "r6_points_to_stack:\n"

    /* Set ARM_FaultInfo.info.content.tz_fault_mode to 1 if the fault happened in the Secure World --- */
#if (ARM_FAULT_TZ_SECURE != 0)              // If code was compiled for and is running in Secure World
    "lsrs  r4,  r7, #1\n"                   // Shift   bit [0] of R7 into Carry flag
    "bcs   tz_fault_mode_end\n"             // If      bit [0] of R7 == 1, do not set tz_fault_mode bit
  "set_tz_fault_mode:\n"                    // else if bit [0] of R7 == 0,        set tz_fault_mode bit
    "ldr   r0,  =%c[info_addr]\n"           // Load info address
    "ldr   r1,  [r0]\n"                     // Load info value
    "ldr   r2,  =%c[info_tz_fault_mode_msk]\n"  // Load tz_fault_mode bit mask
    "orrs  r1,  r2\n"                       // OR it with content of the info
    "str   r1,  [r0]\n"                     // Store updated info value back
  "tz_fault_mode_end:\n"
#endif

 /* --- Determine stack validity --- */

 /* Determine if stack is valid.
    If stack pointer value is not 0 and if no stacking fault has occurred.
    If stack information is not valid then mark it by setting bit [1] of the R7 to value 1.
    Note: for Armv6-M and Armv8-M Baseline CFSR register is not available, so stack is
          considered valid although it might not always be so. */
    "cmp   r6,  #0\n"                       // Compare stack pointer to 0
    "bne   check_CFSR\n"                    // If      stack pointer != 0, branch to check CFSR register flags
  "stack_pointer_is_zero:\n"                // else if stack pointer == 0, stack information is invalid
    "movs  r3,  #2\n"
    "orrs  r7,  r3\n"                       // R7 |= (1 << 1)
    "b     stack_check_end\n"               // Exit stack checking
  "check_CFSR:\n"
#if (ARM_FAULT_FAULT_REGS_EXIST != 0)       // If fault registers exist
#if (ARM_FAULT_TZ_SECURE != 0)              // If code was compiled for and is running in Secure World
    "lsrs  r0,  r7, #1\n"                   // Shift   bit [0] of R7 into Carry flag
    "bcc   load_cfsr_addr\n"                // If      bit [0] of R7 == 0, jump to load CFSR register address
  "load_cfsr_ns_addr:\n"                    // else if bit [0] of R7 == 1, load CFSR_NS register address
    "ldr   r2,  =%c[cfsr_ns_addr]\n"        // R2 = CFSR_NS address
    "b     load_cfsr_val\n"
  "load_cfsr_addr:\n"
#endif
    "ldr   r2,  =%c[cfsr_addr]\n"           // R2 = CFSR address
  "load_cfsr_val:\n"
    "ldr   r0,  [r2]\n"                     // R0 = CFSR (or CFSR_NS) register value
    "ldr   r1,  =%c[cfsr_err_msk]\n"        // R1 = SCB_CFSR_Stack_Err_Msk
    "ands  r0,  r1\n"                       // Mask CFSR value with stacking error bits
    "beq   stack_check_end\n"               // If   no stacking error (== 0), jump to stack_check_end
  "stack_info_is_invalid:\n"                // else if stacking error (!= 0), stack information is invalid
    "movs  r3,  #2\n"                       // R3  =  1 << 1
    "orrs  r7,  r3\n"                       // R7 |= (1 << 1)
#endif
  "stack_check_end:\n"

 /* --- Store stacked context into ARM_FaultInfo (R0 .. xPSR; R4 .. R11 only if additional state context exists) --- */

 /* Check if state context (also additional state context if it exists (on TrustZone only)) is valid and
    if it is then copy it into ARM_FaultInfo */
    "lsrs  r0,  r7, #2\n"                   // Shift bit [1] of R7 into Carry flag
    "bcs   state_context_end\n"             // If stack is not valid (bit == 1), skip copying information from stack

#if (ARM_FAULT_ARCH_ARMV8x_M != 0)          // If arch is Armv8/8.1-M
 /* If additional state context was stacked upon exception entry, copy IntegritySignature and R4 .. R11 into ARM_FaultInfo */
 /* Content of additional state context on stack is:
    Integrity signature, Reserved, R4, R5, R6, R7, R8, R9, R10, R11 */
    "mov   r0,  lr\n"                       // R0 = LR (EXC_RETURN)
    "lsrs  r0,  r0, #6\n"                   // Shift   bit [5] (DCRS) into Carry flag
    "bcs   additional_context_end\n"        // If      bit [5] (DCRS) == 1, skip additional state context
                                            // else if bit [5] (DCRS) == 0, copy additional state context
    "ldr   r5,  =%c[IntegritySignature_addr]\n"     // Load IntegritySignature address
    "ldm   r6!, {r0, r1}\n"                 // Load stacked IntegritySignature, Reserved
    "str   r0,  [r5]\n"                     // Store        IntegritySignature value only
    "ldr   r5,  =%c[r4_addr]\n"             // Load R4 address
    "ldm   r6!, {r0-r3}\n"                  // Load stacked R4 .. R7
    "stm   r5!, {r0-r3}\n"                  // Store        R4 .. R7
    "ldm   r6!, {r0-r3}\n"                  // Load stacked R8 .. R11
    "stm   r5!, {r0-r3}\n"                  // Store        R8 .. R11

  "additional_context_end:\n"
#endif

 /* Copy state context stacked upon exception entry into ARM_FaultInfo */
 /* Content of state context (Basic Stack Frame) on stack is:
    R0, R1, R2, R3, R12, LR (R14), ReturnAddress, xPSR */
    "ldr   r5,  =%c[r0_addr]\n"             // Load R0 address
    "ldm   r6!, {r0-r3}\n"                  // Load stacked R0 .. R3
    "stm   r5!, {r0-r3}\n"                  // Store        R0 .. R3
    "ldr   r5,  =%c[r12_addr]\n"            // Load R12 address
    "ldm   r6!, {r0-r3}\n"                  // Load stacked R12 .. xPSR
    "stm   r5!, {r0-r3}\n"                  // Store        R12 .. xPSR

    /* Set ARM_FaultInfo.info.content.state_context to 1 --- */
    "ldr   r0,  =%c[info_addr]\n"           // Load info address
    "ldr   r1,  [r0]\n"                     // Load info value
    "ldr   r2,  =%c[info_state_context_msk]\n" // Load state_context bit mask
    "orrs  r1,  r2\n"                       // OR it with content of the info
    "str   r1,  [r0]\n"                     // Store updated info value back

  "state_context_end:\n"

 /* --- Store EXC_xPSR, EXC_RETURN, MSP, PSP into ARM_FaultInfo --- */
    "mrs   r0,  xpsr\n"                     // R0 = current xPSR
    "mov   r1,  lr\n"                       // R1 = current LR (exception return code)
#if (ARM_FAULT_TZ_SECURE != 0)              // If code was compiled for and is running in Secure World
    "lsrs  r4,  r7, #1\n"                   // Shift   bit [0] of R7 into Carry flag
    "bcc   load_sps\n"                      // If      bit [0] of R7 == 0, jump to load MSP and PSP
  "load_sps_ns:\n"                          // else if bit [0] of R7 == 1, load MSP_NS and PSP_NS
    "mrs   r2,  msp_ns\n"                   // R0 = current MSP_NS
    "mrs   r3,  psp_ns\n"                   // R1 = current PSP_NS
    "b     store_regs\n"
#endif
  "load_sps:\n"
    "mrs   r2,  msp\n"                      // R0 = current MSP
    "mrs   r3,  psp\n"                      // R1 = current PSP
  "store_regs:\n"
    "ldr   r5,  =%c[EXC_xPSR_addr]\n"       // Load  EXC_xPSR address
    "stm   r5!, {r0-r3}\n"                  // Store EXC_xPSR, EXC_RETURN, MSP, PSP

 /* --- Store MSPLIM and PSPLIM (if they are available) into ARM_FaultInfo --- */
 /* Armv8-M Baseline does not have MSPLIM_NS and PSPLIM_NS */
#if (ARM_FAULT_ARCH_ARMV8x_M != 0)          // If arch is Armv8/8.1-M
#if (ARM_FAULT_TZ_SECURE != 0)              // If code was compiled for and is running in Secure World
    "lsrs  r4,  r7, #1\n"                   // Shift   bit [0] of R7 into Carry flag
    "bcc   load_splims\n"                   // If      bit [0] of R7 == 0, jump to load MSPLIM and PSPLIM
#if (ARM_FAULT_ARCH_ARMV8_M_BASE != 0)      // If arch is Armv8-M Baseline
    "b     splims_end\n"                    // MSPLIM_NS and PSPLIM_NS do not exist, skip storing them
#else                                       // Else if arch is Armv8/8.1-M Mainline
  "load_splims_ns:\n"                       // else if bit [0] of R7 == 1, load MSPLIM_NS and PSPLIM_NS
    "mrs   r0,  msplim_ns\n"                // R0 = current MSPLIM_NS
    "mrs   r1,  psplim_ns\n"                // R1 = current PSPLIM_NS
    "b     store_splims\n"
#endif
#endif
  "load_splims:\n"
    "mrs   r0,  msplim\n"                   // R0 = current MSP
    "mrs   r1,  psplim\n"                   // R1 = current PSP
  "store_splims:\n"
    "stm   r5!, {r0, r1}\n"                 // Store MSPLIM (or MSPLIM_NS) and PSPLIM (or PSPLIM_NS)

    /* Set ARM_FaultInfo.info.content.limit_regs to 1 --- */
    "ldr   r0,  =%c[info_addr]\n"           // Load info address
    "ldr   r1,  [r0]\n"                     // Load info value
    "ldr   r2,  =%c[info_limit_regs_msk]\n" // Load limit_regs bit mask
    "orrs  r1,  r2\n"                       // OR it with content of the info
    "str   r1,  [r0]\n"                     // Store updated info value back

  "splims_end:\n"
#endif

 /* Inline assembly template operands */
 :  /* no outputs */
 :  /* inputs */
    [ARM_FaultInfo_addr]                "i" (&ARM_FaultInfo)
 ,  [ARM_FaultInfo_size]                "i" (sizeof(ARM_FaultInfo)/4)
 ,  [count_addr]                        "i" (&ARM_FaultInfo.count)
 ,  [info_addr]                         "i" (&ARM_FaultInfo.info)
 ,  [info_val]                          "i" (ARM_FAULT_FAULT_INFO_VER_MINOR
                                         |  (ARM_FAULT_FAULT_INFO_VER_MAJOR << 8)
                                         |  (ARM_FAULT_FAULT_REGS_EXIST     << 16)
                                         |  (ARM_FAULT_ARCH_ARMV8x_M_MAIN   << 17)
                                         |  (ARM_FAULT_TZ_ENABLED           << 18)
                                         |  (ARM_FAULT_TZ_SECURE            << 19))
 ,  [info_tz_fault_mode_msk]            "i" (1U << 20)
 ,  [info_state_context_msk]            "i" (1U << 21)
 ,  [info_limit_regs_msk]               "i" (1U << 22)
 ,  [r0_addr]                           "i" (&ARM_FaultInfo.R0)
 ,  [r4_addr]                           "i" (&ARM_FaultInfo.R4)
 ,  [r12_addr]                          "i" (&ARM_FaultInfo.R12)
 ,  [IntegritySignature_addr]           "i" (&ARM_FaultInfo.IntegritySignature)
 ,  [EXC_xPSR_addr]                     "i" (&ARM_FaultInfo.EXC_xPSR)
#if (ARM_FAULT_FAULT_REGS_EXIST != 0)
 ,  [cfsr_err_msk]                      "i" (SCB_CFSR_Stack_Err_Msk)
 ,  [cfsr_addr]                         "i" (SCB_BASE + offsetof(SCB_Type, CFSR))
#if (ARM_FAULT_TZ_SECURE != 0)
 ,  [cfsr_ns_addr]                      "i" (SCB_BASE_NS + offsetof(SCB_Type, CFSR))
#endif
#endif
 :  /* clobber list */
    "r0", "r1", "r2", "r3", "cc", "memory");

#if (ARM_FAULT_FAULT_REGS_EXIST != 0)       // If fault registers exist
  __ASM volatile (
#ifndef __ICCARM__
    ".syntax unified\n"
#endif

 /* --- Store Fault Registers (if they are available) into ARM_FaultInfo --- */
#if (ARM_FAULT_TZ_SECURE != 0)              // If code was compiled for and is running in Secure World
    "lsrs  r4,  r7, #1\n"                   // Shift   bit [0] of R7 into Carry flag
    "bcc   load_scb_addr\n"                 // If      bit [0] of R7 == 0, jump to load SCB BASE address
  "load_scb_ns_addr:\n"                     // else if bit [0] of R7 == 1, load SCB_NS BASE address
    "ldr   r4,  =%c[scb_ns_base_addr]\n"    // Load SCB_NS BASE address
    "b     load_fault_regs\n"
  "load_scb_addr:\n"
#endif
    "ldr   r4,  =%c[scb_base_addr]\n"       // Load SCB BASE address
  "load_fault_regs:\n"
    "ldr   r5,  =%c[csfr_reg_addr]\n"       // Load fault registers start address (CSFR) in ARM_FaultInfo
    "ldr   r0,  [r4, %[cfsr_ofs]]\n"        // R0 = SCB_CFSR
    "ldr   r1,  [r4, %[hfsr_ofs]]\n"        // R1 = SCB_HFSR
    "ldr   r2,  [r4, %[dfsr_ofs]]\n"        // R2 = SCB_DFSR
    "ldr   r3,  [r4, %[mmfar_ofs]]\n"       // R3 = SCB_MMFAR
    "stm   r5!, {r0-r3}\n"                  // Store CFSR, HFSR, DFSR and MMFAR
    "ldr   r0,  [r4, %[bfar_ofs]]\n"        // R0 = SCB_BFSR
    "ldr   r1,  [r4, %[afsr_ofs]]\n"        // R1 = SCB_AFSR
    "stm   r5!, {r0, r1}\n"                 // Store BFSR and AFSR

    /* Set ARM_FaultInfo.info.content.fault_regs to 1 --- */
    "ldr   r0,  =%c[info_addr]\n"           // Load info address
    "ldr   r1,  [r0]\n"                     // Load info value
    "ldr   r2,  =%c[info_fault_regs_msk]\n" // Load fault_regs bit mask
    "orrs  r1,  r2\n"                       // OR it with content of the info
    "str   r1,  [r0]\n"                     // Store updated info value back

 /* --- Armv8.1-M Mainline RAS Fault Status Register (RFSR) --- */
#if (ARM_FAULT_ARCH_ARMV8_1M_MAIN != 0)     // If arch is Armv8.1-M Mainline
    "ldr   r5,  =%c[rfsr_reg_addr]\n"       // Load address of RFSR register in ARM_FaultInfo
    "ldr   r0,  [r4, %[rfsr_ofs]]\n"        // R0 = SCB_RFSR
    "str   r0,  [r5]\n"                     // Store RFSR

    /* Set ARM_FaultInfo.info.content.ras_fault_reg to 1 --- */
    "ldr   r0,  =%c[info_addr]\n"           // Load info address
    "ldr   r1,  [r0]\n"                     // Load info value
    "ldr   r2,  =%c[info_ras_fault_reg_msk]\n"  // Load value for setting ras_fault_reg bit
    "orrs  r1,  r2\n"                       // OR it with content of the info
    "str   r1,  [r0]\n"                     // Store updated info value back
#endif

 /* --- Armv8/8.1-M Mainline Fault Registers --- */
 /* Store values of Armv8/8.1-M Fault Registers (Mainline only) if code is running in Secure World into ARM_FaultInfo */
#if (ARM_FAULT_ARCH_ARMV8x_M_MAIN != 0)     // If arch is Armv8-M Mainline
#if (ARM_FAULT_TZ_SECURE != 0)              // If code was compiled for and is running in Secure World
    "ldr   r5,  =%c[sfsr_reg_addr]\n"       // Load address of SFSR register in ARM_FaultInfo
    "ldr   r4,  =%c[scb_base_addr]\n"       // Load SCB BASE address
    "ldr   r0,  [r4, %[sfsr_ofs]]\n"        // R0 = SFSR
    "ldr   r1,  [r4, %[sfar_ofs]]\n"        // R1 = SFAR
    "stm   r5!, {r0, r1}\n"                 // Store SFSR and SFAR

    /* Set ARM_FaultInfo.info.content.secure_fault_regs to 1 --- */
    "ldr   r0,  =%c[info_addr]\n"           // Load info address
    "ldr   r1,  [r0]\n"                     // Load info value
    "ldr   r2,  =%c[info_secure_fault_regs_msk]\n"  // Load value for setting secure_fault_regs bit
    "orrs  r1,  r2\n"                       // OR it with content of the info
    "str   r1,  [r0]\n"                     // Store updated info value back
#endif
#endif

 /* Inline assembly template operands */
 :  /* no outputs */
 :  /* inputs */
    [info_addr]                         "i" (&ARM_FaultInfo.info)
 ,  [info_fault_regs_msk]               "i" (1U << 23)
 ,  [info_secure_fault_regs_msk]        "i" (1U << 24)
 ,  [info_ras_fault_reg_msk]            "i" (1U << 25)
#if (ARM_FAULT_FAULT_REGS_EXIST != 0)
 ,  [csfr_reg_addr]                     "i" (&ARM_FaultInfo.CFSR)
 ,  [scb_base_addr]                     "i" (SCB_BASE)
 ,  [cfsr_ofs]                          "i" (offsetof(SCB_Type, CFSR ))
 ,  [hfsr_ofs]                          "i" (offsetof(SCB_Type, HFSR ))
 ,  [dfsr_ofs]                          "i" (offsetof(SCB_Type, DFSR ))
 ,  [mmfar_ofs]                         "i" (offsetof(SCB_Type, MMFAR))
 ,  [bfar_ofs]                          "i" (offsetof(SCB_Type, BFAR ))
 ,  [afsr_ofs]                          "i" (offsetof(SCB_Type, AFSR ))
#if (ARM_FAULT_TZ_SECURE != 0)
 ,  [scb_ns_base_addr]                  "i" (SCB_BASE_NS)
#if (ARM_FAULT_ARCH_ARMV8x_M_MAIN != 0)
 ,  [sfsr_reg_addr]                     "i" (&ARM_FaultInfo.SFSR)
 ,  [sfsr_ofs]                          "i" (offsetof(SCB_Type, SFSR ))
 ,  [sfar_ofs]                          "i" (offsetof(SCB_Type, SFAR ))
#if (ARM_FAULT_ARCH_ARMV8_1M_MAIN != 0)
 ,  [rfsr_reg_addr]                     "i" (&ARM_FaultInfo.RFSR)
 ,  [rfsr_ofs]                          "i" (offsetof(SCB_Type, RFSR ))
#endif
#endif
#endif
#endif
 :  /* clobber list */
    "r0", "r1", "r2", "r3", "cc", "memory");
#endif

  __ASM volatile (
#ifndef __ICCARM__
    ".syntax unified\n"
#endif

 /* --- Calculate and store CRC-32 value into ARM_FaultInfo.crc32 --- */
 /* Calculate CRC-32 on ARM_FaultInfo structure (excluding magic_number and crc32 fields) and
    store it into ARM_FaultInfo.crc32 */
    "ldr   r0,  =%c[crc_init_val]\n"    // R0 = init_val parameter
    "ldr   r1,  =%c[crc_data_ptr]\n"    // R1 = data_ptr parameter
    "ldr   r2,  =%c[crc_data_len]\n"    // R2 = data_len parameter
    "ldr   r3,  =%c[crc_polynom]\n"     // R3 = polynom  parameter

 /* Calculate CRC-32 with result provided in R0 register */
    "b     crc_check\n"
  "crc_wloop:\n"
    "ldrb  r5,  [r1,#0]\n"
    "lsls  r5,  r5, #24\n"
    "eors  r0,  r0, r5\n"
    "movs  r4,  #8\n"
  "crc_floop:\n"
    "lsls  r0,  r0, #1\n"
    "bcc   crc_next\n"
    "eors  r0,  r0, r3\n"
  "crc_next:\n"
    "subs  r4,  r4, #1\n"
    "bne   crc_floop\n"
    "adds  r1,  r1, #1\n"
    "subs  r2,  r2, #1\n"
  "crc_check:\n"
    "cmp   r2,  #0\n"
    "bne   crc_wloop\n"

    "ldr   r2,  =%c[crc32_addr]\n"      // Load crc32 address
    "str   r0,  [r2]\n"                 // Store crc32 value

 /* --- Store ARM_FaultInfo.magic_number --- */
    "ldr   r2,  =%c[magic_number_addr]\n"  // Load  magic_number address
    "ldr   r0,  =%c[magic_number_val]\n"   // Load  magic_number value
    "str   r0,  [r2]\n"                    // Store magic_number value

    "dsb\n"                             // Ensure content of ARM_FaultInfo is up-to-date

 /* Restore registers R4 .. R7 */
    "ldr   r0,  =%c[r4_addr]\n"
    "ldm   r0!, {r4-r7}\n"

 /* If additional state information is available clear the R4 .. R7 */
#if (ARM_FAULT_ARCH_ARMV8x_M != 0)      // If arch is Armv8/8.1-M
    "ldr   r0,  =%c[IntegritySignature_addr]\n" // Load IntegritySignature address
    "ldr   r1,  =%c[IntegritySignature_val]\n"  // Load IntegritySignature expected valid value
    "ldr   r2,  [r0]\n"                 // Load IntegritySignature value from ARM_FaultInfo
    "cmp   r2,  r1\n"                   // Compare IntegritySignature to expected valid value
    "bne   restored_r4_to_r7\n"         // If they are different, do not clear R4 .. R7 and exit section
    "movs  r4,  #0\n"                   // otherwise clear R4 .. R7
    "movs  r5,  #0\n"
    "movs  r6,  #0\n"
    "movs  r7,  #0\n"
#endif
  "restored_r4_to_r7:\n"

 /* Jump to ARM_FaultExit function */
    "ldr   r0,  =ARM_FaultExit\n"
    "mov   pc,  r0\n"

 /* Inline assembly template operands */
 :  /* no outputs */
 :  /* inputs */
    [magic_number_addr]                 "i" (&ARM_FaultInfo.magic_number)
 ,  [magic_number_val]                  "i" (ARM_FAULT_MAGIC_NUMBER)
 ,  [crc32_addr]                        "i" (&ARM_FaultInfo.crc32)
 ,  [r4_addr]                           "i" (&ARM_FaultInfo.R4)
 ,  [IntegritySignature_addr]           "i" (&ARM_FaultInfo.IntegritySignature)
#if (ARM_FAULT_ARCH_ARMV8x_M != 0)
 ,  [IntegritySignature_val]            "i" (ARM_FAULT_ASC_INTEGRITY_SIG)
#endif
 ,  [crc_init_val]                      "i" (ARM_FAULT_CRC32_INIT_VAL)
 ,  [crc_data_ptr]                      "i" (&ARM_FaultInfo.count)
 ,  [crc_data_len]                      "i" (sizeof(ARM_FaultInfo) - (sizeof(ARM_FaultInfo.magic_number) + sizeof(ARM_FaultInfo.crc32)))
 ,  [crc_polynom]                       "i" (ARM_FAULT_CRC32_POLYNOM)
 :  /* clobber list */
    "r0", "r1", "r2", "r3", "cc", "memory");
}

/**
  Callback function called after fault information was saved.
  Used to provide a specific reaction to fault after it was saved.
  The default implementation will RESET the system.
  User can override this function to provide desired reaction.
  It is preferred that user implemented function would not use stack
  since it that could cause another fault.
*/
__WEAK __NAKED void ARM_FaultExit (void) {
  //lint -efunc(10,  ARM_FaultExit) "Suppress: expecting ';'"
  //lint -efunc(522, ARM_FaultExit) "Suppress: Warning 522: Highest operation, a 'constant', lacks side-effects [MISRA 2012 Rule 2.2, required]"
  __ASM volatile (
#ifndef __ICCARM__
    ".syntax unified\n"
#endif

    "dsb\n"
    "ldr   r0, =%c[aircr_addr]\n"
    "ldr   r1, =%c[aircr_val]\n"
#if (ARM_FAULT_FAULT_REGS_EXIST != 0)
    "ldr   r2, =%c[aircr_msk]\n"
    "ldr   r3, [r0]\n"
    "ands  r3, r2\n"
    "orrs  r1, r3\n"
#endif
    "str   r1, [r0]\n"
    "dsb\n"
    "b     .\n"

 :  /* no outputs */
 :  /* inputs */
    [aircr_addr] "i" (SCB_BASE + offsetof(SCB_Type, AIRCR))
 ,  [aircr_val]  "i" ((0x5FAUL << SCB_AIRCR_VECTKEY_Pos) | SCB_AIRCR_SYSRESETREQ_Msk)
#if (ARM_FAULT_FAULT_REGS_EXIST != 0)
 ,  [aircr_msk]  "i" (SCB_AIRCR_PRIGROUP_Msk)
#endif
 :  /* clobber list */
    "r0", "r1", "r2", "r3", "cc", "memory");
}


// Helper function

/**
  Calculate CRC-32 on data block in memory
  \param[in]    init_val        initial CRC value
  \param[in]    data_ptr        pointer to data
  \param[in]    data_len        data length (in bytes)
  \param[in]    polynom         CRC polynom
  \return       CRC-32 value (32-bit)
*/
static uint32_t CalcCRC32 (      uint32_t init_val,
                           const uint8_t *data_ptr,
                                 uint32_t data_len,
                                 uint32_t polynom) {
  uint32_t crc32, i;

  crc32 = init_val;
  while (data_len != 0U) {
    crc32 ^= ((uint32_t)*data_ptr) << 24;
    for (i = 8U; i != 0U; i--) {
      if ((crc32 & 0x80000000U) != 0U) {
        crc32 <<= 1;
        crc32  ^= polynom;
      } else {
        crc32 <<= 1;
      }
    }
    data_ptr++;
    data_len--;
  }

  return crc32;
}
