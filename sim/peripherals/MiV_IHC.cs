//
// Chipwings: Renode model of the Microchip Mi-V Inter-Hart Communication (IHC) v1 fabric IP
// for the PolarFire SoC, as used by NuttX's mpfs_ihc.c (direct, hart 1) and by the
// Linux miv-ihc mailbox driver through the OpenSBI vendor extension (harts 2-4).
//
// Layout (base 0x50000000, the Icicle Kit reference design), for local hart l = 0..4:
//   base + 0x500*l + 0x100*slot   channel l -> r, slot = r < l ? r : r - 1   (IHCC)
//   base + 0x500*l + 0x400        aggregator for hart l                     (IHCIA)
//
// IHCC registers:  0x00 version, 0x04 control, 0x08 local hart id, 0x0C message size (words),
//                  0x20 message in [4] (= the remote's message out), 0x30 message out [4]
// IHCIA registers: 0x00 version, 0x04 INT_EN, 0x08 MSG_AVAIL
//                  bit 2r = message present from hart r, bit 2r+1 = ack from hart r
//
// Control register, as Microchip's HSS miv_ihc.c and NuttX use it:
//   bit 0 RMP     set by the sender: "I have put a message in my message out"
//   bit 1 MP      reads the remote's RMP; writing 0 consumes the message (clears the remote's RMP)
//   bit 2 MPIE    message-present interrupt enable
//   bit 3 ACK     the ack this side has sent to the remote, held until the remote clears it
//   bit 4 ACK_CLR writing 0 clears the ack the remote sent us; reads as 1, so read-modify-write
//                 sequences that do not mean to clear it leave it alone
//   bit 5 ACKIE   ack interrupt enable
// The aggregator output for hart l is MSG_AVAIL(l) & INT_EN(l), level triggered, to the PLIC.
//
using System;
using Antmicro.Renode.Core;
using Antmicro.Renode.Logging;
using Antmicro.Renode.Peripherals.Bus;

namespace Antmicro.Renode.Peripherals.Miscellaneous
{
    public class MiV_IHC : IDoubleWordPeripheral, IKnownSize
    {
        public MiV_IHC(uint messageSize = 2, uint version = 5)
        {
            if(messageSize == 0 || messageSize > MaxMessageWords)
            {
                throw new ArgumentException($"messageSize must be 1..{MaxMessageWords}");
            }
            this.messageSize = messageSize;
            this.version = version;
            irqs = new GPIO[Harts];
            for(var i = 0; i < Harts; i++)
            {
                irqs[i] = new GPIO();
            }
            Reset();
        }

        public GPIO Hart0IRQ => irqs[0];
        public GPIO Hart1IRQ => irqs[1];
        public GPIO Hart2IRQ => irqs[2];
        public GPIO Hart3IRQ => irqs[3];
        public GPIO Hart4IRQ => irqs[4];

        public long Size => HartStride * Harts;

        public void Reset()
        {
            lock(sync)
            {
                rmp = new bool[Harts, Harts];
                mpie = new bool[Harts, Harts];
                ackOut = new bool[Harts, Harts];
                ackie = new bool[Harts, Harts];
                messageOut = new uint[Harts, Harts, MaxMessageWords];
                localHartId = new uint[Harts, Harts];
                intEn = new uint[Harts];
                for(var l = 0; l < Harts; l++)
                {
                    for(var r = 0; r < Harts; r++)
                    {
                        localHartId[l, r] = (uint)l;
                    }
                }
                UpdateInterrupts();
            }
        }

        public uint ReadDoubleWord(long offset)
        {
            lock(sync)
            {
                if(!Decode(offset, out var l, out var r, out var reg))
                {
                    this.Log(LogLevel.Warning, "Read from unmapped offset 0x{0:X}", offset);
                    return 0;
                }
                if(r < 0)
                {
                    switch(reg)
                    {
                    case 0x0: return version;
                    case 0x4: return intEn[l];
                    case 0x8: return MessageAvailable(l);
                    default: return 0;
                    }
                }
                switch(reg)
                {
                case 0x00: return version;
                case 0x04: return Control(l, r);
                case 0x08: return localHartId[l, r];
                case 0x0C: return messageSize;
                }
                if(reg >= MessageInOffset && reg < MessageInOffset + 4 * MaxMessageWords)
                {
                    return messageOut[r, l, (reg - MessageInOffset) / 4];
                }
                if(reg >= MessageOutOffset && reg < MessageOutOffset + 4 * MaxMessageWords)
                {
                    return messageOut[l, r, (reg - MessageOutOffset) / 4];
                }
                return 0;
            }
        }

