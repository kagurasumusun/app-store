// SPDX-License-Identifier: MIT
//
// Pure-software implementation of Apple's FairPlay SAP (protocol 200) setup
// handshake and request signing. Everything below is an independent
// implementation written for this project; it consumes the fixed protocol
// constants documented in ThirdParty/sap-tables/README.md. No private Apple
// framework, emulator or extracted system image is involved.

#include "ipashelf/sap_software.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <optional>
#include <utility>

#include "ipashelf/crypto.hpp"
#include "ipashelf/sap_tables.hpp"

namespace ipashelf {
namespace {

using sap_tables::kByteTables;
using sap_tables::kCarryMasks;
using sap_tables::kF2Tables;
using sap_tables::kRngTables;
using sap_tables::kWordTables;

constexpr std::uint8_t kMaskByte = 0x2d;

const std::uint8_t* byte_table(std::size_t index) {
  return kByteTables + 256 * index;
}

std::uint32_t static_word(std::size_t table, std::size_t index) {
  const std::uint8_t* cell = kWordTables + 1024 * table + 4 * index;
  return static_cast<std::uint32_t>(cell[0]) |
         (static_cast<std::uint32_t>(cell[1]) << 8) |
         (static_cast<std::uint32_t>(cell[2]) << 16) |
         (static_cast<std::uint32_t>(cell[3]) << 24);
}

const std::uint8_t* carry_mask(std::size_t table, std::size_t* length) {
  static constexpr std::size_t kLengths[4] = {233, 239, 241, 251};
  static constexpr std::size_t kOffsets[4] = {0, 233, 472, 713};
  *length = kLengths[table];
  return kCarryMasks + kOffsets[table];
}

// RNG table layout inside kRngTables (see the generator script).
const std::uint8_t* iv_whiten() { return kRngTables; }
const std::uint8_t* f1_whiten() { return kRngTables + 256; }
const std::uint8_t* exchange_whiten() { return kRngTables + 512; }
const std::uint8_t* initialize_whiten() { return kRngTables + 768; }
const std::uint8_t* f1_template() { return kRngTables + 1024; }
const std::uint8_t* f1_g(std::size_t table) {
  return kRngTables + 1280 + 256 * table;
}
const std::uint8_t* f1_rc() { return kRngTables + 2304; }

void append_be32(Bytes& output, std::uint32_t value) {
  output.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
  output.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
  output.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
  output.push_back(static_cast<std::uint8_t>(value & 0xff));
}

std::uint32_t read_be32(const std::uint8_t* bytes) {
  return (static_cast<std::uint32_t>(bytes[0]) << 24) |
         (static_cast<std::uint32_t>(bytes[1]) << 16) |
         (static_cast<std::uint32_t>(bytes[2]) << 8) |
         static_cast<std::uint32_t>(bytes[3]);
}

// ---------------------------------------------------------------------------
// Pseudo-random generator
//
// A Mersenne-Twister variant whose twist step runs over the whole state on
// every single output word, combined with address-salted byte whitening.
// The "address" arguments below are fixed arithmetic salts recovered from
// the original layout; they are never host pointers.
// ---------------------------------------------------------------------------

struct SapRng {
  std::array<std::uint32_t, 624> words{};
  std::uint32_t cursor = 0;

  std::uint32_t word() {
    for (std::size_t i = 0; i < words.size(); ++i) {
      const std::uint32_t y =
          (words[i] & 0x80000000u) | (words[(i + 1) % 624] & 0x7fffffffu);
      words[i] = words[(i + 397) % 624] ^ (y >> 1) ^
                 ((y & 1u) != 0 ? 0x9908b0dfu : 0u);
    }
    if (cursor >= 624) cursor = 0;
    std::uint32_t y = words[cursor++];
    y ^= y >> 11;
    y ^= (y << 7) & 0x9d2c5680u;
    y ^= (y << 15) & 0xefc60000u;
    y ^= y >> 18;
    return y;
  }

  // Fills `output` from the last byte backwards. Zero bytes are retried with
  // the next lane/word, so the stream advance is data-dependent.
  void fill(std::uint32_t seed, std::uint32_t address,
            const std::uint8_t* table, std::uint8_t mask, std::uint8_t* output,
            std::size_t count) {
    std::size_t remain = count;
    while (remain != 0) {
      const std::uint32_t next = word();
      for (unsigned shift = 0; shift < 32 && remain != 0; shift += 8) {
        std::uint32_t h =
            (seed ^ (address + static_cast<std::uint32_t>(remain) - 1u)) &
            0x7fffffffu;
        h *= 0x84edb2ebu;
        h ^= h >> 16;
        h *= 0x84edb2ebu;
        const auto value = static_cast<std::uint8_t>(
            ((next >> shift) & 0xffu) ^ (h & 0xffu) ^ table[h >> 24] ^ mask);
        output[remain - 1] = value;
        if (value != 0) --remain;
      }
    }
  }

