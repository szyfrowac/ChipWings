//
// Chipwings: RISC-V 64 CPU for Renode that cannot lose interrupts raised from other threads.
//
// Renode 1.17.0 BaseRiscV.OnGPIO calls TlibSetMipBit on whatever thread raised the interrupt.
// With parallel execution that is often another hart's thread: hart 3 writing hart 2's CLINT
// msip (IPI), or hart 1 raising a PLIC line through the Mi-V IHC. If the target hart updates its
// own mip at the same moment (OpenSBI writes mip.STIP for the Linux timer), one read-modify-write
// of mip overwrites the other and the bit is lost. GPIO.Set skips repeated values, so the line
// never re-asserts: msip reads 1, mip.MSIP stays 0, and an OpenSBI remote TLB flush waits for
// that hart forever (Linux: "rcu_sched detected stalls").
//
// Fix: an interrupt change requested from this CPU's own thread is applied immediately, as
// before. One requested from any other thread is recorded and applied at the nearest synced
// state, when no CPU is executing, so nothing else can be writing this hart's mip. Only the
// latest requested level of each line is applied.
//
using System.Collections.Generic;
using Antmicro.Renode.Core;
using Antmicro.Renode.Peripherals.Timers;
using Antmicro.Renode.Utilities;

using Endianess = ELFSharp.ELF.Endianess;

namespace Antmicro.Renode.Peripherals.CPU
{
    public class SafeIrqRiscV64 : RiscV64
    {
        public SafeIrqRiscV64(
            IMachine machine,
            string cpuType,
            IRiscVTimeProvider timeProvider = null,
            uint hartId = 0,
            [NameAlias("privilegeArchitecture")] PrivilegedArchitecture privilegedArchitecture = PrivilegedArchitecture.Priv1_11,
            Endianess endianness = Endianess.LittleEndian,
            ulong? nmiVectorAddress = null,
            uint? nmiVectorLength = null,
            bool allowUnalignedAccesses = false,
            uint pmpNumberOfAddrBits = 54,
            InterruptMode interruptMode = InterruptMode.Auto,
            PrivilegeLevels privilegeLevels = PrivilegeLevels.MachineSupervisorUser,
            uint pmpEntryCount = 64
        )
            : base(machine, cpuType, timeProvider, hartId, privilegedArchitecture, endianness, nmiVectorAddress, nmiVectorLength,
                   allowUnalignedAccesses, pmpNumberOfAddrBits, interruptMode, privilegeLevels, pmpEntryCount)
        {
            ownerMachine = machine;
        }

        public override void OnGPIO(int number, bool value)
        {
            bool schedule;
            lock(pendingLock)
            {
                pending[number] = value;
                schedule = !applyScheduled;
                applyScheduled = true;
            }

            if(ownerMachine.SystemBus.TryGetCurrentCPU(out var current) && current == this)
            {
                ApplyPending();
            }
            else if(schedule)
            {
                ownerMachine.LocalTimeSource.ExecuteInNearestSyncedState(_ => ApplyPending());
            }
        }

        private void ApplyPending()
        {
            List<KeyValuePair<int, bool>> changes;
            lock(pendingLock)
            {
                changes = new List<KeyValuePair<int, bool>>(pending);
                pending.Clear();
                applyScheduled = false;
            }
            foreach(var change in changes)
            {
                base.OnGPIO(change.Key, change.Value);
            }
        }

        private readonly IMachine ownerMachine;
        private readonly Dictionary<int, bool> pending = new Dictionary<int, bool>();
        private readonly object pendingLock = new object();
        private bool applyScheduled;
    }
}
