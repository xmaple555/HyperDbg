#include "Vmx.h"
#include "Common.h"
#include "Ept.h"
#include "InlineAsm.h"
#include "GlobalVariables.h"
#include "Vmcall.h"
#include "Hooks.h"
#include "Invept.h"
#include "HypervisorRoutines.h"
#include "Events.h"

/* Main Vmexit events handler */
BOOLEAN
VmxVmexitHandler(PGUEST_REGS GuestRegs)
{
    VMEXIT_INTERRUPT_INFO InterruptExit         = {0};
    UINT64                GuestPhysicalAddr     = 0;
    ULONG                 ExitReason            = 0;
    ULONG                 ExitQualification     = 0;
    ULONG                 Rflags                = 0;
    ULONG                 EcxReg                = 0;
    ULONG                 ExitInstructionLength = 0;
    int                   CurrentProcessorIndex = 0;

    /*********** SEND MESSAGE AFTER WE SET THE STATE ***********/

    CurrentProcessorIndex = KeGetCurrentProcessorNumber();

    // Indicates we are in Vmx root mode in this logical core
    g_GuestState[CurrentProcessorIndex].IsOnVmxRootMode = TRUE;
    g_GuestState[CurrentProcessorIndex].IncrementRip    = TRUE;

    __vmx_vmread(VM_EXIT_REASON, &ExitReason);
    ExitReason &= 0xffff;

    __vmx_vmread(EXIT_QUALIFICATION, &ExitQualification);

    // Debugging purpose
    //LogInfo("VM_EXIT_REASON : 0x%x", ExitReason);
    //LogInfo("EXIT_QUALIFICATION : 0x%llx", ExitQualification);

    switch (ExitReason)
    {
    case EXIT_REASON_TRIPLE_FAULT:
    {
        LogError("Triple fault error occured.");

        break;
    }

        // 25.1.2  Instructions That Cause VM Exits Unconditionally
        // The following instructions cause VM exits when they are executed in VMX non-root operation: CPUID, GETSEC,
        // INVD, and XSETBV. This is also true of instructions introduced with VMX, which include: INVEPT, INVVPID,
        // VMCALL, VMCLEAR, VMLAUNCH, VMPTRLD, VMPTRST, VMRESUME, VMXOFF, and VMXON.

    case EXIT_REASON_VMCLEAR:
    case EXIT_REASON_VMPTRLD:
    case EXIT_REASON_VMPTRST:
    case EXIT_REASON_VMREAD:
    case EXIT_REASON_VMRESUME:
    case EXIT_REASON_VMWRITE:
    case EXIT_REASON_VMXOFF:
    case EXIT_REASON_VMXON:
    case EXIT_REASON_VMLAUNCH:
    {
        __vmx_vmread(GUEST_RFLAGS, &Rflags);
        __vmx_vmwrite(GUEST_RFLAGS, Rflags | 0x1); // cf=1 indicate vm instructions fail

        break;
    }

    case EXIT_REASON_CR_ACCESS:
    {
        HvHandleControlRegisterAccess(GuestRegs);
        break;
    }
    case EXIT_REASON_MSR_READ:
    {
        EcxReg = GuestRegs->rcx & 0xffffffff;
        HvHandleMsrRead(GuestRegs);

        break;
    }
    case EXIT_REASON_MSR_WRITE:
    {
        EcxReg = GuestRegs->rcx & 0xffffffff;
        HvHandleMsrWrite(GuestRegs);

        break;
    }
    case EXIT_REASON_CPUID:
    {
        HvHandleCpuid(GuestRegs);
        break;
    }

    case EXIT_REASON_IO_INSTRUCTION:
    {
        LogError("Exit reason for I/O instructions are not supported yet.");
        break;
    }
    case EXIT_REASON_EPT_VIOLATION:
    {
        // Reading guest physical address
        __vmx_vmread(GUEST_PHYSICAL_ADDRESS, &GuestPhysicalAddr);

        if (!EptHandleEptViolation(ExitQualification, GuestPhysicalAddr))
            LogError("There were errors in handling Ept Violation");

        break;
    }
    case EXIT_REASON_EPT_MISCONFIG:
    {
        __vmx_vmread(GUEST_PHYSICAL_ADDRESS, &GuestPhysicalAddr);

        EptHandleMisconfiguration(GuestPhysicalAddr);

        break;
    }
    case EXIT_REASON_VMCALL:
    {
        // Check if it's our routines that request the VMCALL our it relates to Hyper-V
        if (GuestRegs->r10 == 0x48564653 && GuestRegs->r11 == 0x564d43414c4c && GuestRegs->r12 == 0x4e4f485950455256)
        {
            // Then we have to manage it as it relates to us
            GuestRegs->rax = VmxVmcallHandler(GuestRegs->rcx, GuestRegs->rdx, GuestRegs->r8, GuestRegs->r9);
        }
        else
        {
            // Otherwise let the top-level hypervisor to manage it
            GuestRegs->rax = AsmHypervVmcall(GuestRegs->rcx, GuestRegs->rdx, GuestRegs->r8);
        }
        break;
    }
    case EXIT_REASON_EXCEPTION_NMI:
    {
        /*

		Exception or non-maskable interrupt (NMI). Either:
			1: Guest software caused an exception and the bit in the exception bitmap associated with exception�s vector was set to 1
			2: An NMI was delivered to the logical processor and the �NMI exiting� VM-execution control was 1.

		VM_EXIT_INTR_INFO shows the exit infromation about event that occured and causes this exit
		Don't forget to read VM_EXIT_INTR_ERROR_CODE in the case of re-injectiong event

		*/

        // read the exit reason
        __vmx_vmread(VM_EXIT_INTR_INFO, &InterruptExit);

        if (InterruptExit.InterruptionType == INTERRUPT_TYPE_SOFTWARE_EXCEPTION && InterruptExit.Vector == EXCEPTION_VECTOR_BREAKPOINT)
        {
            ULONG64 GuestRip;
            // Reading guest's RIP
            __vmx_vmread(GUEST_RIP, &GuestRip);

            // Send the user
            LogInfo("Breakpoint Hit (Process Id : 0x%x) at : %llx ", PsGetCurrentProcessId(), GuestRip);

            g_GuestState[CurrentProcessorIndex].IncrementRip = FALSE;

            // re-inject #BP back to the guest
            EventInjectBreakpoint();
        }
        else if (InterruptExit.InterruptionType == INTERRUPT_TYPE_HARDWARE_EXCEPTION && InterruptExit.Vector == EXCEPTION_VECTOR_UNDEFINED_OPCODE)
        {
            // Handle the #UD, checking if this exception was intentional.

            if (!SyscallHookHandleUD(GuestRegs, CurrentProcessorIndex))
            {
                // If this #UD was found to be unintentional, inject a #UD interruption into the guest.
                EventInjectUndefinedOpcode();
            }
        }
        else
        {
            LogError("Not expected event occured");
        }
        break;
    }
    case EXIT_REASON_MONITOR_TRAP_FLAG:
    {
        /* Monitor Trap Flag */
        if (g_GuestState[CurrentProcessorIndex].MtfEptHookRestorePoint)
        {
            // Restore the previous state
            EptHandleMonitorTrapFlag(g_GuestState[CurrentProcessorIndex].MtfEptHookRestorePoint);
            // Set it to NULL
            g_GuestState[CurrentProcessorIndex].MtfEptHookRestorePoint = NULL;
        }
        else if (g_GuestState[CurrentProcessorIndex].DebuggingState.UndefinedInstructionAddress != NULL)
        {
            ULONG64 GuestRip;

            // Reading guest's RIP
            __vmx_vmread(GUEST_RIP, &GuestRip);

            if (g_GuestState[CurrentProcessorIndex].DebuggingState.UndefinedInstructionAddress == GuestRip)
            {
                // #UD was not because of syscall because it's no incremented, we should inject the #UD again
                EventInjectUndefinedOpcode();
            }
            else
            {
                // It was because of Syscall, let's log it
                LogInfo("SYSCALL instruction => 0x%llX , process id : 0x%x , rax = 0x%llx",
                        g_GuestState[CurrentProcessorIndex].DebuggingState.UndefinedInstructionAddress,
                        PsGetCurrentProcessId(),
                        GuestRegs->rax);
            }

            // Enable syscall hook again
            SyscallHookDisableSCE();
            g_GuestState[CurrentProcessorIndex].DebuggingState.UndefinedInstructionAddress = NULL;
        }
        else
        {
            LogError("Why MTF occured ?!");
        }

        // Redo the instruction
        g_GuestState[CurrentProcessorIndex].IncrementRip = FALSE;

        // We don't need MTF anymore
        HvSetMonitorTrapFlag(FALSE);

        break;
    }
    case EXIT_REASON_HLT:
    {
        //__halt();
        break;
    }
    default:
    {
        LogError("Unkown Vmexit, reason : 0x%llx", ExitReason);
        break;
    }
    }

    if (!g_GuestState[CurrentProcessorIndex].VmxoffState.IsVmxoffExecuted && g_GuestState[CurrentProcessorIndex].IncrementRip)
    {
        HvResumeToNextInstruction();
    }

    // Set indicator of Vmx non root mode to false
    g_GuestState[CurrentProcessorIndex].IsOnVmxRootMode = FALSE;

    if (g_GuestState[CurrentProcessorIndex].VmxoffState.IsVmxoffExecuted)
        return TRUE;

    return FALSE;
}