/* Mapper 22: VRC2a, swapped A0/A1 and CHR A0 disconnected. */
static BYTE Map22_Chr[8];
static BYTE Map22_Latch;
static unsigned Map22_RamBytes;

static BYTE Map22_ReadSram(WORD address)
{
  if (Map22_RamBytes) return SRAM[address & (Map22_RamBytes - 1)];
  return (BYTE)((address >> 8 & 0xfe) |
    (address < 0x7000 ? Map22_Latch : address >> 8 & 1));
}

static void Map22_Sram(WORD address, BYTE data)
{
  if (Map22_RamBytes) {
    SRAM[address & (Map22_RamBytes - 1)] = data;
    SRAMwritten = true;
  } else if (address < 0x7000) {
    Map22_Latch = data & 1;
  }
}

void Map22_Init()
{
  Map0_Init();
  MapperInit = Map22_Init;
  MapperWrite = Map22_Write;
  MapperSram = Map22_Sram;
  MapperReadSram = Map22_ReadSram;
  MapperSramOwnsWrite = true;
  Map22_Latch = 0;
  Map22_RamBytes = 8192; // iNES does not specify absent WRAM reliably.
  if ((NesHeader.byInfo2 & 0x0c) == 8) {
    BYTE sizes = NesHeader.byReserve[2];
    Map22_RamBytes = (sizes & 15 ? 64u << (sizes & 15) : 0) +
                    (sizes >> 4 ? 64u << (sizes >> 4) : 0);
  }
  for (unsigned n = 0; n < 8; ++n) Map22_Chr[n] = n * 2;
  ROMBANK2 = ROMLASTPAGE(1);
  ROMBANK3 = ROMLASTPAGE(0);
}

void Map22_Write(WORD address, BYTE data)
{
  const unsigned group = address >> 12;
  if (group == 8 || group == 10) {
    ROMBANK[group == 8 ? 0 : 1] = ROMPAGE((data & 31) % (NesHeader.byRomSize * 2));
  } else if (group == 9) {
    InfoNES_Mirroring(data & 1 ? 0 : 1);
  } else if (group >= 11 && group <= 14) {
    const unsigned slot = (group - 11) * 2 + (address & 1);
    const unsigned shift = address & 2 ? 4 : 0;
    Map22_Chr[slot] = (Map22_Chr[slot] & ~(15u << shift)) | ((data & 15) << shift);
    PPUBANK[slot] = VROMPAGE((Map22_Chr[slot] >> 1) % (NesHeader.byVRomSize * 8));
    InfoNES_SetupChr();
  }
}
