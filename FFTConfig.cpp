// Copyright (C) Mihai Preda.

#include "FFTConfig.h"
#include "common.h"

#include <cmath>
#include <cassert>
#include <vector>
#include <algorithm>
#include <string>

using namespace std;

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
  for (u32 width : {256, 512, 1024, 4096}) {
    for (u32 height : {256, 512, 1024}) {
      for (u32 middle : {2, 4, 8, /*3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15*/}) {
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