  void initialize(std::uint32_t mt_seed, std::uint32_t key_word3,
                  std::uint32_t key_word0, std::uint32_t seconds,
                  std::uint32_t microseconds,
                  std::array<std::uint32_t, 128>& pool) {
    std::uint32_t keys[4] = {key_word0, microseconds, seconds, key_word3};
    words[0] = mt_seed;
    for (std::size_t i = 1; i < words.size(); ++i) {
      const std::uint32_t previous = words[i - 1];
      words[i] = 1812433253u * (previous ^ (previous >> 30)) +
                 static_cast<std::uint32_t>(i);
    }
    std::size_t i = 1;
    std::size_t j = 0;
    for (int k = 0; k < 624; ++k) {
      const std::uint32_t previous = words[i - 1];
      words[i] = (words[i] ^ ((previous ^ (previous >> 30)) * 1664525u)) +
                 keys[j] + static_cast<std::uint32_t>(j);
      if (++i == 624) {
        words[0] = words[623];
        i = 1;
      }
      if (++j == 4) {
        keys[0] ^= 0x200u;
        j = 0;
      } else {
        keys[j] ^= 0x201u;
      }
    }
    for (int k = 0; k < 623; ++k) {
      const std::uint32_t previous = words[i - 1];
      words[i] = (words[i] ^ ((previous ^ (previous >> 30)) * 1566083941u)) -
                 static_cast<std::uint32_t>(i);
      if (++i == 624) {
        words[0] = words[623];
        i = 1;
      }
    }
    words[0] = 0x80000000u;
    cursor = 0;
    for (std::uint32_t& entry : pool) entry = word();
    for (std::size_t k = 0; k < 64; k += 4) {
      pool[k] |= ~pool[pool[k] & 62u] << 16;
    }
    std::array<std::uint8_t, 1024> auxiliary{};
    fill(pool[0x30 / 4], 0x7ff9b0u, initialize_whiten(), 0x35,
         auxiliary.data(), auxiliary.size());
  }
};

void f1_round_g(const std::uint8_t high[64], std::uint8_t output[64]) {
  std::uint8_t a[80];
  std::memcpy(a, high, 64);
  std::memcpy(a + 64, f1_rc(), 16);
  for (unsigned i = 0; i < 80; ++i) {
    a[i] = static_cast<std::uint8_t>(((a[i] ^ kMaskByte) + i) & 0xff);
  }
  for (unsigned i = 0; i < 80; ++i) {
    a[i] = static_cast<std::uint8_t>((f1_g(0)[a[i]] + 0x25 + i) & 0xff);
  }
  static constexpr std::uint8_t kConstants[4] = {0x5d, 0xae, 0xff, 0x50};
  for (unsigned round = 0; round < 4; ++round) {
    for (unsigned i = 0; i < 80; ++i) {
      a[i] = static_cast<std::uint8_t>(
          a[i] ^ static_cast<std::uint8_t>((kConstants[round] + i +
                                             f1_g(1)[a[(i + 75) % 80]] +
                                             f1_g(2)[a[(i + 55) % 80]]) &
                                            0xff));
    }
  }
  for (unsigned i = 0; i < 80; ++i) {
    a[i] = static_cast<std::uint8_t>((f1_g(3)[a[i]] + 0xc4 + i) & 0xff);
  }
  std::memcpy(output, a, 64);
}

void f1_from_material(const std::uint8_t material[78],
                      std::uint8_t output[256]) {
  std::uint8_t pre[256];
  std::memcpy(pre, f1_template(), 256);
  std::memcpy(pre + 68, material, 60);
  std::memcpy(pre + 238, material + 60, 18);
  for (unsigned half = 0; half < 2; ++half) {
    std::uint8_t low[64];
    std::uint8_t high[64];
    std::uint8_t g[64];
    std::memcpy(low, pre + half * 128, 64);
    std::memcpy(high, pre + half * 128 + 64, 64);
    for (unsigned round = 0; round < 7; ++round) {
      f1_round_g(high, g);
      for (unsigned i = 0; i < 64; ++i) {
        const std::uint8_t old = low[i];
        low[i] = high[i];
        high[i] = static_cast<std::uint8_t>(old ^ g[i]);
      }
    }
    for (unsigned i = 0; i < 64; ++i) {
      output[half * 128 + i] = static_cast<std::uint8_t>(low[i] ^ kMaskByte);
      output[half * 128 + 64 + i] =
          static_cast<std::uint8_t>(high[i] ^ kMaskByte);
    }
  }
}

// ---------------------------------------------------------------------------
// Source expansion: 576 server-provided bytes become cipher material, two
// auxiliary word tables, two coefficient caches and byte caches.
// ---------------------------------------------------------------------------

constexpr std::uint8_t kPadColumns[11][16] = {
    {0x00, 0x2e, 0x79, 0x60, 0xa4, 0x12, 0x40, 0xad, 0x22, 0xd5, 0xaf, 0x42,
     0xb1, 0x49, 0x7b, 0x0e},
    {0x00, 0x41, 0x84, 0x24, 0x27, 0x14, 0x9e, 0xd7, 0xfe, 0x6f, 0x93, 0xda,
     0x38, 0x6e, 0xc0, 0xfb},
    {0x00, 0x41, 0x84, 0x24, 0x27, 0x14, 0x9e, 0xd7, 0xfe, 0x6f, 0x93, 0xda,
     0x38, 0x6e, 0xc0, 0xfb},
    {0x00, 0x41, 0x84, 0x24, 0x27, 0x14, 0x9e, 0xd7, 0xfe, 0x6f, 0x93, 0xda,
     0x38, 0x6e, 0xc0, 0xfb},
    {0x00, 0x41, 0x84, 0x24, 0x27, 0x14, 0x9e, 0xd7, 0xfe, 0x6f, 0x93, 0xda,
     0x38, 0x6e, 0xc0, 0xfb},
    {0x00, 0x41, 0x84, 0x24, 0x27, 0x14, 0x9e, 0xd7, 0xfe, 0x6f, 0x93, 0xda,
     0x38, 0x6e, 0xc0, 0xfb},
    {0x00, 0x41, 0x84, 0x24, 0x27, 0x14, 0x9e, 0xd7, 0xfe, 0x6f, 0x93, 0xda,
     0x38, 0x6e, 0xc0, 0xfb},
    {0x00, 0x41, 0x84, 0x24, 0x27, 0x14, 0x9e, 0xd7, 0xfe, 0x6f, 0x93, 0xda,
     0x38, 0x6e, 0xc0, 0xfb},
    {0x00, 0x41, 0x84, 0x24, 0x27, 0x14, 0x9e, 0xd7, 0xfe, 0x6f, 0x93, 0xda,
     0x38, 0x6e, 0xc0, 0xfb},
    {0x00, 0x41, 0x84, 0x24, 0x27, 0x14, 0x9e, 0xd7, 0xfe, 0x6f, 0x93, 0xda,
     0x38, 0x6e, 0xc0, 0xfb},
    {0x00, 0xbf, 0x39, 0x19, 0xe1, 0x30, 0xf6, 0x03, 0x1d, 0x72, 0xf1, 0x04,
     0x63, 0x14, 0xcb, 0x50},
};

constexpr std::uint8_t kPadRows[11][16] = {
    {0x4d, 0x01, 0xfd, 0x1f, 0xbd, 0xb5, 0xd0, 0xc4, 0xf2, 0xff, 0x9c, 0x5f,
     0x0d, 0x8f, 0x47, 0x61},
    {0x3e, 0x15, 0x55, 0x00, 0xc9, 0x2e, 0x05, 0x73, 0x67, 0x03, 0x74, 0x0a,
     0x94, 0x5a, 0x91, 0x62},
    {0x91, 0xf9, 0xb5, 0x2f, 0x28, 0xa7, 0xc0, 0x2c, 0x3f, 0xd4, 0xc4, 0x56,
     0xdb, 0xfe, 0x25, 0x44},
    {0x0f, 0x30, 0x9d, 0x17, 0x57, 0xe7, 0x2d, 0x4b, 0x18, 0x43, 0x99, 0x6d,
     0xb3, 0xcd, 0xcc, 0x59},
    {0x94, 0xe2, 0x6c, 0xe1, 0xb3, 0x75, 0x31, 0xda, 0xdb, 0x46, 0xd8, 0xc7,
     0x18, 0xfb, 0x64, 0xee},
    {0x45, 0xd0, 0x7f, 0x57, 0x86, 0xd5, 0x3e, 0xfd, 0x2d, 0xe3, 0x96, 0x4a,
     0x45, 0x68, 0x82, 0xd4},
    {0xa4, 0xac, 0x08, 0x0e, 0x52, 0x09, 0x46, 0x83, 0x0f, 0x9a, 0xa0, 0xb9,
     0x3a, 0x82, 0x52, 0x1d},
    {0x3b, 0xbd, 0xc1, 0x21, 0x19, 0xc4, 0xf7, 0xd2, 0x66, 0x2e, 0x27, 0x1b,
     0x2c, 0xdc, 0x05, 0x76},
    {0x24, 0xcf, 0x99, 0x39, 0x4d, 0x7b, 0x1e, 0x9b, 0x5b, 0x25, 0x49, 0xf0,
     0x07, 0x89, 0x3c, 0xf6},
    {0x5a, 0x1f, 0x9a, 0x95, 0x67, 0x14, 0xf4, 0x7e, 0x4c, 0x41, 0xcd, 0xfe,
     0x3b, 0xb8, 0x81, 0x78},
    {0x61, 0x5d, 0x85, 0xdf, 0xa9, 0x9d, 0xbc, 0x20, 0xe1, 0xd2, 0x80, 0xcd,
     0x8a, 0x53, 0x04, 0x87},
};

constexpr int kInitialOrder[16] = {13, 0, 11, 10, 6, 12, 15, 1,
                                   5,  14, 2, 4,  3, 8,  9,  7};
constexpr int kTerminalOrder[16] = {13, 4, 8, 9, 14, 1, 7, 15,
                                    12, 2, 6, 0, 11, 3, 10, 5};

struct PreparedSource {
  std::array<std::uint8_t, 576> material{};
  std::array<std::uint32_t, 256> auxiliary0{};
  std::array<std::uint32_t, 256> auxiliary1{};
  Bytes cache2;
  Bytes cache3;
  std::uint32_t projection_mask = 0;
  std::array<std::uint8_t, 16> selectors{};
  std::array<std::uint32_t, 16> initial_cache{};
  std::array<std::uint32_t, 16> terminal_cache{};
};

struct SourceSchedule {
  int lengths[16][11]{};
  std::array<std::uint8_t, 16> selectors{};
};

SourceSchedule make_schedule(const std::uint8_t* source) {
  SourceSchedule schedule;
  for (int column = 0; column < 11; ++column) {
    const int bank = std::array<int, 3>{0xe0, 0xf0, 0xc0}[column / 4];
    const int moduli[3][4] = {{7, 27, 47, 77}, {9, 29, 49, 79}, {5, 25, 45, 75}};
    for (int group = 0; group < 4; ++group) {
      const int at = 4 * (column % 4) + group;
      const int value = source[0x30 + at] ^ source[bank + at];
      for (int lane = 0; lane < 4; ++lane) {
        const int digit = (value % moduli[column / 4][lane]) & 3;
        schedule.lengths[4 * group + lane][column] =
            std::array<int, 4>{3, 3, 5, 1}[digit];
      }
    }
    int cost = 0;
    for (const auto& row : schedule.lengths) cost += row[column];
    int row = (source[0x31] ^ source[0xc1]) & 15;
    while (cost > 32) {
      cost -= schedule.lengths[row][column] - 1;
      schedule.lengths[row][column] = 1;
      row = (row + 5) & 15;
    }
  }
  for (int index = 0; index < 8; ++index) {
    const int value = source[0xe0 + index] ^ source[0xf0 + index];
    schedule.selectors[2 * index] = static_cast<std::uint8_t>(value % 7);
    schedule.selectors[2 * index + 1] =
        static_cast<std::uint8_t>((value / 7) % 7);
  }
  return schedule;
}

struct SourceTables {
  std::array<std::uint32_t, 256> words{};
  Bytes cache;
  std::uint32_t mask = 0;
  std::array<std::uint32_t, 9> seeds{};
};

Result<SourceTables> make_source_tables(const std::uint8_t* source) {
  const int size = ((source[0xfa] ^ source[0xca]) << 8) |
                   (source[0xf3] ^ source[0xc3]);
  if (size <= 0) {
    return make_error(ErrorCode::kParse,
                      "SAP source inverse cache must not be empty");
  }
  static constexpr int kPairs[4][2] = {
      {0xea, 0xfa}, {0xe4, 0xf4}, {0xed, 0xfd}, {0xe6, 0xf6}};
  std::uint32_t seed = 0;
  for (const auto& pair : kPairs) {
    seed = (seed << 8) |
           static_cast<std::uint32_t>(source[pair[0]] ^ source[pair[1]]);
  }
  std::array<std::uint32_t, 13> states{};
  std::uint32_t state = seed;
  for (std::uint32_t& entry : states) {
    const std::uint64_t wide = state;
    state = static_cast<std::uint32_t>(((wide * wide) + 1) % 0xfffffffbu);
    entry = state;
  }
  std::uint32_t mask = 1u << (seed & 7u);
  for (int lane = 1; lane < 4; ++lane) {
    mask |= 1u << (8 * lane + static_cast<int>(states[lane - 1] & 7u));
  }
  SourceTables tables;
  tables.mask = mask;
  for (int index = 0; index < 9; ++index) tables.seeds[index] = states[3 + index];
  const std::uint32_t header = states[11];
  state = states[12];
  tables.cache.resize(static_cast<std::size_t>(size));
  for (int index = 0; index < size; ++index) {
    const int at = (index + 3 * source[index % 576] + 2) % 576;
    tables.cache[index] = static_cast<std::uint8_t>(source[at] ^ 0xd6);
  }
  for (int index = 0; index < 256; ++index) {
    std::uint32_t word = header ^ (state & mask);
    for (int bit = 0; bit < 8; ++bit) {
      if ((index & (1 << bit)) != 0) word ^= states[3 + bit];
    }
    tables.words[index] = word;
    tables.cache[(word & ~mask) % static_cast<std::uint32_t>(size)] =
        static_cast<std::uint8_t>(index ^ 0xd6);
    const std::uint32_t value = state ^ 0x63u;
    // The original signed widening contributes residue 512 modulo 576.
    const std::uint64_t correction =
        index != 0 && (state & 0x80000000u) != 0 ? 512 : 0;
    const std::size_t position = static_cast<std::size_t>(
        (17ull * (value >> 4) + 3ull * (value & 15u) + 176 + correction) %
        576);
    state = 0;
    for (int byte = 0; byte < 4; ++byte) {
      state |= static_cast<std::uint32_t>(
                   source[(position + static_cast<std::size_t>(byte)) % 576])
               << (8 * byte);
    }
  }
  return tables;
}

std::uint8_t linear_byte(std::uint8_t value, const std::uint8_t basis[8],
                         std::uint8_t constant = 0) {
  std::uint8_t result = constant;
  for (int bit = 0; bit < 8; ++bit) {
    if ((value & (1u << bit)) != 0) result = static_cast<std::uint8_t>(result ^ basis[bit]);
  }
  return result;
}

int operand_key(const std::uint8_t* source, int row, int position, int length,
                int base, int column) {
  std::uint8_t key = 0;
  for (int byte = 0; byte < length; ++byte) {
    key = static_cast<std::uint8_t>(
        key ^ source[base + position + byte] ^
        kPadColumns[column][(position + byte) % 16] ^ kPadRows[column][row]);
  }
  return key;
}

std::array<std::uint8_t, 576> make_material(const std::uint8_t* source,
                                            const SourceSchedule& schedule) {
  std::array<std::uint8_t, 576> material{};
  // Sixty-four bytes of the original layout had no writer and no consumer;
  // the address-independent form normalizes that padding to zero, which the
  // copy table below reflects.
  static constexpr int kCopies[14][2] = {
      {0x20, 0xf0}, {0x70, 0xe0}, {0x120, 0xd0}, {0x130, 0x220},
      {0x140, 0x110}, {0x150, 0x100}, {0x160, 0x80}, {0x170, 0x120},
      {0x180, 0x20}, {0x190, 0x90}, {0x1a0, 0x190}, {0x1b0, 0x230},
      {0x1e0, 0xc0}, {0x1f0, 0x30}};
  for (const auto& copy : kCopies) {
    std::memcpy(material.data() + copy[0], source + copy[1], 16);
  }
  static constexpr int kDescriptions[9][3] = {
      {1, 0xa0, 0x200}, {2, 0x200, 0x1c0}, {3, 0x60, 0xc0},
      {4, 0, 0},         {5, 0x1c0, 0x50},  {6, 0x130, 0x80},
      {7, 0x1e0, 0xa0},  {8, 0x170, 0x30},  {9, 0x150, 0x100}};
  static constexpr int kOffsets[16] = {2, 12, 6, 16, 0, 8, 22, 28,
                                       4, 26, 18, 10, 20, 30, 24, 14};
  int walk = 0;
  for (const auto& description : kDescriptions) {
    const int column = description[0];
    const int base = description[1];
    const int destination = description[2];
    int position = 0;
    for (int row = 0; row < 16; ++row) {
      const int length = schedule.lengths[row][column];
      const int key =
          operand_key(source, row, position, length, base, column);
      walk = (walk + base + position) % 576;
      int first = source[walk];
      walk = (walk + base + position + 1) % 576;
      first = (first + source[walk]) & 0xff;
      const int at = destination + kOffsets[row];
      material[at] = static_cast<std::uint8_t>(first);
      material[at + 1] = static_cast<std::uint8_t>((key - first) & 0xff);
      position += length;
    }
  }
  return material;
}

Result<PreparedSource> prepare_source(const Bytes& source) {
  if (source.size() != 576) {
    return make_error(ErrorCode::kInvalidArgument,
                      "SAP cipher requires 576 source bytes");
  }
  const std::uint8_t* bytes = source.data();
  const SourceSchedule schedule = make_schedule(bytes);
  auto tables = make_source_tables(bytes);
  if (!tables) return tables.error();

  PreparedSource prepared;
  prepared.material = make_material(bytes, schedule);
  prepared.selectors = schedule.selectors;
  prepared.projection_mask = ~tables.value().mask;

  static constexpr std::uint8_t kIndex0[8] = {0x3c, 0x44, 0x73, 0xcc,
                                              0x50, 0xba, 0x9e, 0xee};
  static constexpr std::uint8_t kIndex1[8] = {0x9f, 0x72, 0xe1, 0x3b,
                                              0xdd, 0x31, 0x24, 0x8f};
  static constexpr std::uint8_t kMap2[8] = {0xe3, 0x34, 0xda, 0x73,
                                            0xb4, 0xa0, 0x4f, 0x2b};
  static constexpr std::uint8_t kMap3[8] = {0x1f, 0x35, 0x3b, 0xf7,
                                            0xd9, 0xad, 0xd4, 0x93};
  for (int index = 0; index < 256; ++index) {
    const std::uint32_t word = tables.value().words[index];
    prepared.auxiliary0[linear_byte(static_cast<std::uint8_t>(index), kIndex0)] =
        word ^ 0xd90b5d96u;
    prepared.auxiliary1[linear_byte(static_cast<std::uint8_t>(index), kIndex1)] =
        word ^ 0xaa8095eeu;
  }
  prepared.cache2.resize(tables.value().cache.size());
  prepared.cache3.resize(tables.value().cache.size());
  for (std::size_t index = 0; index < tables.value().cache.size(); ++index) {
    const std::uint8_t value = tables.value().cache[index];
    prepared.cache2[index] = linear_byte(value, kMap2, 0xc0);
    prepared.cache3[index] = static_cast<std::uint8_t>(
        linear_byte(value, kMap3, 0x47) ^
        static_cast<std::uint8_t>((index >> 2) & 0xff));
  }

  struct CacheDescription {
    int column;
    int base;
    const int* order;
    std::uint32_t bias;
    std::array<std::uint32_t, 16>* target;
  };
  const CacheDescription descriptions[2] = {
      {10, 0x1a0, kInitialOrder, tables.value().seeds[8] ^ 0xe62818fcu,
       &prepared.initial_cache},
      {0, 0x40, kTerminalOrder, 0xaf615514u, &prepared.terminal_cache}};
  for (const CacheDescription& description : descriptions) {
    int position = 0;
    for (int row = 0; row < 16; ++row) {
      const int slot = description.order[row];
      const int length = schedule.lengths[row][description.column];
      if (length != 1) {
        const int key = operand_key(bytes, row, position, length,
                                    description.base, description.column);
        (*description.target)[slot] =
            tables.value().words[key] ^ description.bias;
      }
      position += length;
    }
  }
  return prepared;
}

// ---------------------------------------------------------------------------
// Session parameters derived from the material.
// ---------------------------------------------------------------------------

struct CipherParameters {
  std::array<std::uint32_t, 16> initial_coefficients{};
  std::array<std::uint32_t, 16> terminal_coefficients{};
  std::array<std::uint8_t, 16> initial_field{};
  std::array<std::uint8_t, 7> terminal_seeds{};
  std::array<std::array<std::uint32_t, 4>, 9> raw_words{};
  std::array<std::array<std::uint8_t, 16>, 9> round_keys{};
};

std::uint32_t pair_word(const std::array<std::uint8_t, 576>& material,
                        int source, int mask) {
  std::uint32_t word = 0;
  for (int byte = 0; byte < 4; ++byte) {
    word |= static_cast<std::uint32_t>(
                material[source + byte] ^
                material[0x120 + (mask + byte) % 16])
            << (8 * byte);
  }
  return word;
}

CipherParameters make_parameters(const PreparedSource& prepared) {
  const auto& material = prepared.material;
  CipherParameters parameters;
  parameters.initial_coefficients = prepared.initial_cache;
  parameters.terminal_coefficients = prepared.terminal_cache;
  for (int row = 0; row < 16; ++row) {
    const int slot = kInitialOrder[row];
    if (parameters.initial_coefficients[slot] == 0) {
      parameters.initial_coefficients[slot] =
          pair_word(material, 0x180 + 4 * row, 4 * (row % 4)) ^ 0xe62818fcu;
    }
  }
  for (int row = 0; row < 16; ++row) {
    const int slot = kTerminalOrder[row];
    if (parameters.terminal_coefficients[slot] != 0) continue;
    int source = 0;
    int mask = 0;
    if (row < 5) {
      source = 0x130 + 4 * row;
      mask = 4 * row;
    } else if (row < 13) {
      source = 0x134 + 4 * row;
      mask = 4 * row + 4;
    } else {
      source = 0x138 + 4 * row;
      mask = 4 * row + 8;
    }
    parameters.terminal_coefficients[slot] =
        pair_word(material, source, mask) ^ 0xaf615514u;
  }
  for (int index = 0; index < 16; ++index) {
    const int left = (3 * index + 7) % 16;
    const int right = (11 * index + 2) % 16;
    const int a = material[0x1e0 + left] ^ material[0x70 + left];
    const int b = material[0x20 + right] ^ material[0x70 + right];
    parameters.initial_field[index] =
        static_cast<std::uint8_t>(((a - b) & 0xff) ^ kMaskByte);
  }
  parameters.terminal_seeds = {
      static_cast<std::uint8_t>(material[0x1f2] ^ material[0x72]),
      static_cast<std::uint8_t>(material[0x1f7] ^ material[0x1e7]),
      std::uint8_t{0x24},
      static_cast<std::uint8_t>(material[0x1eb] ^ material[0x2b]),
      static_cast<std::uint8_t>(material[0x75] ^ material[0x1f5]),
      std::uint8_t{0x11},
      static_cast<std::uint8_t>(material[0x23] ^ material[0x73])};
  for (int row = 0; row < 9; ++row) {
    std::uint32_t physical[4];
    for (int column = 0; column < 4; ++column) {
      const int index = 4 * row + 36 * column;
      physical[column] =
          pair_word(material, 0x130 + index, index) + 0x2186d6ffu;
    }
    parameters.raw_words[row] = {physical[1], physical[3], physical[0],
                                 physical[2]};
  }
  std::reverse(parameters.raw_words.begin(), parameters.raw_words.end());
  static constexpr int kOffsets[9] = {0x100, 0x30, 0xa0, 0x80, 0x50,
                                      0,     0xc0, 0x1c0, 0x200};
  static constexpr int kPairs[16][2] = {
      {0, 1},   {8, 9},   {22, 23}, {28, 29}, {20, 21}, {30, 31},
      {24, 25}, {14, 15}, {2, 3},   {12, 13}, {6, 7},   {16, 17},
      {4, 5},   {26, 27}, {18, 19}, {10, 11}};
  for (int round = 0; round < 9; ++round) {
    for (int lane = 0; lane < 16; ++lane) {
      parameters.round_keys[round][lane] = static_cast<std::uint8_t>(
          (material[kOffsets[round] + kPairs[lane][0]] +
           material[kOffsets[round] + kPairs[lane][1]]) &
          0xff);
    }
  }
  return parameters;
}

// ---------------------------------------------------------------------------
// Initial encoder: sixteen finite byte maps projecting the source tables.
// ---------------------------------------------------------------------------

struct InitialEncoder {
  std::array<std::array<std::uint8_t, 256>, 16> tables{};

