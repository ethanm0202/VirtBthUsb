/*
 * arming.h - Fail-closed arming gate evaluation.
 *
 * Windows permits exactly one active Bluetooth adapter. To prevent DeckBtUsb
 * from bringing up a secondary virtual radio unattended at boot (which breaks
 * system-wide Bluetooth Radio Management if the physical radio is active), the
 * driver operates under a strict fail-closed arming gate.
 *
 * Only an exact REG_DWORD value of 1 at
 * HKLM\SYSTEM\CurrentControlSet\Services\DeckBtUsb\Parameters\Enabled
 * arms the controller. Absent key, absent value, wrong type, wrong size, or
 * any read failure must evaluate to disarmed (0).
 *
 * This header compiles in kernel mode (the driver) and in user mode (the
 * host-side self-test suites) with no Windows or WDF headers required.
 */

#pragma once

/* Win32 / NT registry type for a 32-bit unsigned integer (REG_DWORD == 4). */
#ifndef DECKBT_REG_DWORD
#define DECKBT_REG_DWORD 4u
#endif

/*
 * Evaluates the arming gate.
 * Returns 1 (armed) iff:
 *   - ReadSucceeded is non-zero
 *   - Type is REG_DWORD (4)
 *   - Size is 4 bytes (sizeof(unsigned long))
 *   - Value is exactly 1
 * Returns 0 (disarmed) in all other cases.
 */
static __inline unsigned char
DeckBtArmingGateArmed(
    unsigned char ReadSucceeded,
    unsigned long Type,
    unsigned long Size,
    unsigned long Value)
{
    if (!ReadSucceeded) {
        return 0;
    }
    if (Type != DECKBT_REG_DWORD) {
        return 0;
    }
    if (Size != sizeof(unsigned long)) {
        return 0;
    }
    if (Value != 1u) {
        return 0;
    }
    return 1;
}
