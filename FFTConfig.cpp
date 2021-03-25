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

// This table tells us how many bits-per-word are saved by MAX_ACCURACY, the 6 MiddleMul chains, and ULTRA_TRIG.
// The 8 columns are the BPW savings for MAX_ACCURACY, MM2_CHAIN=1, MM_CHAIN=1, MM2_CHAIN=2, MM_CHAIN=2,
// MM2_CHAIN=3, MM_CHAIN=3, ULTRA_TRIG #defines.
//
// These values are different for each MIDDLE value and are generated by looking at the average round off error and pErr
// for a sample exponent using progressively longer chain lengths and finally ULTRA_TRIG.  If the average round off error drops
// from .26 to .24 then we can store about log2(((.26 - .24) / .26)) / 2 more bits per FFT word.  Actually the BPW savings is
// a bit more complicated than that because the std. dev. is also changing which impacts pErr.
//
// FFTConfig.h sets max bits-per-word based on a pErr of 0.5%.  The last entry accounts for increased ULTRA_TRIG accuracy plus
// some extra bits-per-word to take us to a pErr of about 0.1%.  Note that the 4 digits of precision in the
// table below is ludicrous.
//
// The MAX_ACCURACY savings is an educated conservative guess based on a sample size of one.  MAX_ACCURACY is not very costly.
static double chain_savings[16][8] = {
	{0, 0, 0, 0, 0, 0, 0, 0},						// MIDDLE=0
	{0, 0, 0, 0, 0, 0, 0, 0},						// MIDDLE=1
	{0, 0, 0, 0, 0, 0, 0, 0},						// MIDDLE=2
	{0.15, 0.0116, 0.0000, 0.0094, 0.0000, 0.0000, 0.0177, 0.0176+0.0211},	// MIDDLE=3
	{0.13, 0.0192, 0.0385, 0.0221, 0.0000, 0.0000, 0.0045, 0.0176+0.0074},	// MIDDLE=4
	{0.05, 0.0329, 0.0658, 0.0231, 0.0031, 0.0000, 0.0000, 0.0176+0.0137},	// MIDDLE=5
	{0.09, 0.0781, 0.1562, 0.0579, 0.0092, 0.0000, 0.0016, 0.0176+0.0386},	// MIDDLE=6
	{0.08, 0.0640, 0.1280, 0.0417, 0.0089, 0.0096, 0.0000, 0.0176+0.0137},	// MIDDLE=7
	{0.05, 0.0652, 0.1304, 0.0464, 0.0124, 0.0093, 0.0005, 0.0176+0.0122},	// MIDDLE=8
	{0.07, 0.0811, 0.1621, 0.0590, 0.0174, 0.0134, 0.0016, 0.0176+0.0122},	// MIDDLE=9
	{0.05, 0.0836, 0.1672, 0.0638, 0.0195, 0.0172, 0.0047, 0.0176+0.0079},	// MIDDLE=10
	{0.05, 0.0793, 0.1587, 0.0672, 0.0241, 0.0220, 0.0026, 0.0176+0.0040},	// MIDDLE=11
	{0.05, 0.1040, 0.2080, 0.0860, 0.0246, 0.0275, 0.0086, 0.0176+0.0209},	// MIDDLE=12
	{0.05, 0.0890, 0.1779, 0.0814, 0.0286, 0.0303, 0.0068, 0.0176+0.0059},	// MIDDLE=13
	{0.06, 0.0962, 0.1925, 0.0924, 0.0280, 0.0327, 0.0113, 0.0176+0.0058},	// MIDDLE=14
	{0.05, 0.1045, 0.2090, 0.0897, 0.0413, 0.0358, 0.0094, 0.0176+0.0154}};	// MIDDLE=15

tuple<bool,u32,u32,bool> FFTConfig::getChainLengths(u32 fftSize, u32 exponent, u32 middle) {
  i32 i;
  u32 maxExp = getMaxExp(fftSize, middle);
  double max_bits_per_word = double(maxExp) / double(fftSize);
  double bits_per_word = double(exponent) / double(fftSize);
  for (i = 7; i >= 0; i--) {
    max_bits_per_word -= chain_savings[middle][i];
    if (bits_per_word >= max_bits_per_word) { break; }
  }
  
  auto [mm_chain, mm2_chain] = vector<pair<u32,u32>>{{0, 0}, {0, 0}, {0, 1}, {1, 1}, {1, 2}, {2, 2}, {2, 3}, {3, 3}, {3, 3}}[i + 1];
  if (middle <= 6 && mm2_chain == 3) { mm2_chain = 2; } // For MIDDLE=3-6, mm2_chain=2 is better than mm2_chain=3
  if ((middle == 5 || middle == 7) && mm_chain == 3) { mm_chain = 2; } // For MIDDLE=5,7, mm_chain=2 is better than mm_chain=3

  bool max_accuracy = (i >= 0);
  bool ultra_trig = (i == 7);

  return {max_accuracy, mm_chain, mm2_chain, ultra_trig};
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
    u32 middle = parseInt(spec.substr(p1+1, p2 - (p1 + 1)));
    u32 height = parseInt(spec.substr(p2+1));
    return {width, middle, height};
  } else {
    u32 fftSize = parseInt(spec);
    vector<FFTConfig> configs = genConfigs();    
    if (auto it = find_if(configs.begin(), configs.end(), [fftSize](const FFTConfig& c) { return c.fftSize() >= fftSize; }); it != configs.end()) { return *it; }
    log("Could not find a FFT config for '%s'\n", spec.c_str());
    throw "Invalid FFT spec";
  }
}

vector<FFTConfig> FFTConfig::genConfigs() {
  vector<FFTConfig> configs;
  for (u32 width : {256, 1024}) {
    for (u32 height : {256, 1024}) {
      for (u32 middle : {2, 4}) {
             // {3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}) {
        configs.push_back({width, middle, height});
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
