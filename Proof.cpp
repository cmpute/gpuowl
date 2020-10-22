// Copyright (C) Mihai Preda.

#include "Proof.h"
#include "ProofCache.h"
#include "Sha3Hash.h"
#include "MD5.h"
#include "Gpu.h"

#include <vector>
#include <string>
#include <cassert>
#include <filesystem>
#include <cinttypes>
#include <climits>

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error Byte order must be Little Endian
#endif


namespace proof {

array<u64, 4> hashWords(u32 E, const Words& words) {
  return std::move(SHA3{}.update(words.data(), (E-1)/8+1)).finish();
}

array<u64, 4> hashWords(u32 E, array<u64, 4> prefix, const Words& words) {
  return std::move(SHA3{}.update(prefix).update(words.data(), (E-1)/8+1)).finish();
}

string fileHash(const fs::path& filePath) {
  File fi = File::openRead(filePath, true);
  char buf[64 * 1024];
  MD5 h;
  u32 size = 0;
  while ((size = fi.readUpTo(buf, sizeof(buf)))) { h.update(buf, size); }
  return std::move(h).finish();
}

ProofInfo getInfo(const fs::path& proofFile) {
  string hash = proof::fileHash(proofFile);
  File fi = File::openRead(proofFile, true);
  u32 E = 0, power = 0;
  char c = 0;
  if (fi.scanf(Proof::HEADER, &power, &E, &c) != 3 || c != '\n') {
    log("Proof file '%s' has invalid header\n", proofFile.string().c_str());
    throw "Invalid proof header";
  }
  return {power, E, hash};
}

}

// ---- Proof ----

fs::path Proof::save(const fs::path& proofResultDir) {
  string strE = to_string(E);
  u32 power = middles.size();
  fs::path fileName = proofResultDir / (strE + '-' + to_string(power) + ".proof");
  File fo = File::openWrite(fileName);
  fo.printf(HEADER, power, E, '\n');
  fo.write(B.data(), (E-1)/8+1);
  for (const Words& w : middles) { fo.write(w.data(), (E-1)/8+1); }
  return fileName;
}

Proof Proof::load(const fs::path& path) {
  File fi = File::openRead(path, true);
  u32 E = 0, power = 0;
  char c = 0;
  if (fi.scanf(HEADER, &power, &E, &c) != 3 || c != '\n') {
    log("Proof file '%s' has invalid header\n", path.string().c_str());
    throw "Invalid proof header";
  }
  u32 nBytes = (E - 1) / 8 + 1;
  Words B = fi.readBytesLE(nBytes);
  vector<Words> middles;
  for (u32 i = 0; i < power; ++i) { middles.push_back(fi.readBytesLE(nBytes)); }
  return {E, B, middles};
}

bool Proof::verify(Gpu *gpu) {
  u32 power = middles.size();
  assert(power > 0);

  u32 topK = roundUp(E, (1 << power));
  assert(topK % (1 << power) == 0);
  assert(topK > E);
  u32 step = topK / (1 << power);

  bool isPrime = false;
  {
    Words A{makeWords(E, 3)};
    log("proof: doing %d iterations\n", topK - E + 1);
    A = gpu->expExp2(A, topK - E + 1);
    isPrime = (A == B);
    // log("the proof indicates %s (%016" PRIx64 " vs. %016" PRIx64 " for a PRP)\n",
    //    isPrime ? "probable prime" : "composite", res64(B), res64(A));
  }

  Words A{makeWords(E, 3)};
    
  auto hash = proof::hashWords(E, B);
    
  for (u32 i = 0; i < power; ++i) {
    Words& M = middles[i];
    hash = proof::hashWords(E, hash, M);
    u64 h = hash[0];
    A = gpu->expMul(A, h, M);
    B = gpu->expMul(M, h, B);
  }
    
  log("proof verification: doing %d iterations\n", step);
  A = gpu->expExp2(A, step);

  bool ok = (A == B);
  if (ok) {
    log("proof: %u proved %s\n", E, isPrime ? "probable prime" : "composite");
  } else {
    log("proof: invalid (%016" PRIx64 " expected %016" PRIx64 ")\n", res64(A), res64(B));
  }
  return ok;
}

// ---- ProofSet ----

Proof ProofSet::computeProof(Gpu *gpu) {
  assert(power > 0);
    
  Words B = load(topK);
  Words A = makeWords(E, 3);

  vector<Words> middles;
  vector<u64> hashes;

  auto hash = proof::hashWords(E, B);

  vector<Buffer<i32>> bufVect = gpu->makeBufVector(power);
    
  for (u32 p = 0; p < power; ++p) {
    auto bufIt = bufVect.begin();
    assert(p == hashes.size());
    log("proof: building level %d, hash %016" PRIx64 "\n", (p + 1), hash[0]);
    u32 s = topK / (1 << (p + 1));
    for (int i = 0; i < (1 << p); ++i) {
      Words w = load(s * (2 * i + 1));
      gpu->writeIn(*bufIt++, w);
      for (int k = 0; i & (1 << k); ++k) {
        --bufIt;
        u64 h = hashes[p - 1 - k];
        gpu->expMul(*(bufIt - 1), h, *bufIt);
      }
    }
    assert(bufIt == bufVect.begin() + 1);
    middles.push_back(gpu->readAndCompress(bufVect.front()));
    hash = proof::hashWords(E, hash, middles.back());
    hashes.push_back(hash[0]);
  }
  return Proof{E, std::move(B), std::move(middles)};
}
