// Copyright (C) Mihai Preda.

#include "Task.h"

#include "Gpu.h"
#include "Args.h"
#include "File.h"
#include "GmpUtil.h"
#include "Worktodo.h"
#include "Saver.h"
#include "version.h"
#include "Proof.h"
#include "log.h"

#include <cstdio>
#include <cmath>
#include <thread>
#include <cassert>

namespace {

string json(const vector<string>& v) {
  bool isFirst = true;
  string s = "{";
  for (const std::string& e : v) {
    if (e.empty()) { continue; }
    if (!isFirst) { s += ", "; } else { isFirst = false; }
    s += e;
  }
  return {isFirst ? ""s : (s + '}')};
}

struct Hex {
  explicit Hex(u64 value) : value{value} {}
  u64 value;
};

string json(Hex x) { return '"' + hex(x.value) + '"'; }
string json(const string& s) { return '"' + s + '"'; }
string json(u32 x) { return json(to_string(x)); }

template<typename T> string json(const string& key, const T& value) { return json(key) + ':' + json(value); }

string maybe(const string& key, const string& value) { return value.empty() ? ""s : json(key, value); }

template<typename T> void operator+=(vector<T>& a, const vector<T>& b) { a.insert(a.end(), b.begin(), b.end()); }

vector<string> commonFields(u32 E, const char *worktype, const string &status) {
  return {json("status", status),
          json("exponent", E),
          json("worktype", worktype)
  };
}

vector<string> tailFields(const std::string &AID, const Args &args) {
  return {json("program", vector<string>{json("name", "gpuowl"), json("version", VERSION)}),
          maybe("user", args.user),
          maybe("computer", args.cpu),
          maybe("aid", AID),
          maybe("uid", args.uid),
          json("timestamp", timeStr())
  };
}

void writeResult(u32 E, const char *workType, const string &status, const std::string &AID, const Args &args,
                 const vector<string>& extras) {
  vector<string> fields = commonFields(E, workType, status);
  fields += extras;
  fields += tailFields(AID, args);
  string s = json(std::move(fields));
  log("%s\n", s.c_str());
  File::append(args.resultsFile, s + '\n');
}

}

/*
string Task::kindStr() const {
  assert(kind == PRP || kind == PM1);
  return kind == PRP ? "PRP" : "PM1";
}
*/

void Task::writeResultPRP(const Args &args, bool isPrime, u64 res64, u32 fftSize, u32 nErrors, const fs::path& proofPath) const {
  vector<string> fields{json("res64", Hex{res64}),
                        json("residue-type", 1),
                        json("errors", vector<string>{json("gerbicz", nErrors)}),
                        json("fft-length", fftSize)
  };

  // "proof":{"version":1, "power":6, "hashsize":64, "md5":"0123456789ABCDEF"}, 
  if (!proofPath.empty()) {
    ProofInfo info = proof::getInfo(proofPath);
    fields.push_back(json("proof", vector<string>{
            json("version", 1),
            json("power", info.power),
            json("hashsize", 64),
            json("md5", info.md5)
            }));
  }
  
  writeResult(exponent, "PRP-3", isPrime ? "P" : "C", AID, args, fields);
}

void Task::writeResultPM1(const Args& args, const string& factor, u32 fftSize) const {
  assert(B1);
  bool hasFactor = !factor.empty();

  u32 reportB2 = B2;

  writeResult(exponent, "PM1", hasFactor ? "F" : "NF", AID, args,
              {json("B1", B1),
               (reportB2 > B1) ? json("B2", reportB2) : "",
               json("fft-length", fftSize),
               factor.empty() ? "" : (json("factors") + ':' + "[\""s + factor + "\"]")
              });
}

void Task::execute(const Args& args) {
  LogContext pushContext(std::to_string(exponent));
  
  if (kind == VERIFY) {
    Proof proof = Proof::load(verifyPath);
    auto gpu = Gpu::make(proof.E, args);
    bool ok = proof.verify(gpu.get());
    log("proof '%s' %s\n", verifyPath.c_str(), ok ? "verified" : "failed");
    return;
  }

  assert(kind == PRP || kind == PM1);

  auto gpu = Gpu::make(exponent, args);
  auto fftSize = gpu->getFFTSize();

  if (kind == PRP) {
    auto [factor, isPrime, res64, nErrors, proofPath] = gpu->isPrimePRP(args, *this);
    if (factor.empty()) {
      writeResultPRP(args, isPrime, res64, fftSize, nErrors, proofPath);
    }

    Worktodo::deleteTask(*this);
    if (!isPrime) { Saver::cleanup(exponent, args); }
  } else { // P-1
    LogContext p1{"P1"};
    assert(!line.empty());  // We want to pass the same line to mprime following first-stage
    gpu->doPm1(args, *this);
    File::openAppend(args.mprimeDir/"worktodo.add").write(line);
    Worktodo::deleteTask(*this);
    /*
    {
      char buf[256];
      snprintf(buf, sizeof(buf), "Pminus1=%s,1,2,%u,-1,%u,%u,%u\n",
               AID.empty() ? "N/A" : AID.c_str(), exponent, B1, B1, howFarFactored);
      File fo = File::openAppend(args.mprimeDir/"worktodo.add");
      fo.write(""s + buf);
    }
    */
  }
}
