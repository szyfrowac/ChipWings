//
// Chipwings: RISC-V CLINT for Renode whose msip IPI line cannot get out of step with the register.
//
// Based on Renode 1.17.0 src/Emulator/Cores/RiscV/CoreLevelInterruptor.cs
// (renode-infrastructure 066a7f13, MIT licence, Copyright (c) Antmicro).
//
// Why: with parallel execution each hart runs on its own thread. In the stock model a write to
// msip stores the bit and then sets the IRQ line, with no lock around the pair. When hart 3 sets
// hart 2's msip while hart 2 clears its own (OpenSBI's IPI handler), the two writes can interleave
// as store 0, store 1, Set(true), Set(false): msip reads 1 but the MSIP line stays low, hart 2
// never takes the IPI and an OpenSBI remote TLB flush on hart 3 waits for it forever (Linux:
// "rcu_sched detected stalls").
//
// Fix: after storing msip, set the line to the value msip holds *now* and repeat until it no
// longer changes, so whichever write lands last also leaves the line matching it. No extra lock:
// a lock here deadlocked against the timer framework (CompareReached runs under the clock
// source's lock while an mtimecmp write calls into the timer). mtimecmp keeps the stock
// behaviour; only the owning hart writes it.
//
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.Threading;
using Antmicro.Renode.Core;
using Antmicro.Renode.Exceptions;
using Antmicro.Renode.Logging;
using Antmicro.Renode.Peripherals.Bus;
using Antmicro.Renode.Peripherals.Timers;

namespace Antmicro.Renode.Peripherals.IRQControllers
{
    [AllowedTranslations(AllowedTranslation.QuadWordToDoubleWord)]
    public class SafeIpiCoreLevelInterruptor : IDoubleWordPeripheral, IKnownSize, INumberedGPIOOutput, IRiscVTimeProvider
    {
        public SafeIpiCoreLevelInterruptor(IMachine machine, ulong frequency, int numberOfTargets = 1, ulong divider = 1)
        {
            this.machine = machine;
            if(numberOfTargets < 1)
            {
                throw new ConstructionException($"Invalid numberOfTargets: provided {numberOfTargets} but should be greater or equal to 1.");
            }
            this.numberOfTargets = numberOfTargets;
            msip = new int[numberOfTargets];
            for(var i = 0; i < numberOfTargets; i++)
            {
                var hartId = i;
                irqs[2 * hartId] = new GPIO();
                irqs[2 * hartId + 1] = new GPIO();
                var timer = new ComparingTimer(machine.ClockSource, frequency, this, hartId.ToString(), enabled: true, eventEnabled: true, divider: divider);
                timer.CompareReached += () => irqs[2 * hartId + 1].Set(true);
                mTimers.Add(timer);
            }
            Connections = new ReadOnlyDictionary<int, IGPIO>(irqs);
        }

        public void Reset()
        {
            for(var i = 0; i < numberOfTargets; i++)
            {
                Volatile.Write(ref msip[i], 0);
            }
            foreach(var irq in irqs.Values)
            {
                irq.Set(false);
            }
            foreach(var timer in mTimers)
            {
                timer.Reset();
            }
        }

        public uint ReadDoubleWord(long offset)
        {
            if(offset == MTimeLo)
            {
                return (uint)TimerValue;
            }
            if(offset == MTimeHi)
            {
                return (uint)(TimerValue >> 32);
            }
            if(TryGetMsipHart(offset, out var hart))
            {
                return (uint)Volatile.Read(ref msip[hart]);
            }
            if(TryGetMTimeCmpHart(offset, out hart, out var high))
            {
                var compare = mTimers[hart].Compare;
                return high ? (uint)(compare >> 32) : (uint)compare;
            }
            this.Log(LogLevel.Warning, "Unhandled read from offset 0x{0:X}", offset);
            return 0;
        }

        public void WriteDoubleWord(long offset, uint value)
        {
            if(offset == MTimeLo || offset == MTimeHi)
            {
                var timerValue = TimerValue;
                timerValue = offset == MTimeLo
                    ? (timerValue & ~0xffffffffUL) | value
                    : (timerValue & 0xffffffffUL) | ((ulong)value << 32);
                foreach(var timer in mTimers)
                {
                    timer.Value = timerValue;
                }
                return;
            }
            if(TryGetMsipHart(offset, out var hart))
            {
                Volatile.Write(ref msip[hart], (int)(value & 1));
                int current;
                do
                {
                    current = Volatile.Read(ref msip[hart]);
                    irqs[2 * hart].Set(current != 0);
                }
                while(Volatile.Read(ref msip[hart]) != current);
                return;
            }
            if(TryGetMTimeCmpHart(offset, out hart, out var high))
            {
                var limit = mTimers[hart].Compare;
                limit = high
                    ? (limit & 0xffffffffUL) | ((ulong)value << 32)
                    : (limit & ~0xffffffffUL) | value;
                irqs[2 * hart + 1].Set(false);
                mTimers[hart].Compare = limit;
                if(TimerValue >= limit)
                {
                    irqs[2 * hart + 1].Set(true);
                }
                return;
            }
            this.Log(LogLevel.Warning, "Unhandled write of 0x{0:X} to offset 0x{1:X}", value, offset);
        }

        public ulong TimerValue
        {
            get
            {
                if(machine.SystemBus.TryGetCurrentCPU(out var cpu))
                {
                    cpu.SyncTime();
                }
                return mTimers[0].Value;
            }
        }

        public long Size => 0x10000;

        public IReadOnlyDictionary<int, IGPIO> Connections { get; }

        private bool TryGetMsipHart(long offset, out int hart)
        {
            hart = (int)(offset / 4);
            return offset >= MSipHart0 && offset % 4 == 0 && hart < numberOfTargets;
        }

        private bool TryGetMTimeCmpHart(long offset, out int hart, out bool high)
        {
            var rel = offset - MTimeCmpHart0Lo;
            hart = (int)(rel / 8);
            high = rel % 8 == 4;
            return rel >= 0 && rel % 4 == 0 && hart < numberOfTargets;
        }

        private readonly int[] msip;
        private readonly int numberOfTargets;
        private readonly Dictionary<int, IGPIO> irqs = new Dictionary<int, IGPIO>();
        private readonly List<ComparingTimer> mTimers = new List<ComparingTimer>();
        private readonly IMachine machine;

        private const long MSipHart0 = 0x0;
        private const long MTimeCmpHart0Lo = 0x4000;
        private const long MTimeLo = 0xBFF8;
        private const long MTimeHi = 0xBFFC;
    }
}