  std::array<std::uint8_t, 17> encode_masked(
      const std::array<std::uint8_t, 16>& block) const {
    static constexpr int kDestinations[16] = {4, 5, 1, 13, 15, 0, 9, 8,
                                              16, -1, 12, 10, 11, 3, 2, 14};
    std::array<std::uint8_t, 17> output{};
    for (int source = 0; source < 16; ++source) {
      const int destination = kDestinations[source];
      if (destination >= 0) {
        output[destination] = tables[source][block[source]];
      }
    }
    output[6] = static_cast<std::uint8_t>(output[4] ^ output[14] ^ output[15]);
    output[7] = static_cast<std::uint8_t>(output[6] ^ tables[9][block[9]]);
    return output;
  }
};

// ---------------------------------------------------------------------------
// Terminal transform: 17 state bytes back to a sixteen-byte block.
// ---------------------------------------------------------------------------

struct Terminal {
  std::array<std::array<std::uint32_t, 256>, 16> word_tables{};
  std::array<std::uint32_t, 256> auxiliary{};
  Bytes cache;
  std::array<std::uint32_t, 16> lane_keys{};
  std::uint32_t projection_mask = 0;
  std::array<std::uint8_t, 16> output_seeds{};

  std::array<std::uint8_t, 16> decode(
      const std::array<std::uint8_t, 17>& state) const {
    static constexpr int kSources[16] = {4, 10, -1, 7, 11, 2, 0, 1,
                                         16, 9, 3, 6, 12, 5, 15, 14};
    static constexpr int kTableOrder[16] = {13, 5, 10, 2, 14, 1, 11, 8,
                                            6, 9, 0, 15, 4, 7, 3, 12};
    static constexpr int kDestinations[16] = {13, 5, 10, 2, 14, 1, 11, 8,
                                              6, 9, 0, 15, 4, 7, 3, 12};
    const int coupled = state[8] ^ state[13];
    const int left = (((state[12] ^ 0x31) + 0x6a) ^ kMaskByte) - 0x80;
    int middle = ((state[14] ^ 0xd2) + 0x4c + left) & 0xff;
    middle = (((middle ^ state[13]) + 1) ^ 0x7e) & 0xff;
    const int mixing = (middle + (state[15] ^ kMaskByte)) & 0xff;

    std::array<std::uint8_t, 16> output = output_seeds;
    for (int lane = 0; lane < 16; ++lane) {
      const int position = kSources[lane];
      const int index =
          position < 0 ? coupled : (state[position] ^ kMaskByte);
      std::uint32_t word = word_tables[kTableOrder[lane]][index];
      if (position < 0) {
        word ^= auxiliary[coupled ^ mixing] ^ auxiliary[mixing] ^
                auxiliary[coupled] ^ auxiliary[0];
      }
      word ^= lane_keys[lane];
      const std::size_t cache_index =
          (word & projection_mask) % cache.size();
      output[kDestinations[lane]] = static_cast<std::uint8_t>(
          output[kDestinations[lane]] ^ cache[cache_index]);
    }
    return output;
  }
};

std::array<std::uint8_t, 16> updated_field(
    const std::array<std::uint8_t, 16>& field,
    const std::array<std::uint32_t, 4>& raw_words) {
  const std::uint32_t words[4] = {raw_words[2] + 0xde792901u,
                                  raw_words[0] + 0xde792901u,
                                  raw_words[3] + 0xde792901u,
                                  raw_words[1] + 0xde792901u};
  static constexpr std::uint32_t kPrimes[4] = {233, 239, 241, 251};
  static constexpr std::uint8_t kConstants[4] = {0xf6, 0x19, 0x50, 0x8e};
  std::array<std::uint8_t, 16> result{};
  for (int position = 0; position < 16; ++position) {
    const int table = position % 4;
    std::size_t length = 0;
    const std::uint8_t* mask = carry_mask(table, &length);
    const std::size_t index = words[position / 4] % kPrimes[table];
    result[position] = static_cast<std::uint8_t>(field[position] ^ mask[index] ^
                                                  kConstants[table]);
  }
  return result;
}

std::uint8_t carry_value(const std::array<std::uint8_t, 4>& frame,
                         const std::array<std::uint8_t, 16>& block, int offset,
                         int first_mask) {
  int value = ((frame[0] ^ kMaskByte) + (block[offset] ^ first_mask) -
               (frame[1] ^ kMaskByte)) &
              0xff;
  value ^= block[offset + 1] ^ kMaskByte;
  value = (value + (frame[2] ^ kMaskByte) - (block[offset + 2] ^ kMaskByte)) &
          0xff;
  value ^= frame[3] ^ kMaskByte;
  return static_cast<std::uint8_t>(
      (value + (block[offset + 3] ^ kMaskByte)) & 0xff);
}

std::array<std::uint8_t, 17> nonlinear_round(
    const std::array<std::uint8_t, 17>& state,
    const std::array<int, 16>& tables, std::uint8_t carry) {
  static constexpr int kDestinations[14] = {0, 2, 3, 4, 5, 8, 9,
                                            10, 11, 12, 13, 14, 15, 16};
  static constexpr int kTableOrder[14] = {9, 6, 1, 0, 5, 3, 14,
                                          7, 12, 2, 15, 11, 4, 8};
  static constexpr int kSources[14] = {9, 16, 2, 3, 10, 15, 11,
                                       5, 14, 7, 6, 0, 12, 1};
  std::array<std::uint8_t, 17> output{};
  for (int lane = 0; lane < 14; ++lane) {
    output[kDestinations[lane]] =
        byte_table(tables[kTableOrder[lane]])
            [state[kSources[lane]] ^ kMaskByte];
  }
  output[1] = byte_table(tables[10])[state[8] ^ state[13]];
  output[6] =
      static_cast<std::uint8_t>(carry ^ output[3] ^ output[4] ^ output[5]);
  output[7] = static_cast<std::uint8_t>(
      output[6] ^ byte_table(tables[13])[state[4] ^ kMaskByte]);
  return output;
}

// ---------------------------------------------------------------------------
// F2: two-input, ten-round substitution/permutation function.
// ---------------------------------------------------------------------------

Result<std::array<std::uint8_t, 16>> f2_head(
    const std::array<std::uint8_t, 16>& input1,
    const std::array<std::uint8_t, 16>& input2) {
  constexpr std::size_t kKeySubstitutionOffset = 4096;
  constexpr std::size_t kEntryOffset = 5120;
  constexpr std::size_t kFinalOffset = 9216;
  constexpr std::size_t kMainOffset = 13312;
  constexpr std::size_t kLinearOffset = 17408;
  const std::uint8_t* data = kF2Tables;
  using sap_tables::kF2EntryPositions;
  using sap_tables::kF2Expansion;
  using sap_tables::kF2FinalPositions;
  using sap_tables::kF2KeySelections;
  using sap_tables::kF2MainRouting;

  std::array<std::uint8_t, 16> encoded{};
  for (int position = 0; position < 16; ++position) {
    encoded[position] = data[256 * position + input1[position]];
  }
  std::array<std::uint32_t, 14> words{};
  for (int position = 0; position < 4; ++position) {
    const int offset = 4 * position;
    words[position] =
        (static_cast<std::uint32_t>(encoded[offset]) << 24) |
        (static_cast<std::uint32_t>(encoded[offset + 1]) << 16) |
        (static_cast<std::uint32_t>(encoded[offset + 2]) << 8) |
        static_cast<std::uint32_t>(encoded[offset + 3]);
  }
  int word_count = 4;
  for (const auto& step : kF2Expansion) {
    std::uint32_t word = 0;
    for (int index = 0; index < step.selection_count; ++index) {
      word ^= words[step.selection[index]];
    }
    word = ((word << 8) | (word >> 24)) ^ step.whitening;
    std::uint32_t substituted = 0;
    for (int position = 0; position < 4; ++position) {
      const int shift = 24 - 8 * position;
      const std::uint8_t byte =
          static_cast<std::uint8_t>((word >> shift) & 0xff);
      substituted |=
          static_cast<std::uint32_t>(
              data[kKeySubstitutionOffset + 256 * position + byte])
          << shift;
    }
    words[word_count++] = substituted;
  }

  std::array<std::array<std::uint8_t, 16>, 11> additions{};
  for (int row = 0; row < 11; ++row) {
    for (int lane = 0; lane < 16; ++lane) {
      const auto& selection = kF2KeySelections[row][lane];
      std::uint32_t word = 0;
      for (int index = 0; index < selection.word_count; ++index) {
        word ^= words[selection.words[index]];
      }
      std::uint8_t byte =
          static_cast<std::uint8_t>((word >> selection.shift) & 0xff);
      if (selection.linear >= 0) {
        byte = data[kLinearOffset +
                    256 * static_cast<std::size_t>(selection.linear) + byte];
      }
      additions[row][lane] =
          static_cast<std::uint8_t>(byte ^ selection.whitening);
    }
  }

  std::array<std::uint8_t, 16> state = additions[0];
  for (int position = 0; position < 16; ++position) {
    const std::uint8_t byte =
        data[kEntryOffset + 256 * position + input2[position]];
    state[kF2EntryPositions[position]] =
        static_cast<std::uint8_t>(state[kF2EntryPositions[position]] ^ byte);
  }
  for (int round = 0; round < 9; ++round) {
    std::array<std::uint8_t, 16> output = additions[round + 1];
    for (int position = 0; position < 16; ++position) {
      const auto& routing = kF2MainRouting[round][position];
      const std::size_t offset =
          kMainOffset + 4 * (256 * routing.table + state[position]);
      const std::uint32_t word =
          static_cast<std::uint32_t>(data[offset]) |
          (static_cast<std::uint32_t>(data[offset + 1]) << 8) |
          (static_cast<std::uint32_t>(data[offset + 2]) << 16) |
          (static_cast<std::uint32_t>(data[offset + 3]) << 24);
      for (int byte_position = 0; byte_position < 4; ++byte_position) {
        const std::uint8_t byte = static_cast<std::uint8_t>(
            (word >> (24 - 8 * byte_position)) & 0xff);
        const int destination = routing.destinations[byte_position];
        output[destination] =
            static_cast<std::uint8_t>(output[destination] ^ byte);
      }
    }
    state = output;
  }
  std::array<std::uint8_t, 16> output = additions[10];
  for (int position = 0; position < 16; ++position) {
    const std::uint8_t byte =
        data[kFinalOffset + 256 * position + state[position]];
    output[kF2FinalPositions[position]] = static_cast<std::uint8_t>(
        output[kF2FinalPositions[position]] ^ byte);
  }
  for (std::uint8_t& byte : output) {
    byte = static_cast<std::uint8_t>(byte ^ kMaskByte);
  }
  return output;
}

// ---------------------------------------------------------------------------
// Minimal X.509 DER reader for the setup certificate container. The
// container embeds a chain; the certificate whose encoding ends last (and
// is at least 512 bytes) supplies the RSA-2048 setup key.
// ---------------------------------------------------------------------------

struct DerView {
  std::uint8_t tag = 0;
  const std::uint8_t* content = nullptr;
  std::size_t content_size = 0;
  std::size_t total_size = 0;
};

bool read_der(const std::uint8_t* data, std::size_t size, DerView& view) {
  if (size < 2) return false;
  view.tag = data[0];
  std::size_t offset = 1;
  std::size_t length = 0;
  const std::uint8_t first = data[offset++];
  if ((first & 0x80) == 0) {
    length = first;
  } else {
    const int count = first & 0x7f;
    if (count == 0 || count > 4 || offset + static_cast<std::size_t>(count) > size) {
      return false;
    }
    if (data[offset] == 0) return false;  // DER lengths are minimal.
    for (int index = 0; index < count; ++index) {
      length = (length << 8) | data[offset++];
    }
    if (length < 128) return false;
  }
  if (length > size - offset) return false;
  view.content = data + offset;
  view.content_size = length;
  view.total_size = offset + length;
  return true;
}

bool expect_der(const std::uint8_t*& data, std::size_t& size, std::uint8_t tag,
                DerView& view) {
  if (!read_der(data, size, view) || view.tag != tag) return false;
  data += view.total_size;
  size -= view.total_size;
  return true;
}

Bytes integer_magnitude(const DerView& view) {
  Bytes magnitude(view.content, view.content + view.content_size);
  std::size_t start = 0;
  while (start + 1 < magnitude.size() && magnitude[start] == 0) ++start;
  magnitude.erase(magnitude.begin(),
                  magnitude.begin() + static_cast<std::ptrdiff_t>(start));
  return magnitude;
}

struct CertificateKey {
  Bytes serial;   // Wire form: DER integer content (sign byte retained).
  Bytes modulus;  // 256-byte big-endian magnitude.
  Bytes exponent;
  std::size_t end = 0;
};

bool parse_certificate(const std::uint8_t* data, std::size_t size,
                       CertificateKey& key) {
  DerView certificate;
  if (!read_der(data, size, certificate) || certificate.tag != 0x30) {
    return false;
  }
  const std::uint8_t* cursor = certificate.content;
  std::size_t remaining = certificate.content_size;
  DerView tbs;
  if (!expect_der(cursor, remaining, 0x30, tbs)) return false;
  const std::uint8_t* field = tbs.content;
  std::size_t field_size = tbs.content_size;
  DerView item;
  if (field_size > 0 && field[0] == 0xa0) {  // Explicit version [0].
    if (!expect_der(field, field_size, 0xa0, item)) return false;
  }
  DerView serial;
  if (!expect_der(field, field_size, 0x02, serial)) return false;
  if (serial.content_size == 0 || serial.content_size > 127) return false;
  // The wire form retains DER's positive-integer sign byte.
  key.serial.assign(serial.content, serial.content + serial.content_size);
  for (int skipped = 0; skipped < 4; ++skipped) {  // signature, issuer,
    if (!read_der(field, field_size, item)) return false;  // validity,
    field += item.total_size;                                // subject
    field_size -= item.total_size;
  }
  DerView spki;
  if (!expect_der(field, field_size, 0x30, spki)) return false;
  const std::uint8_t* spki_cursor = spki.content;
  std::size_t spki_size = spki.content_size;
  DerView algorithm;
  if (!expect_der(spki_cursor, spki_size, 0x30, algorithm)) return false;
  DerView oid;
  const std::uint8_t* oid_cursor = algorithm.content;
  std::size_t oid_size = algorithm.content_size;
  if (!expect_der(oid_cursor, oid_size, 0x06, oid)) return false;
  static constexpr std::uint8_t kRsaOid[9] = {0x2a, 0x86, 0x48, 0x86, 0xf7,
                                              0x0d, 0x01, 0x01, 0x01};
  if (oid.content_size != sizeof(kRsaOid) ||
      std::memcmp(oid.content, kRsaOid, sizeof(kRsaOid)) != 0) {
    return false;
  }
  DerView bit_string;
  if (!expect_der(spki_cursor, spki_size, 0x03, bit_string)) return false;
  if (bit_string.content_size == 0 || bit_string.content[0] != 0) return false;
  DerView rsa_key;
  if (!read_der(bit_string.content + 1, bit_string.content_size - 1,
                rsa_key) ||
      rsa_key.tag != 0x30 ||
      rsa_key.total_size != bit_string.content_size - 1) {
    return false;
  }
  const std::uint8_t* key_cursor = rsa_key.content;
  std::size_t key_size = rsa_key.content_size;
  DerView modulus;
  DerView exponent;
  if (!expect_der(key_cursor, key_size, 0x02, modulus)) return false;
  if (!expect_der(key_cursor, key_size, 0x02, exponent)) return false;
  Bytes modulus_magnitude = integer_magnitude(modulus);
  Bytes exponent_magnitude = integer_magnitude(exponent);
  if (modulus_magnitude.size() != 256 ||
      (modulus_magnitude[0] & 0x80) == 0) {
    return false;  // Not an RSA-2048 modulus.
  }
  if (exponent_magnitude.empty() ||
      (exponent_magnitude.back() & 1) == 0 ||
      (exponent_magnitude.size() == 1 && exponent_magnitude[0] < 3)) {
    return false;
  }
  key.modulus = std::move(modulus_magnitude);
  key.exponent = std::move(exponent_magnitude);
  key.end = certificate.total_size;
  return true;
}

Result<CertificateKey> select_certificate(const Bytes& container) {
  if (container.size() < 512 || container.size() > (1u << 20)) {
    return make_error(ErrorCode::kParse,
                      "invalid SAP certificate container length");
  }
  std::optional<CertificateKey> best;
  for (std::size_t offset = 0; offset + 4 < container.size(); ++offset) {
    if (container[offset] != 0x30) continue;
    CertificateKey candidate;
    if (!parse_certificate(container.data() + offset,
                           container.size() - offset, candidate)) {
      continue;
    }
    const std::size_t end = offset + candidate.end;
    if (candidate.end >= 512 && (!best || end > best->end)) {
      candidate.end = end;
      best = std::move(candidate);
    }
  }
  if (!best) {
    return make_error(ErrorCode::kParse,
                      "SAP certificate container has no usable certificate");
  }
  return std::move(best.value());
}

bool valid_session(const SapSessionData& session) {
  return session.source.size() == 576 && session.f2_tail.size() <= 16 &&
         session.f3.size() <= 16u * 1024 * 1024;
}

}  // namespace

// ---------------------------------------------------------------------------
// Cipher
// ---------------------------------------------------------------------------

struct SapSoftwareCipher::Impl {
  CipherParameters parameters;
  InitialEncoder initial;
  Terminal terminal;
  std::array<std::array<std::uint8_t, 4>, 3> carry_frames{};
  // Static byte-table indices per nonlinear stage.
  std::array<std::array<int, 16>, 8> nonlinear_tables{};

