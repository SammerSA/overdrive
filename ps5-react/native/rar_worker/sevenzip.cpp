// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: MIT
//
// Native 7-Zip decoder behind the existing 7ZX1 worker protocol. Independent
// folders use private readers; a single folder uses native LZMA2 reset-block MT
// when the memory budget permits it. The parent still owns staging/publication.
#include "sevenzip.hpp"
#include "worker_io.hpp"
#include "CPP/Common/MyInitGuid.h"
#include "CPP/Common/MyCom.h"
#include "CPP/Common/UTFConvert.h"
#include "CPP/Windows/PropVariant.h"
#include "CPP/7zip/Archive/7z/7zHandler.h"
#include "CPP/7zip/Common/RegisterCodec.h"
#include "CPP/7zip/Compress/ZstdDecoder.h"
#include <atomic>
#include <chrono>
#include <map>
#include <set>
#include <string_view>

// The third-party 7z Zstandard method ID that libarchive already accepts.
namespace NCompress { namespace NZstd {
REGISTER_CODEC_E(OverdriveZstd, CDecoder(), CEncoder(), 0x04F71101, "ZSTD")
}}

namespace sevenzip {
namespace {
using namespace worker;
using NWindows::NCOM::CPropVariant;
constexpr std::uint64_t mib = 1ULL << 20, budget = 320 * mib;
constexpr unsigned max_threads = 4;
const char* unsafe = "Archive contains an unsafe path, link or special file.";
const char* damaged = "Unsupported or damaged 7z archive.";
const char* too_large = "Archive exceeds the extraction size limit.";

struct Options {
  std::uint64_t memory = 0, limit = 0;
  unsigned threads = max_threads;
  std::vector<std::string> flatten, sources;
  std::string reserved, destination;
};
bool read_options(Options& o) {
  std::string line;
  while (read_line(line)) {
    const auto at = line.find(' ');
    const auto key = line.substr(0, at), value = at == std::string::npos ? "" : line.substr(at + 1);
    if (key == "end") return !o.destination.empty() && !o.sources.empty() && o.limit && o.memory && o.memory <= 288 * mib;
    if (key == "memory") o.memory = strtoull(value.c_str(), nullptr, 10);
    else if (key == "threads") o.threads = unsigned(std::clamp<unsigned long>(strtoul(value.c_str(), nullptr, 10), 1, max_threads));
    else if (key == "limit") o.limit = strtoull(value.c_str(), nullptr, 10);
    else if (key == "flatten") o.flatten.push_back(value);
    else if (key == "reserved") o.reserved = value;
    else if (key == "dest") o.destination = value;
    else if (key == "source") o.sources.push_back(value);
  }
  return false;
}
struct State {
  std::atomic<bool> cancelled{false}, abort{false};
  std::atomic<std::uint64_t> written{0};
  std::mutex mutex;
  std::string error;
  void fail(const std::string& reason) {
    std::lock_guard lock(mutex);
    if (error.empty()) error = reason;
    abort = true;
  }
  bool stopped() const { return cancelled || abort; }
};

// One seekable concatenation of the ordered volumes per decoder. No joined
// archive, shared file offset or simultaneous calls into one SDK handler.
class Input final : public IInStream, public CMyUnknownImp {
  Z7_COM_UNKNOWN_IMP_2(IInStream, ISequentialInStream)
  Z7_IFACE_COM7_IMP(ISequentialInStream)
  Z7_IFACE_COM7_IMP(IInStream)
public:
  struct Part { int fd; std::uint64_t start, size; };
  std::vector<Part> parts;
  std::uint64_t position = 0, length = 0;
  State& state;
  explicit Input(State& s) : state(s) {}
  ~Input() { for (const auto& p : parts) close(p.fd); }
  bool open_sources(const std::vector<std::string>& paths) {
    for (const auto& path : paths) {
      const int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
      struct stat st;
      if (fd < 0) return false;
      if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
          std::uint64_t(st.st_size) > std::uint64_t(INT64_MAX) - length) { close(fd); return false; }
      parts.push_back({fd, length, std::uint64_t(st.st_size)});
      length += st.st_size;
    }
    return true;
  }
};
Z7_COM7F_IMF(Input::Read(void* data, UInt32 size, UInt32* processed)) {
  if (processed) *processed = 0;
  if (state.stopped()) return E_ABORT;
  for (const auto& p : parts) {
    if (position < p.start || position >= p.start + p.size) continue;
    const auto count = std::min<std::uint64_t>(size, p.start + p.size - position);
    ssize_t got;
    do { got = pread(p.fd, data, count, position - p.start); } while (got < 0 && errno == EINTR);
    if (got < 0) return E_FAIL;
    position += got;
    if (processed) *processed = UInt32(got);
    return S_OK;
  }
  return position <= length ? S_OK : E_FAIL;
}
Z7_COM7F_IMF(Input::Seek(Int64 offset, UInt32 origin, UInt64* result)) {
  const std::uint64_t base = origin == 0 ? 0 : origin == 1 ? position : length;
  if (origin > 2 || (offset < 0 && std::uint64_t(-(offset + 1)) + 1 > base) ||
      (offset >= 0 && std::uint64_t(offset) > std::uint64_t(INT64_MAX) - base)) return E_INVALIDARG;
  position = base + offset;
  if (result) *result = position;
  return S_OK;
}
struct Reader {
  CMyComPtr<IInArchive> archive;
  CMyComPtr<IInStream> input;
  NArchive::N7z::CHandler* handler = nullptr; // Owned by archive.
  bool configure(unsigned threads, std::uint64_t decoder_budget) {
    const wchar_t* names[] = {L"mt", L"mtf", L"memuse"};
    CPropVariant values[3];
    values[0] = UInt32(threads);
    values[1] = true;
    values[2] = UInt64(decoder_budget);
    return static_cast<ISetProperties*>(handler)->SetProperties(names, values, 3) == S_OK;
  }
  bool open_archive(const Options& o, State& state, unsigned threads, std::uint64_t decoder_budget) {
    auto* stream = new Input(state);
    input = stream;
    if (!stream->open_sources(o.sources)) return false;
    handler = new NArchive::N7z::CHandler;
    archive = handler;
    return configure(threads, decoder_budget) && archive->Open(input, nullptr, nullptr) == S_OK;
  }
  std::uint64_t database_bytes() const {
    const auto& db = handler->MemoryDatabase();
    const auto vector_bytes = [](const auto& v) { return std::uint64_t(v.Size()) * sizeof(v[0]); };
    const auto optional_bytes = [&](const auto& v) { return vector_bytes(v.Defs) + vector_bytes(v.Vals); };
    // Record vectors may retain growth capacity. The raw name/coder buffers and
    // object arrays below have exact sizes in the pinned SDK.
    std::uint64_t vectors = vector_bytes(db.Files) + vector_bytes(db.IsAnti) +
        optional_bytes(db.CTime) + optional_bytes(db.ATime) + optional_bytes(db.MTime) +
        optional_bytes(db.StartPos) + optional_bytes(db.Attrib) + optional_bytes(db.FolderCRCs) +
        vector_bytes(db.ParsedMethods.IDs) + vector_bytes(db.ArcInfo.FileInfoPopIDs);
    std::uint64_t bytes = 4 * mib + vectors + vectors / 4 + db.NamesBuf.Size() + db.CodersData.Size();
    const std::uint64_t files = db.Files.Size(), folders = db.NumFolders;
    bytes += (files + 1) * sizeof(size_t) + files * sizeof(NArchive::N7z::CNum);
    if (db.NumPackStreams) bytes += (std::uint64_t(db.NumPackStreams) + 1) * sizeof(UInt64);
    if (folders) {
      bytes += folders * (sizeof(NArchive::N7z::CNum) * 2 + sizeof(Byte));
      bytes += (folders + 1) * (sizeof(NArchive::N7z::CNum) * 2 + sizeof(size_t));
      bytes += std::uint64_t(db.FoToCoderUnpackSizes[db.NumFolders]) * sizeof(UInt64);
    }
    return bytes;
  }
  bool single_lzma2_per_folder() const {
    const auto& db = handler->MemoryDatabase();
    for (unsigned i = 0; i < db.NumFolders; ++i) {
      NArchive::N7z::CFolder folder;
      db.ParseFolderInfo(i, folder);
      unsigned decoders = 0;
      for (unsigned j = 0; j < folder.Coders.Size(); ++j)
        if (folder.Coders[j].MethodID == 0x21 && ++decoders > 1) return false;
    }
    return true;
  }
};
struct Entry { std::string name; std::uint64_t size = 0; bool directory = false; };
struct Plan {
  std::vector<Entry> entries;
  std::vector<std::vector<UInt32>> groups;
  std::uint64_t shared_memory = 0, reader_memory = 0, planned_memory = 0, opening_memory = 0;
  unsigned workers = 1, decoder_threads = 1;
  std::atomic<unsigned> used{0}, active{0}, peak{0};
  std::atomic<std::uint64_t> write_ns{0}, sync_ns{0}, wait_ns{0};
  std::atomic<unsigned> writes{0}, syncs{0};
};
bool property(IInArchive* a, UInt32 i, PROPID id, CPropVariant& v) { return a->GetProperty(i, id, &v) == S_OK; }
bool flag(IInArchive* a, UInt32 i, PROPID id, bool& bad) {
  CPropVariant v;
  if (!property(a, i, id, v) || (v.vt != VT_EMPTY && v.vt != VT_BOOL)) { bad = true; return false; }
  return v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE;
}
std::string plan_archive(Reader& reader, const Options& o, Plan& plan, State& state) {
  UInt32 count = 0;
  if (reader.archive->GetNumberOfItems(&count) != S_OK || count > 1000000) return damaged;
  std::map<UInt32, std::vector<UInt32>> folders;
  std::set<std::string, std::less<>> files, directories;
  std::uint64_t declared = 0;
  plan.reader_memory = reader.database_bytes();
  // Validation temporarily retains duplicate file names and implicit directory
  // prefixes. Bound those too, before insertion: a small header can describe
  // thousands of distinct deep paths whose expanded prefixes dwarf the header.
  // Prepay worst-case growth/overlap of the index vectors and their final move.
  std::uint64_t validation_memory = plan.reader_memory + 8 * mib +
      std::uint64_t(count) * (sizeof(Entry) + sizeof(std::vector<UInt32>) + 16);
  const auto charge = [&](std::uint64_t bytes) {
    if (validation_memory > budget || bytes > budget - validation_memory) return false;
    validation_memory += bytes;
    return true;
  };
  const auto keep_name = [&](auto& names, std::string_view name) {
    if (names.contains(name)) return 0;
    // Node links/color, string object, allocator rounding and the character data.
    if (!charge(sizeof(std::string) + 8 * sizeof(void*) + name.size() + 48)) return -1;
    names.emplace(name);
    return 1;
  };
  if (validation_memory > budget) return too_much_memory;
  plan.entries.reserve(count);
  for (UInt32 i = 0; i < count; ++i) {
    if (state.stopped()) return {};
    Entry e;
    CPropVariant v;
    if (!property(reader.archive, i, kpidPath, v) || v.vt != VT_BSTR || !v.bstrVal) return unsafe;
    AString utf8;
    ConvertUnicodeToUTF8(v.bstrVal, utf8);
    CUtf8Check utf_check;
    utf_check.Check_Buf(utf8.Ptr(), utf8.Len());
    if (!utf_check.IsOK()) return unsafe;
    e.name.assign(utf8.Ptr(), utf8.Len());
    if (SysStringLen(v.bstrVal) != MyStringLen(v.bstrVal)) return unsafe;
    while (!e.name.empty() && e.name.back() == '/') e.name.pop_back();
    if (!e.name.empty() && e.name[0] != '/') e.name = without_dots(e.name);
    bool bad = false;
    e.directory = flag(reader.archive, i, kpidIsDir, bad);
    if (flag(reader.archive, i, kpidEncrypted, bad)) return "Encrypted 7z archives are not supported.";
    if (flag(reader.archive, i, kpidIsAnti, bad) || bad || !safe(e.name) ||
        (!o.reserved.empty() && e.name.find(o.reserved) != std::string::npos)) return unsafe;
    for (const auto id : {kpidSymLink, kpidHardLink}) {
      CPropVariant link;
      if (!property(reader.archive, i, id, link) || link.vt != VT_EMPTY) return unsafe;
    }
    CPropVariant attrib;
    if (!property(reader.archive, i, kpidAttrib, attrib) || (attrib.vt != VT_EMPTY && attrib.vt != VT_UI4)) return unsafe;
    if (attrib.vt == VT_UI4) {
      const unsigned mode = attrib.ulVal >> 16, type = mode & S_IFMT;
      if ((attrib.ulVal & 0x400) || (type && type != (e.directory ? S_IFDIR : S_IFREG))) return unsafe;
    }
    CPropVariant size;
    if (!property(reader.archive, i, kpidSize, size) || size.vt != VT_UI8) return damaged;
    e.size = size.uhVal.QuadPart;
    if (!charge(e.name.capacity() + 1 + 16)) return too_much_memory;
    if (!e.directory) {
      if (e.size > o.limit - declared) return too_large;
      declared += e.size;
      if (flattened(o.flatten, e.name)) e.name = e.name.substr(e.name.rfind('/') + 1);
      const int kept = keep_name(files, e.name);
      if (kept < 0) return too_much_memory;
      if (!kept) return unsafe;
      CPropVariant block;
      if (!property(reader.archive, i, kpidBlock, block) || (block.vt != VT_EMPTY && block.vt != VT_UI4)) return damaged;
      const UInt32 folder = block.vt == VT_UI4 ? block.ulVal : UINT32_MAX;
      if (!folders.contains(folder) && !charge(128)) return too_much_memory;
      folders[folder].push_back(i);
    } else if (keep_name(directories, e.name) < 0) return too_much_memory;
    plan.entries.push_back(std::move(e));
  }
  // Precreate directories serially and reject file/dir and flattened collisions,
  // so parallel writers cannot change the meaning of another worker's path.
  for (const auto& e : plan.entries)
    for (size_t at = e.name.find('/'); at != std::string::npos; at = e.name.find('/', at + 1))
      if (keep_name(directories, std::string_view(e.name.data(), at)) < 0) return too_much_memory;
  for (const auto& dir : directories) if (files.contains(dir)) return unsafe;
  files.clear();
  for (const auto& dir : directories) {
    std::string target;
    if (!make_parents(o.destination, dir, target) ||
        (mkdir(target.c_str(), 0777) && (errno != EEXIST || !plain_directory(target)))) return strerror(errno);
  }
  plan.groups.reserve(folders.size());
  for (auto& pair : folders) plan.groups.push_back(std::move(pair.second));
  folders.clear();
  // Paths and the scheduling plan are shared, once. Only the SDK database is
  // duplicated. Counting the shared index for every reader needlessly reduced
  // large-file-count archives to two workers.
  plan.shared_memory = 4 * mib + plan.entries.capacity() * sizeof(Entry) +
      plan.groups.capacity() * sizeof(std::vector<UInt32>);
  for (const auto& e : plan.entries) plan.shared_memory += e.name.capacity() + 1 + 16;
  for (const auto& group : plan.groups) plan.shared_memory += group.capacity() * sizeof(UInt32) + 16;
  plan.workers = unsigned(std::min<std::uint64_t>({o.threads, std::uint64_t(std::max<long>(1, cores())),
      std::max<size_t>(1, plan.groups.size())}));
  // Readers open serially, before any writer exists. Leave a separate transient
  // allowance for encoded headers and their decoder rather than opening four
  // copies' scratch buffers at once. Runtime allowance includes each 16 MiB
  // writer ring plus filter/read buffers and decoder-thread overhead.
  for (;;) {
    plan.planned_memory = plan.shared_memory + plan.workers * (plan.reader_memory + o.memory + 24 * mib);
    plan.opening_memory = plan.shared_memory + plan.workers * plan.reader_memory + 128 * mib;
    if (std::max(plan.planned_memory, plan.opening_memory) <= budget) break;
    if (plan.workers == 1) return too_much_memory;
    --plan.workers;
  }
  const auto expected_block = std::max<std::uint64_t>(mib, std::max(o.memory, std::min<std::uint64_t>(256 * mib, o.memory * 4)));
  const auto mt_thread_memory = expected_block * 2 + expected_block / 16 + 320 * 1024;
  const auto decoder_budget = (budget - plan.shared_memory) / plan.workers - plan.reader_memory - 24 * mib;
  plan.decoder_threads = plan.workers == 1 ? unsigned(std::max<std::uint64_t>(1,
      std::min<std::uint64_t>({o.threads, std::uint64_t(std::max<long>(1, cores())), decoder_budget / mt_thread_memory}))) : 1;
  // SDK memory limits apply per codec. A compound folder must not grant the
  // same reset-block thread budget independently to multiple LZMA2 decoders.
  if (plan.decoder_threads > 1 && !reader.single_lzma2_per_folder()) plan.decoder_threads = 1;
  if (plan.decoder_threads > 1) {
    const auto decoder_memory = std::max(o.memory, mt_thread_memory * plan.decoder_threads);
    plan.planned_memory = plan.shared_memory + plan.workers * (plan.reader_memory + decoder_memory + 24 * mib);
  }
  return {};
}
class Output final : public ISequentialOutStream, public CMyUnknownImp {
  Z7_COM_UNKNOWN_IMP_1(ISequentialOutStream)
  Z7_IFACE_COM7_IMP(ISequentialOutStream)
public:
  Writer& writer;
  State& state;
  std::uint64_t remaining;
  Output(Writer& w, State& s, std::uint64_t size) : writer(w), state(s), remaining(size) {}
};
Z7_COM7F_IMF(Output::Write(const void* data, UInt32 size, UInt32* processed)) {
  if (processed) *processed = 0;
  if (state.stopped()) return E_ABORT;
  if (size > remaining) { state.fail(too_large); return E_FAIL; }
  if (!writer.append(static_cast<const unsigned char*>(data), size)) { state.fail(writer.error()); return E_FAIL; }
  remaining -= size;
  state.written += size;
  if (processed) *processed = size;
  return S_OK;
}
class Callback final : public IArchiveExtractCallback, public CMyUnknownImp {
  Z7_COM_UNKNOWN_IMP_1(IArchiveExtractCallback)
  Z7_IFACE_COM7_IMP(IProgress)
  Z7_IFACE_COM7_IMP(IArchiveExtractCallback)
public:
  Writer& writer;
  State& state;
  const Options& options;
  const Plan& plan;
  const Entry* current = nullptr;
  Output* output = nullptr;
  CMyComPtr<ISequentialOutStream> held;
  Callback(Writer& w, State& s, const Options& o, const Plan& p) : writer(w), state(s), options(o), plan(p) {}
};
Z7_COM7F_IMF(Callback::SetTotal(UInt64)) { return state.stopped() ? E_ABORT : S_OK; }
Z7_COM7F_IMF(Callback::SetCompleted(const UInt64*)) { return state.stopped() ? E_ABORT : S_OK; }
Z7_COM7F_IMF(Callback::PrepareOperation(Int32)) { return state.stopped() ? E_ABORT : S_OK; }
Z7_COM7F_IMF(Callback::GetStream(UInt32 index, ISequentialOutStream** out, Int32 mode)) {
  *out = nullptr;
  held.Release(); output = nullptr; current = nullptr;
  if (state.stopped()) return E_ABORT;
  if (mode != NArchive::NExtract::NAskMode::kExtract) return S_OK;
  if (index >= plan.entries.size()) return E_FAIL;
  current = &plan.entries[index];
  if (current->directory) return S_OK;
  const auto path = options.destination + "/" + current->name;
  const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0666);
  if (fd < 0) { state.fail(strerror(errno)); return E_FAIL; }
  writer.begin(fd, current->name);
  output = new Output(writer, state, current->size);
  held = output;
  *out = held; (*out)->AddRef();
  return S_OK;
}
Z7_COM7F_IMF(Callback::SetOperationResult(Int32 result)) {
  if (result != NArchive::NExtract::NOperationResult::kOK) {
    state.fail(result == NArchive::NExtract::NOperationResult::kCRCError ? "7z checksum mismatch." : damaged);
    return E_FAIL;
  }
  if (current && !current->directory) {
    if (!output || output->remaining) { state.fail("Extracted file size mismatch."); return E_FAIL; }
    writer.end(current->size);
  }
  held.Release(); output = nullptr; current = nullptr;
  return state.stopped() ? E_ABORT : S_OK;
}
std::string claim(std::uint64_t& took) {
  const auto start = now_ns();
  auto* block = static_cast<volatile unsigned char*>(std::malloc(budget));
  if (!block) return too_much_memory;
  for (std::uint64_t at = 0; at < budget; at += 4096) block[at] = 1;
  took = (now_ns() - start) / 1000000;
  std::free(const_cast<unsigned char*>(block));
  return took > 1000 ? "The console has too little free memory to extract this archive now. Close other apps and retry." : "";
}
void extract(const Options& o, State& state, Plan& plan) {
  std::vector<std::unique_ptr<Reader>> readers;
  readers.push_back(std::make_unique<Reader>());
  if (!readers.front()->open_archive(o, state, 1, o.memory)) { if (!state.stopped()) state.fail(damaged); return; }
  if (const auto error = plan_archive(*readers.front(), o, plan, state); !error.empty()) { state.fail(error); return; }
  if (state.stopped()) return;
  const auto decoder_budget = (budget - plan.shared_memory) / plan.workers - plan.reader_memory - 24 * mib;
  for (unsigned i = 1; i < plan.workers; ++i) {
    readers.push_back(std::make_unique<Reader>());
    if (!readers.back()->open_archive(o, state, 1, o.memory)) { if (!state.stopped()) state.fail(damaged); return; }
  }
  for (auto& reader : readers) if (!reader->configure(plan.decoder_threads, decoder_budget)) { state.fail(damaged); return; }
  std::atomic<size_t> next{0};
  std::vector<std::thread> threads;
  const auto run = [&](unsigned index) {
    try {
      auto& reader = *readers[index];
      Writer writer;
      CMyComPtr<IArchiveExtractCallback> callback = new Callback(writer, state, o, plan);
      bool used = false;
      while (!state.stopped()) {
        const auto i = next.fetch_add(1);
        if (i >= plan.groups.size()) break;
        if (!used) { ++plan.used; used = true; }
        const unsigned active = ++plan.active;
        unsigned peak = plan.peak;
        while (peak < active && !plan.peak.compare_exchange_weak(peak, active)) {}
        const auto& group = plan.groups[i];
        const HRESULT result = reader.archive->Extract(group.data(), UInt32(group.size()), false, callback);
        --plan.active;
        if (result != S_OK && !state.stopped()) state.fail(result == E_OUTOFMEMORY ? too_much_memory : damaged);
      }
      if (const auto error = writer.finish(); !error.empty()) state.fail(error);
      plan.write_ns += writer.write_ns; plan.sync_ns += writer.sync_ns; plan.wait_ns += writer.wait_ns;
      plan.writes += writer.writes; plan.syncs += writer.syncs;
    } catch (const std::bad_alloc&) { state.fail(too_much_memory); }
      catch (...) { state.fail("7z extraction failed."); }
  };
  // Joining is unconditional, including a partially created pool: no decoder
  // can outlive the plan or the parent's cancellation acknowledgement.
  try { for (unsigned i = 0; i < plan.workers; ++i) threads.emplace_back(run, i); }
  catch (...) { state.fail("Cannot start 7z decoder threads."); }
  for (auto& thread : threads) thread.join();
}
}
int run() {
  umask(0);
  Options options;
  if (!read_options(options)) { say("fail Invalid 7z worker request."); return 1; }
  say("ready");
  State state;
  Plan plan;
  std::mutex mutex;
  std::condition_variable wake;
  bool done = false;
  // A single input reader owns cancellation; all decoders consult atomic flags.
  std::thread alive;
  try { alive = std::thread([&] {
    std::unique_lock lock(mutex);
    unsigned ticks = 0;
    while (!wake.wait_for(lock, std::chrono::milliseconds(100), [&] { return done; })) {
      if (stop_requested()) state.cancelled = true;
      if (++ticks % 3 == 0) say("p " + std::to_string(state.written.load()));
      if (ticks % 10 == 0) say("k");
    }
  }); } catch (...) { say("fail Cannot start 7z progress thread."); return 1; }
  std::uint64_t claim_took = 0;
  std::uint64_t extract_took = 0;
  try {
    const auto error = claim(claim_took);
    if (!error.empty()) state.fail(error);
    else if (!state.stopped()) {
      const auto start = now_ns();
      extract(options, state, plan);
      extract_took = (now_ns() - start) / 1000000;
    }
  } catch (const std::bad_alloc&) { state.fail(too_much_memory); }
    catch (...) { state.fail("7z extraction failed."); }
  {
    std::lock_guard lock(mutex);
    done = true;
  }
  wake.notify_all(); alive.join();
  say("p " + std::to_string(state.written.load()));
  if (!state.stopped()) say("s");
  say("t threads=" + std::to_string(plan.workers * plan.decoder_threads) + " workers=" + std::to_string(plan.workers) +
      " decoder_threads=" + std::to_string(plan.decoder_threads) + " cores=" + std::to_string(cores()) +
      " workers_used=" + std::to_string(plan.used.load()) + " active_peak=" + std::to_string(plan.peak.load()) +
      " shared_mib=" + std::to_string(plan.shared_memory / mib) + " reader_mib=" + std::to_string(plan.reader_memory / mib) +
      " planned_mib=" + std::to_string(plan.planned_memory / mib) + " opening_mib=" + std::to_string(plan.opening_memory / mib) +
      " claim=" + std::to_string(claim_took) + "ms extract=" + std::to_string(extract_took) +
      "ms write=" + std::to_string(plan.write_ns.load() / 1000000) + "ms/" + std::to_string(plan.writes.load()) +
      " fsync=" + std::to_string(plan.sync_ns.load() / 1000000) + "ms/" + std::to_string(plan.syncs.load()) +
      " waited=" + std::to_string(plan.wait_ns.load() / 1000000) + "ms cpu=" + std::to_string(process_cpu_us() / 1000) + "ms");
  if (state.cancelled) say("cancelled");
  else say(state.error.empty() ? "ok" : "fail " + state.error);
  return state.error.empty() ? 0 : 1;
}
}
