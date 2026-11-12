// Content-addressed store for data a savestate needs from outside the game's memory (GPU resource contents, shader
// code): savestates\gpu\<xx>\<hash>.bin. Identical data is kept once, however many savestates use it. Writes go
// through a background thread.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace store {

// Off: nothing is stored (savestates only for this session); Fresh: stored data starts empty (what is on disk is from
// an earlier session and about to be swept); Keep: what is on disk counts (savestates of earlier sessions use it).
enum Mode { Off, Fresh, Keep };
void Init(const std::string& folder, Mode mode);
uint64_t Hash(const void* p, size_t n);
bool Has(uint64_t hash);
void Put(uint64_t hash, std::vector<uint8_t>&& data);   // queued; no-op when present
bool Get(uint64_t hash, std::vector<uint8_t>* out);
void Flush();                                            // waits until queued writes are on disk
size_t Pending();
// Deletes stored data that neither `keep` nor this session (stored or queued) refers to.
void Sweep(const std::unordered_set<uint64_t>& keep);
// Deletes all stored data (an earlier session's savestates are gone) and the folder if it ends up empty.
void Clear();

}  // namespace store