  std::array<std::uint8_t, 17> mix(const std::array<std::uint8_t, 17>& state,
                                   const std::array<std::uint8_t, 17>& previous,
                                   int stage, std::uint8_t carry) const {
    static constexpr int kIndexSources[16] = {15, 5, 2, 10, 11, 7, 9, 13,
                                              4, 3, 12, 8, 16, 0, 1, 14};
    static constexpr int kFirstWordTables[16] = {28, 39, 50, 61, 28, 39,
                                                 50, 61, 56, 67, 6, 17,
                                                 56, 67, 6, 17};
    static constexpr int kOutputColumns[17] = {1, 3, 1, 2, 3, 3, 2, 3, -1,
                                               0, 2, 0, 0, 2, 1, 0, 1};
    static constexpr int kOutputShifts[17] = {24, 0, 8, 0, 8, 24, 24, 16, 0,
                                              8, 8, 16, 0, 16, 0, 24, 16};
    static constexpr std::uint8_t kOutputXors[17] = {
        0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0x2d, 0,
        0xa0, 0x2d, 0x75, 0x32, 0, 0x2d, 0xbb, 0x2d};
    const auto& key = parameters.round_keys[stage];
    std::array<std::uint32_t, 4> accumulators = parameters.raw_words[stage];
    for (std::uint32_t& accumulator : accumulators) {
      accumulator += 0xde792901u;
    }
    accumulators[0] ^= 0x96588d1fu;
    for (int lane = 0; lane < 16; ++lane) {
      std::uint8_t index =
          static_cast<std::uint8_t>(state[kIndexSources[lane]] ^ key[lane]);
      if (lane == 5) index = static_cast<std::uint8_t>(index ^ state[6]);
      const int table =
          (kFirstWordTables[lane] - 16 * stage + 144) % 72;
      accumulators[lane / 4] ^= static_word(table, index);
    }
    const std::uint8_t byte11 = static_cast<std::uint8_t>(
        static_cast<std::uint8_t>((accumulators[0] >> 16) & 0xff) ^ 0x75);
    const std::uint8_t byte8 = static_cast<std::uint8_t>(
        previous[16] ^ previous[8] ^ byte11 ^ state[9] ^ key[6] ^ carry);
    std::array<std::uint8_t, 17> output{};
    // The old target is copied from previous before each mix, cancelling the
    // old XOR previous term at every ordinary output position.
    for (int position = 0; position < 17; ++position) {
      if (position == 8) continue;
      const std::uint32_t word = accumulators[kOutputColumns[position]];
      output[position] = static_cast<std::uint8_t>(
          static_cast<std::uint8_t>((word >> kOutputShifts[position]) & 0xff) ^
          kOutputXors[position]);
    }
    output[8] = byte8;
    output[13] = static_cast<std::uint8_t>(output[13] ^ byte8);
    return output;
  }
};

SapSoftwareCipher::SapSoftwareCipher(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
SapSoftwareCipher::SapSoftwareCipher(SapSoftwareCipher&&) noexcept = default;
SapSoftwareCipher& SapSoftwareCipher::operator=(
    SapSoftwareCipher&&) noexcept = default;
SapSoftwareCipher::~SapSoftwareCipher() = default;

Result<std::unique_ptr<SapSoftwareCipher>> SapSoftwareCipher::create(
    const Bytes& source) {
  auto prepared = prepare_source(source);
  if (!prepared) return prepared.error();
  auto impl = std::make_unique<Impl>();
  impl->parameters = make_parameters(prepared.value());
  const auto& material = prepared.value().material;

  // Session word tables and initial-encoder caches.
  std::array<std::array<std::uint32_t, 256>, 16> session_word_tables{};
  std::array<Bytes, 16> caches;
  for (int slot = 0; slot < 16; ++slot) {
    const int a = material[0x1f0 + slot] ^ material[0x70 + slot];
    const int b = material[0x20 + slot] ^ material[0x70 + slot];
    const int word_mask = (a + b) & 0xff;
    const std::uint8_t* table = byte_table(40 + (79 * slot) % 144);
    for (int index = 0; index < 256; ++index) {
      session_word_tables[slot][index] =
          prepared.value().auxiliary0[table[index] ^ word_mask];
    }
    const int index = (13 * slot) % 16;
    const int left = material[0x1e0 + index] ^ material[0x70 + index];
    const int right = material[0x20 + index] ^ material[0x70 + index];
    const int source_mask = ((left - right) & 0xff) ^ 0xfe;
    const std::uint8_t* cache_table = byte_table(184 + (9 * slot) % 16);
    caches[slot].resize(prepared.value().cache2.size());
    for (std::size_t i = 0; i < caches[slot].size(); ++i) {
      caches[slot][i] = cache_table[prepared.value().cache2[i] ^ source_mask];
    }
  }

  const std::size_t cache_size = caches[0].size();
  if (cache_size == 0) {
    return make_error(ErrorCode::kParse, "SAP initial cache is empty");
  }
  for (int source_index = 0; source_index < 16; ++source_index) {
    const int coefficient = kInitialOrder[source_index];
    const int cache = (5 * source_index) % 16;
    for (int byte = 0; byte < 256; ++byte) {
      const std::uint32_t word =
          prepared.value().auxiliary1[byte ^ kMaskByte] ^
          impl->parameters.initial_coefficients[coefficient];
      const std::size_t index =
          ((word ^ 0x4ca88d12u) & prepared.value().projection_mask) %
          cache_size;
      impl->initial.tables[source_index][byte] = caches[cache][index];
    }
  }

  const std::array<std::uint8_t, 16> field = updated_field(
      impl->parameters.initial_field, impl->parameters.raw_words[3]);
  impl->carry_frames = {
      std::array<std::uint8_t, 4>{impl->parameters.initial_field[0],
                                  impl->parameters.initial_field[2],
                                  impl->parameters.initial_field[11],
                                  impl->parameters.initial_field[12]},
      std::array<std::uint8_t, 4>{field[6], field[1], field[3], field[14]},
      std::array<std::uint8_t, 4>{field[8], field[9], field[13], field[15]}};

  Terminal terminal;
  terminal.word_tables = session_word_tables;
  terminal.auxiliary = prepared.value().auxiliary0;
  terminal.cache.resize(prepared.value().cache3.size());
  for (std::size_t index = 0; index < terminal.cache.size(); ++index) {
    terminal.cache[index] = static_cast<std::uint8_t>(
        prepared.value().cache3[index] ^
        static_cast<std::uint8_t>((index >> 2) & 0xff) ^ 0xfa);
  }
  terminal.projection_mask = prepared.value().projection_mask;
  static constexpr int kCoefficientOrder[16] = {3, 1, 6, 8, 10, 4, 0, 12,
                                                7, 2, 13, 5, 14, 15, 9, 11};
  static constexpr int kDestinations[16] = {13, 5, 10, 2, 14, 1, 11, 8,
                                            6, 9, 0, 15, 4, 7, 3, 12};
  for (int lane = 0; lane < 16; ++lane) {
    const std::uint8_t field_byte =
        static_cast<std::uint8_t>(field[kDestinations[lane]] ^ kMaskByte);
    terminal.lane_keys[lane] =
        impl->parameters.terminal_coefficients[kCoefficientOrder[lane]] ^
        prepared.value().auxiliary0[field_byte] ^ 0xaf615514u;
    terminal.output_seeds[lane] =
        impl->parameters.terminal_seeds[prepared.value().selectors[lane]];
  }
  impl->terminal = std::move(terminal);

  static constexpr int kKeyOrder[16] = {12, 0, 1, 8, 5, 7, 9, 4,
                                        2, 14, 11, 13, 3, 10, 15, 6};
  static constexpr int kPositionOrder[16] = {4, 0, 15, 8, 11, 9, 7, 12,
                                             14, 2, 5, 3, 13, 6, 1, 10};
  static constexpr int kStarts[16] = {2, 4, 8, 3, 7, 2, 6, 1,
                                      5, 0, 4, 8, 3, 7, 2, 6};
  for (int stage = 0; stage < 8; ++stage) {
    for (int lane = 0; lane < 16; ++lane) {
      const int position = kPositionOrder[lane];
      const int round = (kStarts[position] + 2 * stage) % 9;
      impl->nonlinear_tables[stage][kKeyOrder[lane]] =
          40 + 16 * round + position;
    }
  }

  return std::unique_ptr<SapSoftwareCipher>(
      new SapSoftwareCipher(std::move(impl)));
}

std::array<std::uint8_t, 16> SapSoftwareCipher::encrypt(
    const std::array<std::uint8_t, 16>& block) const {
  static constexpr int kPreviousSlices[9] = {8, 0, 0, 1, 1, 1, 4, 2, 2};
  std::array<std::uint8_t, 16> masked{};
  for (int index = 0; index < 16; ++index) {
    masked[index] = static_cast<std::uint8_t>(block[index] ^ kMaskByte);
  }
  std::array<std::uint8_t, 17> state = impl_->initial.encode_masked(masked);
  const std::uint8_t carries[3] = {
      carry_value(impl_->carry_frames[0], masked, 0, 0x20),
      carry_value(impl_->carry_frames[1], masked, 4, 0x26),
      carry_value(impl_->carry_frames[2], masked, 8, 0x28)};
  std::array<std::uint8_t, 17> expanded{};
  for (int position = 0; position < 17; ++position) {
    const int source_position = (3 + 7 * position) % 16;
    const std::uint8_t unmasked =
        static_cast<std::uint8_t>(masked[source_position] ^ kMaskByte);
    expanded[position] = static_cast<std::uint8_t>(
        ((unmasked + position) & 0xff) ^ kMaskByte);
  }
  std::array<std::array<std::uint8_t, 17>, 9> slices{};
  slices[0] = expanded;
  slices[8] = expanded;
  for (int stage = 0; stage < 9; ++stage) {
    const std::uint8_t carry = carries[stage < 4 ? 0 : stage < 8 ? 1 : 2];
    const std::array<std::uint8_t, 17> mixed =
        impl_->mix(state, slices[kPreviousSlices[stage]], stage, carry);
    if (stage == 8) {
      std::array<std::uint8_t, 16> decoded = impl_->terminal.decode(mixed);
      for (std::uint8_t& byte : decoded) {
        byte = static_cast<std::uint8_t>(byte ^ kMaskByte);
      }
      return decoded;
    }
    slices[stage] = mixed;
    state = nonlinear_round(mixed, impl_->nonlinear_tables[stage], carry);
  }
  return {};  // Unreachable: stage eight always returns.
}

Result<std::array<std::uint8_t, 16>> sap_software_f2_head(
    const std::array<std::uint8_t, 16>& input1,
    const std::array<std::uint8_t, 16>& input2) {
  return f2_head(input1, input2);
}

// ---------------------------------------------------------------------------
// Signer
// ---------------------------------------------------------------------------

struct SapSoftwareSigner::Impl {
  std::unique_ptr<SapSoftwareCipher> cipher;
  SapSessionData session;
  std::uint32_t platform = 1;
  Bytes f2;
  SapRng random;
  std::optional<Bytes> blob;
  std::mutex mutex;
};

SapSoftwareSigner::SapSoftwareSigner(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
SapSoftwareSigner::SapSoftwareSigner(SapSoftwareSigner&&) noexcept = default;
SapSoftwareSigner& SapSoftwareSigner::operator=(
    SapSoftwareSigner&&) noexcept = default;
SapSoftwareSigner::~SapSoftwareSigner() = default;

Result<std::unique_ptr<SapSoftwareSigner>> SapSoftwareSigner::create(
    SapSessionData session, std::uint32_t platform) {
  if (!valid_session(session)) {
    return make_error(ErrorCode::kInvalidArgument,
                      "invalid SAP session field lengths");
  }
  auto cipher = SapSoftwareCipher::create(session.source);
  if (!cipher) return cipher.error();
  auto head = f2_head(session.f2_input1, session.f2_input2);
  if (!head) return head.error();
  auto impl = std::make_unique<Impl>();
  impl->cipher = std::move(cipher.value());
  impl->f2.assign(head.value().begin(), head.value().end());
  impl->f2.insert(impl->f2.end(), session.f2_tail.begin(),
                  session.f2_tail.end());
  impl->platform = platform;
  impl->random.words = session.random_words;
  impl->random.cursor = session.random_cursor;
  impl->session = std::move(session);
  return std::unique_ptr<SapSoftwareSigner>(
      new SapSoftwareSigner(std::move(impl)));
}

Result<Bytes> SapSoftwareSigner::sign(const Bytes& body) {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (!impl_->blob) {
    std::array<std::uint8_t, 78> material{};
    const auto address =
        static_cast<std::uint32_t>(impl_->session.f1_material_address);
    impl_->random.fill(impl_->session.seed30, address, f1_whiten(), 0x27,
                       material.data(), 60);
    impl_->random.fill(impl_->session.seed30, address + 0xaau, f1_whiten(),
                       0x27, material.data() + 60, 18);
    std::array<std::uint8_t, 256> f1{};
    f1_from_material(material.data(), f1.data());
    Bytes blob;
    blob.push_back(3);
    append_be32(blob, impl_->platform);
    append_be32(blob, 256);
    blob.insert(blob.end(), f1.begin(), f1.end());
    append_be32(blob, static_cast<std::uint32_t>(impl_->f2.size()));
    blob.insert(blob.end(), impl_->f2.begin(), impl_->f2.end());
    append_be32(blob,
                static_cast<std::uint32_t>(impl_->session.f3.size()));
    blob.insert(blob.end(), impl_->session.f3.begin(),
                impl_->session.f3.end());
    const std::size_t padding = (16 - blob.size() % 16) % 16;
    blob.insert(blob.end(), padding, 0);
    impl_->blob = std::move(blob);
  }
  const Bytes& blob = impl_->blob.value();
  std::array<std::uint8_t, 16> iv{};
  impl_->random.fill(
      impl_->session.seed54,
      static_cast<std::uint32_t>(impl_->session.iv_address), iv_whiten(), 0x12,
      iv.data(), iv.size());
  Sha1 digest;
  digest.update(blob.data(), blob.size());
  digest.update(body.data(), body.size());
  const std::array<std::uint8_t, 20> hash = digest.finish();
  std::array<std::uint8_t, 16> first_input{};
  for (int index = 0; index < 16; ++index) {
    first_input[index] = static_cast<std::uint8_t>(iv[index] ^ hash[index]);
  }
  const std::array<std::uint8_t, 16> first =
      impl_->cipher->encrypt(first_input);
  std::array<std::uint8_t, 16> tag_input{};
  for (int index = 0; index < 16; ++index) {
    const std::uint8_t tail = index < 4 ? hash[16 + index] : 0;
    tag_input[index] = static_cast<std::uint8_t>(first[index] ^ tail);
  }
  const std::array<std::uint8_t, 16> tag = impl_->cipher->encrypt(tag_input);
  Bytes signature;
  signature.reserve(1 + iv.size() + tag.size() + 4 + blob.size());
  signature.push_back(2);
  signature.insert(signature.end(), iv.begin(), iv.end());
  signature.insert(signature.end(), tag.begin(), tag.end());
  append_be32(signature, static_cast<std::uint32_t>(blob.size()));
  signature.insert(signature.end(), blob.begin(), blob.end());
  return signature;
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

struct SapSoftwareHandshake::Impl {
  Bytes request;
  std::array<std::uint8_t, 16> response_key{};
  std::array<std::uint8_t, 16> response_echo{};
  Bytes f2_material;
  SapRng random;
  std::uint32_t seed30 = 0;
  std::uint32_t seed54 = 0;
};

SapSoftwareHandshake::SapSoftwareHandshake(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}
SapSoftwareHandshake::SapSoftwareHandshake(SapSoftwareHandshake&&) noexcept =
    default;
SapSoftwareHandshake& SapSoftwareHandshake::operator=(
    SapSoftwareHandshake&&) noexcept = default;
SapSoftwareHandshake::~SapSoftwareHandshake() = default;

const Bytes& SapSoftwareHandshake::request() const { return impl_->request; }
std::array<std::uint8_t, 16> SapSoftwareHandshake::response_key() const {
  return impl_->response_key;
}
std::array<std::uint8_t, 16> SapSoftwareHandshake::response_echo() const {
  return impl_->response_echo;
}

Result<std::unique_ptr<SapSoftwareHandshake>>
SapSoftwareHandshake::create_deterministic(
    const Bytes& certificate_container,
    const std::array<std::uint32_t, 3>& entropy, std::int64_t seconds,
    std::uint32_t microseconds) {
  if (microseconds >= 1000000) {
    return make_error(ErrorCode::kInvalidArgument,
                      "invalid SAP setup entropy or time");
  }
  auto certificate = select_certificate(certificate_container);
  if (!certificate) return certificate.error();

  auto impl = std::make_unique<Impl>();
  std::array<std::uint32_t, 128> pool{};
  impl->random.initialize(entropy[0], entropy[1], entropy[2],
                          static_cast<std::uint32_t>(seconds), microseconds,
                          pool);
  impl->seed30 = pool[0x30 / 4];
  impl->seed54 = pool[0x54 / 4];
  const std::uint32_t seed3c = pool[0x3c / 4];

  const auto generate_setup = [&](std::size_t count, std::uint32_t address) {
    Bytes output(count);
    impl->random.fill(impl->seed54, address, iv_whiten(), 0x12, output.data(),
                      count);
    return output;
  };
  const auto generate_exchange = [&](std::size_t count,
                                     std::uint32_t address) {
    Bytes output(count);
    impl->random.fill(seed3c, address, exchange_whiten(), 0x4b, output.data(),
                      count);
    return output;
  };

  // These constants are arithmetic salts from the reconstructed layout.
  const Bytes initial = generate_setup(33, 0x7ffac0u);
  const std::size_t material_size = 16 + initial[32] % 17;
  impl->f2_material.resize(material_size);
  for (std::size_t index = 0; index < material_size; ++index) {
    impl->f2_material[index] =
        static_cast<std::uint8_t>(initial[index] ^ kMaskByte);
  }
  const Bytes response_key = generate_exchange(16, 0x7ffc14u);
  const Bytes response_echo = generate_exchange(16, 0x7ffc14u);
  const Bytes encryption_key = generate_exchange(16, 0x7ffc14u);
  const Bytes iv = generate_exchange(16, 0x7ffc14u);
  const Bytes authentication_key = generate_exchange(20, 0x7ffc24u);
  std::copy(response_key.begin(), response_key.end(),
            impl->response_key.begin());
  std::copy(response_echo.begin(), response_echo.end(),
            impl->response_echo.begin());

  Bytes padding;
  for (std::uint32_t address = 0x7ff813u; address-- > 0x7ff808u;) {
    padding.push_back(static_cast<std::uint8_t>(
        (((address ^ 0xbau) * (address + 0x11u)) & 0xffu) ^ kMaskByte));
  }
  Bytes plaintext;
  plaintext.push_back(2);
  append_be32(plaintext, 1);
  plaintext.insert(plaintext.end(), response_key.begin(), response_key.end());
  plaintext.insert(plaintext.end(), response_echo.begin(),
                   response_echo.end());
  plaintext.insert(plaintext.end(), padding.begin(), padding.end());

  const Bytes oaep_seed = generate_setup(20, 0x7ff4e4u);
  std::array<std::uint8_t, 20> seed{};
  std::copy(oaep_seed.begin(), oaep_seed.end(), seed.begin());
  Bytes key_material = encryption_key;
  key_material.insert(key_material.end(), authentication_key.begin(),
                      authentication_key.end());
  auto key_block = rsa_oaep_sha1_encrypt(
      key_material, seed, certificate.value().modulus,
      certificate.value().exponent);
  if (!key_block) return key_block.error();

  std::array<std::uint8_t, 16> aes_key{};
  std::array<std::uint8_t, 16> aes_iv{};
  std::copy(encryption_key.begin(), encryption_key.end(), aes_key.begin());
  std::copy(iv.begin(), iv.end(), aes_iv.begin());
  auto encrypted = aes128_cbc_encrypt(plaintext, aes_key, aes_iv);
  if (!encrypted) return encrypted.error();
  const std::array<std::uint8_t, 20> mac =
      hmac_sha1(authentication_key, encrypted.value().data(),
                encrypted.value().size());

  Bytes request;
  request.push_back(1);
  request.push_back(
      static_cast<std::uint8_t>(certificate.value().serial.size()));
  request.insert(request.end(), certificate.value().serial.begin(),
                 certificate.value().serial.end());
  request.insert(request.end(), key_block.value().begin(),
                 key_block.value().end());
  request.insert(request.end(), iv.begin(), iv.end());
  append_be32(request,
              static_cast<std::uint32_t>(encrypted.value().size()));
  request.insert(request.end(), encrypted.value().begin(),
                 encrypted.value().end());
  request.insert(request.end(), mac.begin(), mac.end());
  impl->request = std::move(request);
  return std::unique_ptr<SapSoftwareHandshake>(
      new SapSoftwareHandshake(std::move(impl)));
}

Result<std::unique_ptr<SapSoftwareHandshake>> SapSoftwareHandshake::create(
    const Bytes& certificate_container) {
  auto entropy = random_bytes(12);
  if (!entropy) return entropy.error();
  std::array<std::uint32_t, 3> words{};
  std::memcpy(words.data(), entropy.value().data(), 12);
  const auto now = std::chrono::system_clock::now();
  const auto since_epoch = now.time_since_epoch();
  const auto seconds =
      std::chrono::duration_cast<std::chrono::seconds>(since_epoch).count();
  const auto microseconds =
      std::chrono::duration_cast<std::chrono::microseconds>(since_epoch)
          .count() %
      1000000;
  const auto normalized_microseconds = static_cast<std::uint32_t>(
      microseconds < 0 ? microseconds + 1000000 : microseconds);
  return create_deterministic(certificate_container, words,
                              static_cast<std::int64_t>(seconds),
                              normalized_microseconds);
}

Result<SapSessionData> SapSoftwareHandshake::complete(const Bytes& reply) const {
  const Bytes& bytes = reply;
  if (bytes.size() < 21 || bytes.size() > (1u << 20) || bytes[0] != 2) {
    return make_error(ErrorCode::kParse, "invalid SAP setup response");
  }
  const auto length_at = [&](std::size_t offset, std::uint32_t& value) {
    if (offset + 4 > bytes.size()) return false;
    value = read_be32(bytes.data() + offset);
    return true;
  };
  std::uint32_t encrypted_length = 0;
  if (!length_at(17, encrypted_length) || encrypted_length == 0 ||
      encrypted_length % 16 != 0 ||
      encrypted_length > bytes.size() - 21 - 25) {
    return make_error(ErrorCode::kParse,
                      "invalid SAP setup ciphertext length");
  }
  const std::size_t record_offset = 21 + encrypted_length;
  std::uint32_t declared = 0;
  if (!length_at(record_offset + 21, declared)) {
    return make_error(ErrorCode::kParse, "truncated SAP setup length");
  }
  const std::uint32_t record_length = declared + 25u;
  if (record_length > 16u * 1024 * 1024 ||
      record_length > bytes.size() - record_offset) {
    return make_error(ErrorCode::kParse,
                      "invalid SAP setup signature record length");
  }
  std::array<std::uint8_t, 16> iv{};
  std::copy(bytes.begin() + 1, bytes.begin() + 17, iv.begin());
  const Bytes ciphertext(bytes.begin() + 21,
                         bytes.begin() + static_cast<std::ptrdiff_t>(record_offset));
  auto plaintext = aes128_cbc_decrypt(ciphertext, impl_->response_key, iv);
  if (!plaintext) return plaintext.error();

  static constexpr std::size_t kExpected[6] = {16, 16, 16, 576, 576, 16};
  std::array<Bytes, 6> fields;
  std::size_t offset = 0;
  for (int index = 0; index < 6; ++index) {
    if (offset + 4 > plaintext.value().size()) {
      return make_error(ErrorCode::kParse,
                        "truncated decrypted SAP setup field");
    }
    const std::uint32_t count =
        read_be32(plaintext.value().data() + offset);
    offset += 4;
    if (count != kExpected[index] ||
        count > plaintext.value().size() - offset) {
      return make_error(ErrorCode::kParse,
                        "invalid decrypted SAP setup field length");
    }
    fields[index].assign(
        plaintext.value().begin() + static_cast<std::ptrdiff_t>(offset),
        plaintext.value().begin() +
            static_cast<std::ptrdiff_t>(offset + count));
    offset += count;
  }
  std::uint8_t difference = 0;
  for (int index = 0; index < 16; ++index) {
    difference = static_cast<std::uint8_t>(
        difference | (fields[5][index] ^ impl_->response_echo[index]));
  }
  if (difference != 0) {
    return make_error(ErrorCode::kParse,
                      "SAP reply belongs to another exchange");
  }

  SapSessionData session;
  session.source = fields[4];
  for (int index = 0; index < 16; ++index) {
    session.f2_input1[index] =
        static_cast<std::uint8_t>((fields[2][index] + kMaskByte) & 0xff);
  }
  const std::size_t input2_size = std::min<std::size_t>(16, impl_->f2_material.size());
  std::copy(impl_->f2_material.begin(),
            impl_->f2_material.begin() +
                static_cast<std::ptrdiff_t>(input2_size),
            session.f2_input2.begin());
  for (std::size_t index = input2_size; index < impl_->f2_material.size();
       ++index) {
    session.f2_tail.push_back(
        static_cast<std::uint8_t>(impl_->f2_material[index] ^ kMaskByte));
  }
  session.f3.assign(bytes.begin() + static_cast<std::ptrdiff_t>(record_offset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(record_offset + record_length));
  session.random_words = impl_->random.words;
  session.random_cursor = impl_->random.cursor;
  session.seed30 = impl_->seed30;
  session.seed54 = impl_->seed54;
  session.f1_material_address =
      0x400000000604ULL + (impl_->f2_material.size() > 28 ? 16 : 0);
  if (!valid_session(session)) {
    return make_error(ErrorCode::kParse, "invalid SAP session from setup");
  }
  return session;
}

// ---------------------------------------------------------------------------
// SapBackend adapter
// ---------------------------------------------------------------------------

namespace {

class SoftwareSapBackend final : public SapBackend {
 public:
  Result<std::uint64_t> initialize(const Bytes& hardware_block) override {
    if (hardware_block.empty() || hardware_block.size() > 64) {
      return make_error(ErrorCode::kInvalidArgument,
                        "SAP hardware identity is invalid");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::uint64_t context = next_context_++;
    contexts_.emplace(context, Context{});
    return context;
  }

  Result<std::pair<Bytes, std::int32_t>> exchange(
      std::uint32_t version, const Bytes& hardware_block,
      std::uint64_t context, const Bytes& input) override {
    if (version != 200) {
      return make_error(ErrorCode::kUnsupported,
                        "unsupported SAP setup version");
    }
    if (hardware_block.empty()) {
      return make_error(ErrorCode::kInvalidArgument,
                        "SAP hardware identity is invalid");
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto found = contexts_.find(context);
    if (found == contexts_.end()) {
      return make_error(ErrorCode::kSignerUnavailable,
                        "SAP context is not initialized");
    }
    if (!found->second.handshake) {
      auto handshake = SapSoftwareHandshake::create(input);
      if (!handshake) return handshake.error();
      Bytes request = handshake.value()->request();
      found->second.handshake = std::move(handshake.value());
      return std::pair<Bytes, std::int32_t>{std::move(request), 1};
    }
    auto session = found->second.handshake->complete(input);
    if (!session) return session.error();
    auto signer =
        SapSoftwareSigner::create(std::move(session.value()));
    if (!signer) return signer.error();
    found->second.signer = std::move(signer.value());
    found->second.handshake.reset();
    return std::pair<Bytes, std::int32_t>{Bytes{}, 0};
  }

  Result<Bytes> sign(std::uint64_t context, const Bytes& input) override {
    std::shared_ptr<SapSoftwareSigner> signer;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto found = contexts_.find(context);
      if (found == contexts_.end() || !found->second.signer) {
        return make_error(ErrorCode::kSignerUnavailable,
                          "SAP setup has not completed");
      }
      signer = found->second.signer;
    }
    return signer->sign(input);
  }

  Status teardown(std::uint64_t context) override {
    std::lock_guard<std::mutex> lock(mutex_);
    contexts_.erase(context);
    return ok_status();
  }

 private:
  struct Context {
    std::shared_ptr<SapSoftwareHandshake> handshake;
    std::shared_ptr<SapSoftwareSigner> signer;
  };

  std::mutex mutex_;
  std::map<std::uint64_t, Context> contexts_;
  std::uint64_t next_context_ = 1;
};

}  // namespace

std::shared_ptr<SapBackend> create_software_sap_backend() {
  return std::make_shared<SoftwareSapBackend>();
}

Result<std::shared_ptr<SapBackend>> create_platform_sap_backend() {
  return std::shared_ptr<SapBackend>(create_software_sap_backend());
}

}  // namespace ipashelf
