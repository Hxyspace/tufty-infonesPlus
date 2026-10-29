/* Mapper 19: Namco 163 digital registers. No expansion-audio output.
 * CIRAM is shared by pattern and name tables; no private 8 KiB CHR RAM. */
static BYTE Map19_Chr[8];
BYTE *Map19_Internal;
static BYTE Map19_ChrControl, Map19_WriteProtect, Map19_Address;
static BYTE Map19_AudioDivider, Map19_AudioChannel;
static bool Map19_AudioDisabled;
static unsigned Map19_RamBytes;
static WORD Map19_Irq;
static uint32_t Map19_LastCycle;

/* Advance the CPU-readable phase registers without generating samples.
 * Only this mapper pays for it, at register access / scanline boundaries. */
static void Map19_Phase(unsigned channel)
{
  BYTE *r = Map19_Internal + 0x40 + channel * 8;
  const uint32_t frequency = r[0] | (uint32_t(r[2]) << 8) | (uint32_t(r[4] & 3) << 16);
  const uint32_t phase = r[1] | (uint32_t(r[3]) << 8) | (uint32_t(r[5]) << 16);
  const uint32_t length = (256u - (r[4] & 0xfc)) << 16;
  uint32_t next = phase + frequency;
  if (next >= length) next %= length;
  r[1] = next; r[3] = next >> 8; r[5] = next >> 16;
}

static void Map19_Sync()
{
  const uint32_t now = K6502_Now();
  const uint32_t elapsed = now - Map19_LastCycle;
  Map19_LastCycle = now;
  const unsigned counter = Map19_Irq & 0x7fff;
  if ((Map19_Irq & 0x8000) && counter < 0x7fff) {
    if (elapsed >= 0x7fff - counter) { Map19_Irq = 0xffff; IRQ_REQ; }
    else Map19_Irq += elapsed;
  }
  if (Map19_AudioDisabled || !elapsed) return;
  uint32_t ticks = elapsed / 15;
  unsigned remainder = elapsed % 15 + Map19_AudioDivider;
  ticks += remainder / 15;
  Map19_AudioDivider = remainder % 15;
  if (!ticks) return;
  const unsigned channels = ((Map19_Internal[0x7f] >> 4) & 7) + 1;
  // Usually at most eight updates per scanline. Using the native 32-bit
  // phase arithmetic avoids a 64-bit divide and extra batch state.
  while (ticks--) {
    Map19_Phase(Map19_AudioChannel);
    Map19_AudioChannel = Map19_AudioChannel <= 8 - channels ? 7 : Map19_AudioChannel - 1;
  }
}

static void Map19_SetChr(unsigned slot)
{
  const BYTE bank = Map19_Chr[slot];
  PPUBANK[slot] = bank >= 0xe0 && !(Map19_ChrControl & (slot < 4 ? 0x40 : 0x80))
    ? VRAMPAGE(bank & 1) : VROMPAGE(bank % (NesHeader.byVRomSize * 8));
}

static BYTE Map19_ReadSram(WORD address)
{
  return Map19_RamBytes ? SRAM[address & (Map19_RamBytes - 1)] : (BYTE)(address >> 8);
}

static void Map19_Sram(WORD address, BYTE data)
{
  const unsigned page = (address - 0x6000) >> 11;
  if (Map19_RamBytes && (Map19_WriteProtect & 0x40) && !(Map19_WriteProtect & (1 << page))) {
    SRAM[address & (Map19_RamBytes - 1)] = data;
    SRAMwritten = true;
  }
}

