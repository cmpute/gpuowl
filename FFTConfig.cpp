// Copyright (C) Mihai Preda.

#include "FFTConfig.h"
#include "common.h"

#include <cmath>
#include <cassert>
#include <vector>
#include <algorithm>
#include <string>

using namespace std;

// This routine predicts the maximum carry32 we might see.  This was based on 500,000 iterations
// of 24518003 using a 1.25M FFT.  The maximum carry32 value observed was 0x32420000.
// As FFT length grows, so does the expected max carry32.  As we store fewer bits-per-word in
// an FFT size, the expected max carry32 decreases.  Our formula is:
//		max carry32 = 0x32420000 * 2^(BPW - 18.706) * 2 ^ (2 * 0.279 * log2(FFTSize / 1.25M))
//
// Note that the mul-by-3 carryFusedMul kernel, triples the expected max carry32 value.
// As of now, I have limited data on the carryFusedMulDelta kernel.
//
// Note: This routine returns the top 16 bits of the expected max carry32.

u32 FFTConfig::getMaxCarry32(u32 fftSize, u32 exponent) {
  return (u32) (0x3242 * pow (2.0, 0.558 * log2(fftSize / (1.25 * 1024 * 1024)) + double(exponent) / double(fftSize) - 18.706));
}

// This table tells us how many bits-per-word are saved for the 6 MiddleMul chain #defines.
// The 6 columns are for the MM_CHAIN4, MM2_CHAIN4, MM_CHAIN3, MM2_CHAIN3, MM_CHAIN2, MM2_CHAINe #defines.
// These values are different for each MIDDLE value and are generated by looking at the average round off error
// for a sample exponent using progressively smaller chain lengths.  If the average round off error drops
// from .26 to .24 then we can store log2(((.26 - .24) / .26)) / 2 more bits per FFT word.
static double chain_savings[16][6] = {
	{0, 0, 0, 0, 0, 0},				// MIDDLE=0
	{0, 0, 0, 0, 0, 0},				// MIDDLE=1
	{0, 0, 0, 0, 0, 0},				// MIDDLE=2
	{0, 0, 0, 0, 0, 0},				// MIDDLE=3
	{0, 0, 0, 0, 0, 0.01545},			// MIDDLE=4
	{0, 0, 0, 0, 0.03601, 0.03260},			// MIDDLE=5
	{0, 0.04395, 0.06918, 0, 0.06233, 0.02752},	// MIDDLE=6
	{0.07562, 0.06041, 0, 0, 0, 0.02210},		// MIDDLE=7
	{0.08041, 0.07971, 0, 0, 0.01159, 0.03387},	// MIDDLE=8
	{0.10607, 0.06663, 0, 0.02365, 0, 0.00552},	// MIDDLE=9
	{0.11329, 0.15097, 0, 0, 0.01641, 0.01082},	// MIDDLE=10
	{0.11155, 0.14219, 0, 0, 0.02002, 0.01579},	// MIDDLE=11
	{0.14193, 0.18250, 0, 0.00987, 0.01078, 0.00937}, // MIDDLE=12
	{0, 0, 0, 0, 0, 0},				// MIDDLE=13
	{0, 0, 0, 0, 0, 0},				// MIDDLE=14
	{0, 0, 0, 0, 0, 0}};				// MIDDLE=15

void FFTConfig::getChainLengths(u32 fftSize, u32 exponent, u32 middle, u32 *mm_chain, u32 *mm2_chain) {
  i32 i;
  u32 maxExp = getMaxExp(fftSize, middle);
  double max_bits_per_word = double(maxExp) / double(fftSize);
  double bits_per_word = double(exponent) / double(fftSize);
  for (i = 5; i >= 0; i--) {
    max_bits_per_word -= chain_savings[middle][i];
    if (bits_per_word >= max_bits_per_word) break;
  }
  if (i == 5) *mm_chain = 2, *mm2_chain = 2;
  if (i == 4) *mm_chain = 3, *mm2_chain = 2;
  if (i == 3) *mm_chain = 3, *mm2_chain = 3;
  if (i == 2) *mm_chain = 4, *mm2_chain = 3;
  if (i == 1) *mm_chain = 4, *mm2_chain = 4;
  if (i == 0) *mm_chain = 0, *mm2_chain = 4;
  if (i == -1) *mm_chain = 0, *mm2_chain = 0;
}

namespace {

u32 parseInt(const string& s) {
  if (s.empty()) { return 1; }
  char c = s.back();
  u32 multiple = c == 'k' || c == 'K' ? 1024 : c == 'm' || c == 'M' ? 1024 * 1024 : 1;
  return strtod(s.c_str(), nullptr) * multiple;
}

}

FFTConfig FFTConfig::fromSpec(const string& spec) {
  bool hasParts = spec.find(':') != string::npos;
  if (hasParts) {
    auto p1 = spec.find(':');
    u32 width = parseInt(spec.substr(0, p1));
    auto p2 = spec.find(':', p1+1);
    if (p2 == string::npos) {
      log("FFT spec must be of the form width:middle:height , found '%s'\n", spec.c_str());
      throw "Invalid FFT spec";
    }
    u32 middle = parseInt(spec.substr(p1+1, p2));
    u32 height = parseInt(spec.substr(p2+1));
    return {width, middle, height};
  } else {
    u32 fftSize = parseInt(spec);
    vector<FFTConfig> configs = genConfigs();
    for (auto config : configs) {
      if (config.fftSize() >= fftSize) {
        return config;
      }
    }
    log("Could not find a FFT config for '%s'\n", spec.c_str());
    throw "Invalid FFT spec";
  }
}

vector<FFTConfig> FFTConfig::genConfigs() {
  vector<FFTConfig> configs;
  for (u32 width : {256, 512, 1024, 4096}) {
    for (u32 height : {256, 512, 1024}) {
      for (u32 middle : {1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}) {
        if (middle > 1 || width * height < 512 * 512) {
          configs.push_back({width, middle, height});
        }
      }
    }
  }
  std::sort(configs.begin(), configs.end(),
            [](const FFTConfig &a, const FFTConfig &b) {
              if (a.fftSize() != b.fftSize()) { return (a.fftSize() < b.fftSize()); }
              if (a.width != b.width) {
                if (a.width == 1024 || b.width == 1024) { return a.width == 1024; }
                return a.width < b.width;
              }
              return a.height < b.height;
            });
  return configs;
}

string numberK(u32 n) {
  u32 K = 1024;
  u32 M = K * K;

  if (n % M == 0) { return to_string(n / M) + 'M'; }

  char buf[64];
  if (n >= M && (n * u64(100)) % M == 0) {
    snprintf(buf, sizeof(buf), "%.2f", float(n) / M);
    return string(buf) + 'M';
  } else if (n >= K) {
    snprintf(buf, sizeof(buf), "%g", float(n) / K);
    return string(buf) + 'K';
  } else {
    return to_string(n);
  }
}