        public void WriteDoubleWord(long offset, uint value)
        {
            lock(sync)
            {
                if(!Decode(offset, out var l, out var r, out var reg))
                {
                    this.Log(LogLevel.Warning, "Write 0x{0:X} to unmapped offset 0x{1:X}", value, offset);
                    return;
                }
                if(r < 0)
                {
                    if(reg == 0x4)
                    {
                        intEn[l] = value & 0x3FF;
                        UpdateInterrupts();
                    }
                    return;
                }
                if(reg == 0x04)
                {
                    WriteControl(l, r, value);
                }
                else if(reg == 0x08)
                {
                    localHartId[l, r] = value;
                }
                else if(reg >= MessageOutOffset && reg < MessageOutOffset + 4 * MaxMessageWords)
                {
                    messageOut[l, r, (reg - MessageOutOffset) / 4] = value;
                }
            }
        }

        private bool Decode(long offset, out int local, out int remote, out long reg)
        {
            local = (int)(offset / HartStride);
            var slot = (int)(offset % HartStride / ChannelStride);
            reg = offset % ChannelStride;
            remote = -1;
            if(local >= Harts)
            {
                return false;
            }
            if(slot < Harts - 1)
            {
                remote = slot < local ? slot : slot + 1;
            }
            return true;
        }

        private uint Control(int l, int r)
        {
            return (rmp[l, r] ? Rmp : 0u)
                | (rmp[r, l] ? Mp : 0u)
                | (mpie[l, r] ? MpIe : 0u)
                | (ackOut[l, r] ? Ack : 0u)
                | AckClr
                | (ackie[l, r] ? AckIe : 0u);
        }

        private void WriteControl(int l, int r, uint value)
        {
            var sending = (value & Rmp) != 0 && !rmp[l, r];
            rmp[l, r] = (value & Rmp) != 0;
            if((value & Mp) == 0 && rmp[r, l])
            {
                rmp[r, l] = false;
                this.Log(LogLevel.Debug, "hart {0} consumed message from hart {1}", l, r);
            }
            mpie[l, r] = (value & MpIe) != 0;
            if((value & Ack) != 0 && !ackOut[l, r])
            {
                this.Log(LogLevel.Debug, "hart {0} acks hart {1}", l, r);
            }
            ackOut[l, r] = (value & Ack) != 0;
            if((value & AckClr) == 0)
            {
                ackOut[r, l] = false;
            }
            ackie[l, r] = (value & AckIe) != 0;
            if(sending)
            {
                this.Log(LogLevel.Debug, "hart {0} -> hart {1}: 0x{2:X8} 0x{3:X8}", l, r, messageOut[l, r, 0], messageOut[l, r, 1]);
            }
            UpdateInterrupts();
        }

        private uint MessageAvailable(int l)
        {
            var avail = 0u;
            for(var r = 0; r < Harts; r++)
            {
                if(r == l)
                {
                    continue;
                }
                if(rmp[r, l] && mpie[l, r])
                {
                    avail |= 1u << (2 * r);
                }
                if(ackOut[r, l] && ackie[l, r])
                {
                    avail |= 1u << (2 * r + 1);
                }
            }
            return avail;
        }

        private void UpdateInterrupts()
        {
            for(var l = 0; l < Harts; l++)
            {
                irqs[l].Set((MessageAvailable(l) & intEn[l]) != 0);
            }
        }

        private bool[,] rmp;
        private bool[,] mpie;
        private bool[,] ackOut;
        private bool[,] ackie;
        private uint[,,] messageOut;
        private uint[,] localHartId;
        private uint[] intEn;

        private readonly GPIO[] irqs;
        private readonly uint messageSize;
        private readonly uint version;
        private readonly object sync = new object();

        private const int Harts = 5;
        private const int MaxMessageWords = 4;
        private const long HartStride = 0x500;
        private const long ChannelStride = 0x100;
        private const long MessageInOffset = 0x20;
        private const long MessageOutOffset = 0x30;

        private const uint Rmp = 1u << 0;
        private const uint Mp = 1u << 1;
        private const uint MpIe = 1u << 2;
        private const uint Ack = 1u << 3;
        private const uint AckClr = 1u << 4;
        private const uint AckIe = 1u << 5;
    }
}