void Map19_Init()
{
  Map0_Init();
  if (!Map19_Internal) {
    Map19_Internal = (BYTE *)infones::memory::allocate(128);
    if (!Map19_Internal) return;
  }
  MapperInit = Map19_Init;
  MapperWrite = Map19_Write;
  MapperApu = Map19_Apu;
  MapperReadApu = Map19_ReadApu;
  MapperSram = Map19_Sram;
  MapperReadSram = Map19_ReadSram;
  MapperSramOwnsWrite = true;
  MapperHSync = Map19_HSync;
  ROMBANK2 = ROMLASTPAGE(1);
  ROMBANK3 = ROMLASTPAGE(0);
  Map19_ChrControl = Map19_WriteProtect = Map19_Address = 0;
  Map19_Irq = 0;
  Map19_LastCycle = 0; // K6502_Reset follows mapper initialization.
  Map19_AudioDivider = 0;
  Map19_AudioChannel = 7;
  Map19_AudioDisabled = false;
  InfoNES_MemorySet(Map19_Internal, 0, 128);
  Map19_RamBytes = 8192;
  if ((NesHeader.byInfo2 & 0x0c) == 8) {
    const BYTE sizes = NesHeader.byReserve[2];
    Map19_RamBytes = (sizes & 15 ? 64u << (sizes & 15) : 0) +
                    (sizes >> 4 ? 64u << (sizes >> 4) : 0);
  }
  for (unsigned n = 0; n < 8; ++n) Map19_Chr[n] = n;
}

void Map19_Write(WORD address, BYTE data)
{
  const WORD reg = address & 0xf800;
  if (reg >= 0x8000 && reg <= 0xb800) {
    const unsigned slot = (reg - 0x8000) / 0x800;
    Map19_Chr[slot] = data;
    Map19_SetChr(slot);
    InfoNES_SetupChr();
  } else if (reg >= 0xc000 && reg <= 0xd800) {
    PPUBANK[NAME_TABLE0 + (reg - 0xc000) / 0x800] = data >= 0xe0
      ? VRAMPAGE(data & 1) : VROMPAGE(data % (NesHeader.byVRomSize * 8));
  } else if (reg >= 0xe000 && reg <= 0xf000) {
    ROMBANK[(reg - 0xe000) / 0x800] = ROMPAGE((data & 63) % (NesHeader.byRomSize * 2));
    if (reg == 0xe000) {
      Map19_Sync();
      Map19_AudioDisabled = (data & 0x40) != 0;
    } else if (reg == 0xe800) {
      Map19_ChrControl = data;
      for (unsigned n = 0; n < 8; ++n) Map19_SetChr(n);
      InfoNES_SetupChr();
    }
  } else if (reg == 0xf800) {
    Map19_Sync();
    Map19_Address = data;
    Map19_WriteProtect = data;
  }
}

void Map19_Apu(WORD address, BYTE data)
{
  const WORD reg = address & 0xf800;
  if (reg < 0x4800 || reg > 0x5800) return;
  Map19_Sync();
  if (reg == 0x4800) {
    Map19_Internal[Map19_Address & 0x7f] = data;
    if (Map19_Address & 0x80) Map19_Address = 0x80 | ((Map19_Address + 1) & 0x7f);
  } else {
    if (reg == 0x5000) Map19_Irq = (Map19_Irq & 0xff00) | data;
    else Map19_Irq = (Map19_Irq & 0xff) | (WORD(data) << 8);
    IRQ_State = IRQ_Wiring;
  }
}

BYTE Map19_ReadApu(WORD address)
{
  const WORD reg = address & 0xf800;
  if (reg < 0x4800 || reg > 0x5800) return (BYTE)(address >> 8);
  Map19_Sync();
  if (reg == 0x4800) {
    const BYTE value = Map19_Internal[Map19_Address & 0x7f];
    if (Map19_Address & 0x80) Map19_Address = 0x80 | ((Map19_Address + 1) & 0x7f);
    return value;
  }
  return reg == 0x5000 ? (BYTE)Map19_Irq : (BYTE)(Map19_Irq >> 8);
}

void Map19_HSync()
{
  // Counter values track actual CPU cycles; IRQ delivery remains scanline
  // granularity between register accesses, like the other retained mappers.
  Map19_Sync();
}
