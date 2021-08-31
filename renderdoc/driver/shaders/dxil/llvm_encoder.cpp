/******************************************************************************
 * The MIT License (MIT)
 *
 * Copyright (c) 2021 Baldur Karlsson
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

#include "llvm_encoder.h"
#include "os/os_specific.h"

namespace LLVMBC
{
static uint32_t GetBlockAbbrevSize(KnownBlock block)
{
  uint32_t ret = 0;

  // the abbrev sizes seem to be hardcoded in llvm? At least this matches dxc's llvm
  switch(block)
  {
    case KnownBlock::BLOCKINFO: ret = 2; break;
    case KnownBlock::MODULE_BLOCK: ret = 3; break;
    case KnownBlock::PARAMATTR_BLOCK: ret = 3; break;
    case KnownBlock::PARAMATTR_GROUP_BLOCK: ret = 3; break;
    case KnownBlock::CONSTANTS_BLOCK: ret = 4; break;
    case KnownBlock::FUNCTION_BLOCK: ret = 4; break;
    case KnownBlock::VALUE_SYMTAB_BLOCK: ret = 4; break;
    case KnownBlock::METADATA_BLOCK: ret = 3; break;
    case KnownBlock::METADATA_ATTACHMENT: ret = 3; break;
    case KnownBlock::TYPE_BLOCK: ret = 4; break;
    case KnownBlock::USELIST_BLOCK: ret = 3; break;
    case KnownBlock::Count: break;
  }

  return ret;
}

#define MagicFixedSizeNumTypes 99

#define AbbFixed(n)          \
  {                          \
    AbbrevEncoding::Fixed, n \
  }
#define AbbVBR(n)          \
  {                        \
    AbbrevEncoding::VBR, n \
  }
#define AbbArray()           \
  {                          \
    AbbrevEncoding::Array, 0 \
  }
#define AbbLiteral(lit)                    \
  {                                        \
    AbbrevEncoding::Literal, uint64_t(lit) \
  }
#define AbbChar6()           \
  {                          \
    AbbrevEncoding::Char6, 0 \
  }

#define AbbFixedTypes() AbbFixed(MagicFixedSizeNumTypes)

using AbbrevDefinition = AbbrevParam[8];

// known abbreviations. Encoded as an array of abbrevs, with each one being an array of params (the
// last param will have AbbrevEncoding::Unknown == 0)
enum class ValueSymtabAbbrev
{
  Entry8,
  Entry7,
  Entry6,
  BBEntry6,
};

AbbrevDefinition ValueSymtabAbbrevDefs[] = {
    // Entry8
    {
        AbbFixed(3), AbbVBR(8), AbbArray(), AbbFixed(8),
    },
    // Entry7
    {
        AbbLiteral(ValueSymtabRecord::ENTRY), AbbVBR(8), AbbArray(), AbbFixed(7),
    },
    // Entry6
    {
        AbbLiteral(ValueSymtabRecord::ENTRY), AbbVBR(8), AbbArray(), AbbChar6(),
    },
    // BBEntry6
    {
        AbbLiteral(ValueSymtabRecord::BBENTRY), AbbVBR(8), AbbArray(), AbbChar6(),
    },
};

enum class ConstantsAbbrev
{
  SetType,
  Integer,
  EvalCast,
  Null,
};

AbbrevDefinition ConstantsAbbrevDefs[] = {
    // SetType
    {
        AbbLiteral(ConstantsRecord::SETTYPE), AbbFixedTypes(),
    },
    // Integer
    {
        AbbLiteral(ConstantsRecord::INTEGER), AbbVBR(8),
    },
    // EvalCast
    {
        AbbLiteral(ConstantsRecord::EVAL_CAST), AbbFixed(4), AbbFixedTypes(), AbbVBR(8),
    },
    // Null
    {
        AbbLiteral(ConstantsRecord::CONST_NULL),
    },
};

enum class FunctionAbbrev
{
  Load,
  BinOp,
  BinOpFlags,
  Cast,
  RetVoid,
  RetValue,
  Unreachable,
  GEP,
};

AbbrevDefinition FunctionAbbrevDefs[] = {
    // Load
    {
        AbbLiteral(FunctionRecord::INST_LOAD), AbbVBR(6), AbbFixedTypes(), AbbVBR(4), AbbFixed(1),
    },
    // BinOp
    {
        AbbLiteral(FunctionRecord::INST_BINOP), AbbVBR(6), AbbVBR(6), AbbFixed(4),
    },
    // BinOpFlags
    {
        AbbLiteral(FunctionRecord::INST_BINOP), AbbVBR(6), AbbVBR(6), AbbFixed(4), AbbFixed(7),
    },
    // Cast
    {
        AbbLiteral(FunctionRecord::INST_CAST), AbbVBR(6), AbbFixedTypes(), AbbFixed(4),
    },
    // RetVoid
    {
        AbbLiteral(FunctionRecord::INST_RET),
    },
    // RetValue
    {
        AbbLiteral(FunctionRecord::INST_RET), AbbVBR(6),
    },
    // Unreachable
    {
        AbbLiteral(FunctionRecord::INST_UNREACHABLE),
    },
    // GEP
    {
        AbbLiteral(FunctionRecord::INST_GEP), AbbFixed(1), AbbFixedTypes(), AbbArray(), AbbVBR(6),
    },
};

static AbbrevDefinition *GetAbbrevs(KnownBlock block)
{
  AbbrevDefinition *ret = NULL;

  switch(block)
  {
    case KnownBlock::VALUE_SYMTAB_BLOCK: ret = ValueSymtabAbbrevDefs; break;
    case KnownBlock::CONSTANTS_BLOCK: ret = ConstantsAbbrevDefs; break;
    case KnownBlock::FUNCTION_BLOCK: ret = FunctionAbbrevDefs; break;
    default: break;
  }

  return ret;
}

static uint32_t GetNumAbbrevs(KnownBlock block)
{
  uint32_t ret = 0;

  switch(block)
  {
    case KnownBlock::VALUE_SYMTAB_BLOCK: ret = ARRAY_COUNT(ValueSymtabAbbrevDefs); break;
    case KnownBlock::CONSTANTS_BLOCK: ret = ARRAY_COUNT(ConstantsAbbrevDefs); break;
    case KnownBlock::FUNCTION_BLOCK: ret = ARRAY_COUNT(FunctionAbbrevDefs); break;
    default: break;
  }

  return ret;
}

BitcodeWriter::BitcodeWriter(bytebuf &buf) : b(buf)
{
  b.Write(LLVMBC::BitcodeMagic);

  curBlock = KnownBlock::Count;
  abbrevSize = 2;
}

BitcodeWriter::~BitcodeWriter()
{
}

void BitcodeWriter::BeginBlock(KnownBlock block)
{
  uint32_t newAbbrevSize = GetBlockAbbrevSize(block);

  if(newAbbrevSize == 0)
  {
    RDCERR("Encoding error: unrecognised block %u", block);
    return;
  }

  b.fixed(abbrevSize, ENTER_SUBBLOCK);
  b.vbr(8, block);
  b.vbr(4, newAbbrevSize);
  b.align32bits();

  size_t offs = b.GetByteOffset();

  // write a placeholder length
  b.Write<uint32_t>(0U);

  curBlock = block;
  abbrevSize = newAbbrevSize;
  blockStack.push_back({block, offs});
}

void BitcodeWriter::EndBlock()
{
  b.vbr(abbrevSize, END_BLOCK);
  b.align32bits();

  size_t offs = blockStack.back().second;

  // -4 because we don't include the word with the length itself
  size_t lengthInBytes = b.GetByteOffset() - offs - 4;

  b.PatchLengthWord(offs, uint32_t(lengthInBytes / 4));

  blockStack.pop_back();
  if(blockStack.empty())
  {
    curBlock = KnownBlock::Count;
    abbrevSize = 2;
  }
  else
  {
    curBlock = blockStack.back().first;
    abbrevSize = GetBlockAbbrevSize(curBlock);
  }
}

void BitcodeWriter::ModuleBlockInfo(uint32_t numTypes)
{
  // these abbrevs are hardcoded in llvm, at least at dxc's version
  BeginBlock(KnownBlock::BLOCKINFO);

  // the module-level blockinfo contains abbrevs for these block types that can be repeated
  // subblocks
  for(KnownBlock block :
      {KnownBlock::VALUE_SYMTAB_BLOCK, KnownBlock::CONSTANTS_BLOCK, KnownBlock::FUNCTION_BLOCK})
  {
    Unabbrev((uint32_t)BlockInfoRecord::SETBID, (uint32_t)block);
    AbbrevDefinition *abbrevs = GetAbbrevs(block);
    uint32_t numAbbrevs = GetNumAbbrevs(block);

    for(uint32_t i = 0; i < numAbbrevs; i++)
    {
      b.fixed(abbrevSize, DEFINE_ABBREV);

      AbbrevParam *abbrev = abbrevs[i];

      uint32_t numParams = 0;
      while(abbrev[numParams].encoding != AbbrevEncoding::Unknown)
        numParams++;

      b.vbr(5, numParams);

      for(uint32_t p = 0; p < numParams; p++)
      {
        AbbrevParam param = abbrev[p];

        if(param.value == MagicFixedSizeNumTypes)
        {
          param.value = 32 - Bits::CountLeadingZeroes(numTypes);
        }

        const bool lit = param.encoding == AbbrevEncoding::Literal;
        b.fixed<bool>(1, lit);
        if(lit)
        {
          b.vbr(8, param.value);
        }
        else
        {
          b.fixed(3, param.encoding);
          if(param.encoding == AbbrevEncoding::VBR || param.encoding == AbbrevEncoding::Fixed)
            b.vbr(5, param.value);
        }
      }
    }
  }

  EndBlock();
}

void BitcodeWriter::Unabbrev(uint32_t record, uint32_t val)
{
  b.fixed(abbrevSize, UNABBREV_RECORD);
  b.vbr(6, record);
  b.vbr(6, 1U);    // num parameters
  b.vbr(6, val);
}

void BitcodeWriter::Unabbrev(uint32_t record, uint64_t val)
{
  b.fixed(abbrevSize, UNABBREV_RECORD);
  b.vbr(6, record);
  b.vbr(6, 1U);    // num parameters
  b.vbr(6, val);
}

void BitcodeWriter::Unabbrev(uint32_t record, const rdcarray<uint32_t> &vals)
{
  b.fixed(abbrevSize, UNABBREV_RECORD);
  b.vbr(6, record);
  b.vbr(6, vals.size());
  for(uint32_t v : vals)
    b.vbr(6, v);
}

void BitcodeWriter::Unabbrev(uint32_t record, const rdcarray<uint64_t> &vals)
{
  b.fixed(abbrevSize, UNABBREV_RECORD);
  b.vbr(6, record);
  b.vbr(6, vals.size());
  for(uint64_t v : vals)
    b.vbr(6, v);
}
};

#if ENABLED(ENABLE_UNIT_TESTS)

#include "catch/catch.hpp"

#include "llvm_decoder.h"

TEST_CASE("Check LLVM bitwriter", "[llvm]")
{
  bytebuf bits;

  SECTION("Check simple writing of bytes")
  {
    LLVMBC::BitWriter w(bits);

    w.Write<byte>(0x01);
    w.Write<byte>(0x02);
    w.Write<byte>(0x40);
    w.Write<byte>(0x80);
    w.Write<byte>(0xff);

    w.align32bits();

    LLVMBC::BitReader r(bits.data(), bits.size());

    CHECK(r.Read<byte>() == 0x01);
    CHECK(r.Read<byte>() == 0x02);
    CHECK(r.Read<byte>() == 0x40);
    CHECK(r.Read<byte>() == 0x80);
    CHECK(r.Read<byte>() == 0xff);

    r.align32bits();

    CHECK(r.AtEndOfStream());
  }

  SECTION("Check fixed encoding")
  {
    uint32_t val = 0x3CA5F096;

    LLVMBC::BitWriter w(bits);

    for(uint32_t i = 0; i < 32; i++)
      w.fixed(i + 1, val);

    w.align32bits();

    LLVMBC::BitReader r(bits.data(), bits.size());

    for(uint32_t i = 0; i < 32; i++)
    {
      CHECK(r.fixed<uint32_t>(i + 1) == (val & ((1ULL << (i + 1)) - 1)));
    }

    r.align32bits();

    CHECK(r.AtEndOfStream());
  }

  SECTION("Check variable encoding")
  {
    LLVMBC::BitWriter w(bits);

    w.vbr<uint32_t>(8, 0x12);
    w.vbr<uint32_t>(6, 0x12);
    w.vbr<uint32_t>(5, 0x12);
    w.vbr<uint32_t>(4, 0x12);
    w.vbr<uint32_t>(3, 0x12);

    w.vbr<uint32_t>(8, 0x12345678);
    w.vbr<uint32_t>(6, 0x12345678);
    w.vbr<uint32_t>(5, 0x12345678);
    w.vbr<uint32_t>(4, 0x12345678);
    w.vbr<uint32_t>(3, 0x12345678);

    w.vbr<uint64_t>(8, 0x123456789ABCDEFULL);
    w.vbr<uint64_t>(6, 0x123456789ABCDEFULL);
    w.vbr<uint64_t>(5, 0x123456789ABCDEFULL);
    w.vbr<uint64_t>(4, 0x123456789ABCDEFULL);
    w.vbr<uint64_t>(3, 0x123456789ABCDEFULL);

    w.align32bits();

    LLVMBC::BitReader r(bits.data(), bits.size());

    CHECK(r.vbr<uint32_t>(8) == 0x12);
    CHECK(r.vbr<uint32_t>(6) == 0x12);
    CHECK(r.vbr<uint32_t>(5) == 0x12);
    CHECK(r.vbr<uint32_t>(4) == 0x12);
    CHECK(r.vbr<uint32_t>(3) == 0x12);

    CHECK(r.vbr<uint32_t>(8) == 0x12345678);
    CHECK(r.vbr<uint32_t>(6) == 0x12345678);
    CHECK(r.vbr<uint32_t>(5) == 0x12345678);
    CHECK(r.vbr<uint32_t>(4) == 0x12345678);
    CHECK(r.vbr<uint32_t>(3) == 0x12345678);

    CHECK(r.vbr<uint64_t>(8) == 0x123456789ABCDEFULL);
    CHECK(r.vbr<uint64_t>(6) == 0x123456789ABCDEFULL);
    CHECK(r.vbr<uint64_t>(5) == 0x123456789ABCDEFULL);
    CHECK(r.vbr<uint64_t>(4) == 0x123456789ABCDEFULL);
    CHECK(r.vbr<uint64_t>(3) == 0x123456789ABCDEFULL);

    r.align32bits();

    CHECK(r.AtEndOfStream());
  }

  SECTION("Check signed vbr encoding")
  {
    LLVMBC::BitWriter w(bits);

    w.vbr<uint64_t>(4, LLVMBC::BitWriter::svbr(0x12));
    w.vbr<uint64_t>(4, LLVMBC::BitWriter::svbr(-0x12));

    w.vbr<uint64_t>(4, LLVMBC::BitWriter::svbr(0x1234));
    w.vbr<uint64_t>(4, LLVMBC::BitWriter::svbr(-0x1234));

    w.vbr<uint64_t>(4, LLVMBC::BitWriter::svbr(0x12345678));
    w.vbr<uint64_t>(4, LLVMBC::BitWriter::svbr(-0x12345678));

    w.vbr<uint64_t>(4, LLVMBC::BitWriter::svbr(INT_MAX));
    w.vbr<uint64_t>(4, LLVMBC::BitWriter::svbr(-INT_MAX));

    w.align32bits();

    CHECK(bits.size() == 28);

    LLVMBC::BitReader r(bits.data(), bits.size());

    CHECK(LLVMBC::BitReader::svbr(r.vbr<uint32_t>(4)) == 0x12);
    CHECK(LLVMBC::BitReader::svbr(r.vbr<uint32_t>(4)) == -0x12);

    CHECK(LLVMBC::BitReader::svbr(r.vbr<uint32_t>(4)) == 0x1234);
    CHECK(LLVMBC::BitReader::svbr(r.vbr<uint32_t>(4)) == -0x1234);

    CHECK(LLVMBC::BitReader::svbr(r.vbr<uint32_t>(4)) == 0x12345678);
    CHECK(LLVMBC::BitReader::svbr(r.vbr<uint32_t>(4)) == -0x12345678);

    CHECK(LLVMBC::BitReader::svbr(r.vbr<uint32_t>(4)) == INT_MAX);
    CHECK(LLVMBC::BitReader::svbr(r.vbr<uint32_t>(4)) == -INT_MAX);

    r.align32bits();

    CHECK(r.AtEndOfStream());
  }

  SECTION("Check char6 encoding")
  {
    LLVMBC::BitWriter w(bits);

    const char string[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._";

    for(size_t i = 0; i < strlen(string); i++)
      w.c6(string[i]);

    w.align32bits();

    LLVMBC::BitReader r(bits.data(), bits.size());

    for(size_t i = 0; i < strlen(string); i++)
      CHECK(r.c6() == string[i]);

    r.align32bits();

    CHECK(r.AtEndOfStream());
  }

  SECTION("Check blobs")
  {
    bytebuf foo = {0x01, 0x02, 0x40, 0x80, 0xff};
    for(byte i = 0; i < 250; i++)
      foo.push_back(i);

    foo.push_back(0x80);
    foo.push_back(0x70);
    foo.push_back(0x60);

    LLVMBC::BitWriter w(bits);

    w.WriteBlob(foo);

    w.align32bits();

    LLVMBC::BitReader r(bits.data(), bits.size());

    const byte *ptr = NULL;
    size_t len = 0;
    r.ReadBlob(ptr, len);

    r.align32bits();

    CHECK(r.AtEndOfStream());

    REQUIRE(len == foo.size());
    CHECK(bytebuf(ptr, len) == foo);
  }
}

#endif