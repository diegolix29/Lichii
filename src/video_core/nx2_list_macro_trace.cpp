// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <tuple>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/gpu_split.h"
#include "video_core/macro_turing.h"
#include "video_core/nx2_list_macro_trace.h"

namespace VideoCommon::ListMacroTrace {

namespace {

using Effects = Tegra::TuringMacroInterpreter::Effects;

constexpr u32 MacroMethods = 0xe00;
constexpr u32 SamplesPerMacro = 16;
constexpr size_t MaxDumps = 64;

enum class Content : u8 { Commands, MacroData, MethodData, Count };

constexpr std::array<const char*, static_cast<size_t>(Engine::Count)> engine_names{"3D", "compute",
                                                                                   "other"};

struct PathCounts {
    std::array<u64, static_cast<size_t>(Path::Count)> entries{};
    u64 waited_ns{};
    u64 words{};

    void Add(Path path, u64 ns, u32 count) {
        ++entries[static_cast<size_t>(path)];
        waited_ns += ns;
        words += count;
    }
    u64 Total() const {
        return entries[0] + entries[1] + entries[2];
    }
    u64 Waited() const {
        return entries[static_cast<size_t>(Path::Waited)];
    }
};

/// Words a macro call still had due when a GPU-written entry began and when it ended: the call's
/// parameters [total - before, total - after) came from the entry (all from its start if before
/// is Whole).
struct GpuRange {
    static constexpr u32 Whole = ~0U;
    u32 before;
    u32 after;
};

/// The GPU-written entry being parsed.
struct EntryState {
    bool active{};
    Path path{};
    u64 waited_ns{};
    const void* engine{}; ///< The engine whose call, begun before the entry, it continues
    u32 before{};         ///< That call's words due at the entry's start
    bool attributed{};    ///< A call took the entry's wait
};

/// A macro call that GPU-written entries fed and that has not run yet.
struct PendingCall {
    u32 split_starts{}; ///< GpuSplit::Starts then; one from an earlier start ran elsewhere
    std::vector<GpuRange> ranges;
    u64 entries{}; ///< Entries not yet counted for a macro, and their waits
    u64 waited{};
    u64 waited_ns{};
};

struct MacroWindow {
    u64 calls{};
    u64 entries{};
    u64 waited{};
    u64 waited_ns{};
    u64 continued{};
    u64 parameters{};
    u64 gpu_words{};
    u64 on_back{};
    std::array<u64, static_cast<size_t>(Forwarding::Count)> checks{};
    u64 other_maps{}; ///< Forwarding calls whose map differs from the macro's first one
};

struct Macro {
    Engine engine{};
    u64 hash{};
    u32 start{};
    u32 method{};
    u32 samples{};
    u32 mismatches_logged{};
    std::string dump; ///< Its dump file, empty if not written
    std::optional<std::vector<ForwardedWord>> map; ///< The first forwarding call's
    MacroWindow window;
};

struct CodeInfo {
    u64 hash{};
    Effects effects;
};

struct State {
    std::array<PathCounts, static_cast<size_t>(Content::Count)> contents{};
    std::map<std::tuple<Content, Engine, u32>, PathCounts> methods;
    std::map<std::pair<Engine, u64>, Macro> macros;
    std::map<std::tuple<const u32*, u64, u32>, CodeInfo> code;
    EntryState current;
    std::map<const void*, PendingCall> pending; ///< By engine
    size_t dumps{};
    u64 window_start{};
    Call call;
    Macro* call_macro{};
    bool call_sampled{};
    std::span<const u32> call_code;
    std::vector<u32> call_parameters;
    std::vector<u32> call_data_ram;
    std::vector<u32> call_gpu_words; ///< Indices of the parameters GPU-written entries held
};

State& Get() {
    static State state;
    return state;
}

constexpr std::array<const char*, static_cast<size_t>(Forwarding::Count)> forwarding_names{
    "forward", "steer", "transform", "data RAM", "unreproduced"};

std::filesystem::path DumpDirectory() {
    const char* const directory = std::getenv("EDEN_NX2_DEBUG_DIR");
    if (directory != nullptr && *directory != '\0') {
        return std::filesystem::path{directory};
    }
    return Common::FS::GetEdenPath(Common::FS::EdenPath::LogDir) / "list-macros";
}

std::string DumpCode(Engine engine, u64 hash, u32 method, u32 start, u64 generation,
                     std::span<const u32> code, const Effects& effects) {
    const auto directory = DumpDirectory();
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    const auto path =
        directory /
        fmt::format("list-macro-{}-{:016x}.txt", engine == Engine::Maxwell3D ? "3d" : "compute",
                    hash);
    std::ofstream file{path};
    if (!file) {
        LOG_ERROR(HW_GPU, "List macro trace: cannot write {}", path.string());
        return {};
    }
    file << fmt::format("{} macro {:016x} at {:#x} (method {:#x}, code generation {})\n",
                        engine_names[static_cast<size_t>(engine)], hash, start, method,
                        generation);
    file << fmt::format("Methods it can write: {:#x}{}; data RAM: {}\n",
                        fmt::join(effects.methods, ","),
                        effects.methods_known ? "" : " and computed ones",
                        effects.data_ram ? "used" : "not used");
    file << "Instructions it can run (code[pc*3], code[pc*3+1], code[pc*3+2], then the fields of "
            "TuringMacroInterpreter::Decode, lane 0/lane 1):\n";
    for (const u32 pc : effects.instructions) {
        const auto d = Tegra::TuringMacroInterpreter::Decode(
            {code[pc * 3 + 2], code[pc * 3 + 1], code[pc * 3]});
        file << fmt::format(
            "+{:#05x} pc {:#06x}: {:08x} {:08x} {:08x} end {} pred {}/{} op {}/{} dst {}/{} a "
            "{}/{} b {}/{} imm {:#x}/{:#x} method {}/{} emit {}/{}\n",
            pc - start, pc, code[pc * 3], code[pc * 3 + 1], code[pc * 3 + 2], d.end ? 1 : 0,
            d.pred_mode, d.pred_reg, d.op[0], d.op[1], d.dst[0], d.dst[1], d.src_a[0], d.src_a[1],
            d.src_b[0], d.src_b[1], d.imm[0], d.imm[1], d.out_method[0], d.out_method[1],
            d.out_emit[0], d.out_emit[1]);
    }
    file << "Calls fed by GPU-written entries:\n";
    LOG_INFO(HW_GPU, "List macro trace: wrote {} macro {:016x} to {}",
             engine_names[static_cast<size_t>(engine)], hash, path.string());
    return path.string();
}

std::string Pairs(std::span<const std::pair<u32, u32>> pairs) {
    std::string text;
    for (const auto& [address, value] : pairs) {
        text += fmt::format("{}{:#x}={:#x}", text.empty() ? "" : " ", address, value);
    }
    return text.empty() ? "none" : text;
}

std::string MapText(std::span<const ForwardedWord> map) {
    std::string text;
    for (const ForwardedWord& word : map) {
        text += fmt::format("{}word {} -> {:#x} (write {})", text.empty() ? "" : ", ", word.word,
                            word.method, word.write);
    }
    return text.empty() ? "no word written" : text;
}

void Verify(State& state, Macro& macro, const Call& call) {
    const auto check =
        CheckForwarding(state.call_code, macro.start, state.call_parameters, state.call_gpu_words,
                        call.reads, call.writes, state.call_data_ram);
    MacroWindow& w = macro.window;
    bool mismatch = check.result != Forwarding::Forwards;
    if (!mismatch) {
        if (!macro.map) {
            macro.map = check.map;
        } else if (*macro.map != check.map) {
            ++w.other_maps;
            mismatch = true;
        }
    }
    ++w.checks[static_cast<size_t>(check.result)];
    if (mismatch && macro.mismatches_logged < 16) {
        ++macro.mismatches_logged;
        LOG_WARNING(HW_GPU,
                    "List macro verify: {} macro {:016x} at {:#x} does not only forward its "
                    "GPU-written words {} ({}{}): parameters {:08x}, writes {}",
                    engine_names[static_cast<size_t>(macro.engine)], macro.hash, macro.start,
                    fmt::join(state.call_gpu_words, ","),
                    forwarding_names[static_cast<size_t>(check.result)],
                    check.result == Forwarding::Forwards
                        ? fmt::format(", {} instead of {}", MapText(check.map), MapText(*macro.map))
                        : std::string{},
                    fmt::join(state.call_parameters, " "), Pairs(call.writes));
    }
}

void MaybeLog(State& state) {
    const u64 now = Now();
    if (state.window_start == 0) {
        state.window_start = now;
        return;
    }
    if (now - state.window_start < 10'000'000'000ULL) {
        return;
    }
    const double seconds = static_cast<double>(now - state.window_start) / 1e9;
    state.window_start = now;
    const auto ms = [](u64 ns) { return static_cast<double>(ns) / 1e6; };
    PathCounts all;
    for (const PathCounts& counts : state.contents) {
        for (size_t i = 0; i < all.entries.size(); ++i) {
            all.entries[i] += counts.entries[i];
        }
        all.waited_ns += counts.waited_ns;
    }
    const auto& contents = state.contents;
    LOG_INFO(HW_GPU,
             "List macro trace in {:.1f} s: {} GPU-written entries, {} after the channel waited "
             "for the host GPU's copy ({:.2f} ms waited), {} from a copy at once, {} after a drain "
             "and flush; commands {} ({} waited), macro data {} ({} waited), other method data {} "
             "({} waited)",
             seconds, all.Total(), all.Waited(), ms(all.waited_ns),
             all.entries[static_cast<size_t>(Path::Copied)],
             all.entries[static_cast<size_t>(Path::Flushed)], contents[0].Total(),
             contents[0].Waited(), contents[1].Total(), contents[1].Waited(), contents[2].Total(),
             contents[2].Waited());
    std::vector<std::pair<const std::tuple<Content, Engine, u32>*, const PathCounts*>> methods;
    for (const auto& [key, counts] : state.methods) {
        methods.emplace_back(&key, &counts);
    }
    std::ranges::sort(methods, [](const auto& a, const auto& b) {
        return std::pair{a.second->waited_ns, a.second->Total()} >
               std::pair{b.second->waited_ns, b.second->Total()};
    });
    for (size_t i = 0; i < std::min<size_t>(methods.size(), 8); ++i) {
        const auto& [content, engine, method] = *methods[i].first;
        const PathCounts& counts = *methods[i].second;
        LOG_INFO(HW_GPU,
                 "List macro trace: {} of {} method {:#x}: {} entries ({} waited, {:.2f} ms), "
                 "{:.1f} words each",
                 content == Content::MacroData ? "macro data" : "data",
                 engine_names[static_cast<size_t>(engine)], method, counts.Total(),
                 counts.Waited(), ms(counts.waited_ns),
                 static_cast<double>(counts.words) / static_cast<double>(counts.Total()));
    }
    std::vector<Macro*> macros;
    for (auto& [key, macro] : state.macros) {
        if (macro.window.calls != 0) {
            macros.push_back(&macro);
        }
    }
    std::ranges::sort(macros, [](const Macro* a, const Macro* b) {
        return std::pair{a->window.waited_ns, a->window.calls} >
               std::pair{b->window.waited_ns, b->window.calls};
    });
    for (size_t i = 0; i < std::min<size_t>(macros.size(), 8); ++i) {
        const Macro& macro = *macros[i];
        const MacroWindow& w = macro.window;
        LOG_INFO(HW_GPU,
                 "List macro trace: {} macro {:016x} at {:#x} (method {:#x}): {} calls fed by "
                 "GPU-written entries, {} entries ({} waited, {:.2f} ms), {} continued a call begun "
                 "before the entry, {:.1f} parameters and {:.1f} GPU-written words a call, {} ran "
                 "on the back",
                 engine_names[static_cast<size_t>(macro.engine)], macro.hash, macro.start,
                 macro.method, w.calls, w.entries, w.waited, ms(w.waited_ns), w.continued,
                 static_cast<double>(w.parameters) / static_cast<double>(w.calls),
                 static_cast<double>(w.gpu_words) / static_cast<double>(w.calls), w.on_back);
        if (!g_verify) {
            continue;
        }
        const auto& c = w.checks;
        LOG_INFO(HW_GPU,
                 "List macro verify: {} macro {:016x}: {} calls only forward their GPU-written "
                 "words ({}), {} with another map, {} steer, {} transform, {} data RAM, {} "
                 "unreproduced",
                 engine_names[static_cast<size_t>(macro.engine)], macro.hash,
                 c[static_cast<size_t>(Forwarding::Forwards)] - w.other_maps,
                 macro.map ? MapText(*macro.map) : std::string{"no map yet"}, w.other_maps,
                 c[static_cast<size_t>(Forwarding::Steers)],
                 c[static_cast<size_t>(Forwarding::Transforms)],
                 c[static_cast<size_t>(Forwarding::DataRam)],
                 c[static_cast<size_t>(Forwarding::Unreproduced)]);
    }
    state.contents = {};
    state.methods.clear();
    for (auto& [key, macro] : state.macros) {
        macro.window = {};
    }
}

} // Anonymous namespace

u64 Now() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}

void BeginEntry(Path path, u64 waited_ns, Engine engine, const void* engine_object, u32 method,
                u32 method_count, u32 words) {
    State& state = Get();
    const Content content = method_count == 0 ? Content::Commands
                            : engine != Engine::Other && method >= MacroMethods
                                ? Content::MacroData
                                : Content::MethodData;
    state.contents[static_cast<size_t>(content)].Add(path, waited_ns, words);
    if (content != Content::Commands) {
        state.methods[{content, engine, method}].Add(path, waited_ns, words);
    }
    const bool macro_data = content == Content::MacroData;
    state.current = {
        .active = true,
        .path = path,
        .waited_ns = waited_ns,
        .engine = macro_data ? engine_object : nullptr,
        .before = macro_data ? method_count : 0,
        .attributed = false,
    };
}

void EndEntry(const void* macro_engine, u32 method_count) {
    State& state = Get();
    EntryState& entry = state.current;
    if (macro_engine != nullptr && entry.active) {
        PendingCall& call = state.pending[macro_engine];
        if (const u32 starts = GpuSplit::Get().Starts(); call.split_starts != starts) {
            call = {.split_starts = starts};
        }
        call.ranges.push_back({entry.engine == macro_engine ? entry.before : GpuRange::Whole,
                               method_count});
        if (!entry.attributed) {
            entry.attributed = true;
            ++call.entries;
            if (entry.path == Path::Waited) {
                ++call.waited;
                call.waited_ns += entry.waited_ns;
            }
        }
    }
    entry.active = false;
    MaybeLog(state);
}

Call* BeginCall(Engine engine, const void* engine_object, u32 method, std::span<const u32> code,
                u64 code_generation, u32 start, std::span<const u32> parameters,
                std::span<const u32> data_ram) {
    State& state = Get();
    EntryState& entry = state.current;
    const auto pending_it = state.pending.find(engine_object);
    std::optional<PendingCall> pending;
    if (pending_it != state.pending.end()) {
        if (pending_it->second.split_starts == GpuSplit::Get().Starts()) {
            pending = std::move(pending_it->second);
        }
        state.pending.erase(pending_it);
    }
    if (!entry.active && !pending) {
        return nullptr;
    }
    if (static_cast<size_t>(start) * 3 + 2 >= code.size()) {
        return nullptr;
    }
    // The parameters GPU-written entries held: those due when one began and not yet when it
    // ended; all of a call that began and ran inside the current one.
    std::vector<GpuRange> ranges = pending ? pending->ranges : std::vector<GpuRange>{};
    if (entry.active) {
        ranges.push_back({entry.engine == engine_object ? entry.before : GpuRange::Whole, 0});
        if (entry.engine == engine_object) {
            entry.engine = nullptr;
        }
    }
    const u32 total = static_cast<u32>(parameters.size());
    std::vector<u32> gpu_words;
    bool continued = false;
    for (const GpuRange& range : ranges) {
        continued |= range.before != GpuRange::Whole;
        const u32 before = std::min(range.before, total);
        for (u32 index = total - before; index < total - std::min(range.after, before); ++index) {
            gpu_words.push_back(index);
        }
    }
    std::ranges::sort(gpu_words);
    gpu_words.erase(std::unique(gpu_words.begin(), gpu_words.end()), gpu_words.end());
    if (state.code.size() > 4096) {
        state.code.clear();
    }
    auto [it, inserted] = state.code.try_emplace({code.data(), code_generation, start});
    CodeInfo& info = it->second;
    if (inserted) {
        info.effects = Tegra::TuringMacroInterpreter::AnalyzeEffects(code, start);
        info.hash = Tegra::TuringMacroInterpreter::CodeHash(code, start, info.effects);
    }
    Macro& macro = state.macros[{engine, info.hash}];
    macro.engine = engine;
    macro.hash = info.hash;
    macro.start = start;
    macro.method = method;
    if (macro.dump.empty() && state.dumps < MaxDumps) {
        ++state.dumps;
        macro.dump =
            DumpCode(engine, info.hash, method, start, code_generation, code, info.effects);
    }
    MacroWindow& w = macro.window;
    ++w.calls;
    w.parameters += total;
    w.gpu_words += gpu_words.size();
    w.continued += continued ? 1 : 0;
    if (pending) {
        w.entries += pending->entries;
        w.waited += pending->waited;
        w.waited_ns += pending->waited_ns;
    }
    if (entry.active && !entry.attributed) {
        entry.attributed = true;
        ++w.entries;
        if (entry.path == Path::Waited) {
            ++w.waited;
            w.waited_ns += entry.waited_ns;
        }
    }
    state.call_macro = &macro;
    state.call_sampled = !macro.dump.empty() && macro.samples < SamplesPerMacro;
    Call& call = state.call;
    call.capture = state.call_sampled || (g_verify && !gpu_words.empty());
    call.reads.clear();
    call.writes.clear();
    state.call_gpu_words = std::move(gpu_words);
    if (call.capture) {
        state.call_code = code;
        state.call_parameters.assign(parameters.begin(), parameters.end());
        if (g_verify) {
            state.call_data_ram.assign(data_ram.begin(), data_ram.end());
        }
    }
    return &call;
}

void EndCall(Call& call, bool on_back) {
    State& state = Get();
    Macro* const macro = state.call_macro;
    state.call_macro = nullptr;
    if (macro == nullptr) {
        return;
    }
    if (on_back) {
        ++macro->window.on_back;
    } else if (g_verify && call.capture && !state.call_gpu_words.empty()) {
        Verify(state, *macro, call);
    }
    if (!state.call_sampled) {
        return;
    }
    ++macro->samples;
    std::ofstream file{macro->dump, std::ios::app};
    file << fmt::format(
        "call {}: method {:#x}, {} parameters {:08x}, GPU-written: {}\n  registers read: {}\n  "
        "methods written: {}\n",
        macro->samples, macro->method, state.call_parameters.size(),
        fmt::join(state.call_parameters, " "), fmt::join(state.call_gpu_words, ","),
        Pairs(call.reads), on_back ? std::string{"(ran on the back)"} : Pairs(call.writes));
}

ForwardCheck CheckForwarding(std::span<const u32> code, u32 start, std::span<const u32> parameters,
                             std::span<const u32> gpu_words,
                             std::span<const std::pair<u32, u32>> reads,
                             std::span<const std::pair<u32, u32>> writes,
                             std::span<const u32> data_ram) {
    struct Run {
        bool success{};
        std::vector<std::pair<u32, u32>> writes;
        std::vector<std::pair<u32, u32>> data_ram_writes;
    };
    static Tegra::TuringMacroInterpreter interpreter;
    // A tag the macro loops on would otherwise run a million instructions.
    interpreter.step_limit = 1U << 16;
    const auto run = [&](std::span<const u32> words, bool tagged) {
        Run result;
        interpreter.dram.fill(0);
        std::copy_n(data_ram.begin(), std::min(data_ram.size(), interpreter.dram.size()),
                    interpreter.dram.begin());
        interpreter.active_dram = &interpreter.dram;
        std::vector<u32> fifo(words.begin(), words.end());
        size_t next_read = 0;
        bool reads_match = true;
        const auto executed = interpreter.Execute(
            code, start, fifo,
            [&](u32 address) {
                if (next_read >= reads.size() || reads[next_read].first != address) {
                    reads_match = false;
                    return 0U;
                }
                // The engines apply a macro's writes before its next read: a register the call
                // wrote reads back what this run wrote (which must be what the call read).
                const auto written = std::ranges::find(result.writes.rbegin(),
                                                       result.writes.rend(), address,
                                                       &std::pair<u32, u32>::first);
                if (written == result.writes.rend()) {
                    return reads[next_read++].second;
                }
                if (!tagged && written->second != reads[next_read].second) {
                    reads_match = false;
                }
                ++next_read;
                return written->second;
            },
            [&](u32 method, u32 value) {
                if (method >= MacroMethods) {
                    return false;
                }
                result.writes.emplace_back(method, value);
                return true;
            });
        result.success = executed.success && reads_match && next_read == reads.size();
        result.data_ram_writes = interpreter.data_ram_writes;
        return result;
    };
    const Run real = run(parameters, false);
    if (!real.success || !std::ranges::equal(real.writes, writes)) {
        return {Forwarding::Unreproduced, {}};
    }
    std::vector<u32> tags_a(parameters.begin(), parameters.end());
    std::vector<u32> tags_b = tags_a;
    for (const u32 k : gpu_words) {
        if (k >= parameters.size()) {
            return {Forwarding::Unreproduced, {}};
        }
        tags_a[k] = 0x5a001357U | (k << 16);
        tags_b[k] = 0x25002468U | (k << 16);
    }
    const Run a = run(tags_a, true);
    const Run b = run(tags_b, true);
    const auto same_methods = [&real](const Run& tagged) {
        return tagged.success && tagged.writes.size() == real.writes.size() &&
               std::ranges::equal(tagged.writes, real.writes,
                                  [](const auto& x, const auto& y) { return x.first == y.first; });
    };
    if (!same_methods(a) || !same_methods(b)) {
        return {Forwarding::Steers, {}};
    }
    ForwardCheck check{Forwarding::Forwards, {}};
    for (size_t i = 0; i < real.writes.size(); ++i) {
        const u32 value = real.writes[i].second;
        if (a.writes[i].second == value && b.writes[i].second == value) {
            continue;
        }
        const auto forwarded = std::ranges::find_if(gpu_words, [&](u32 k) {
            return a.writes[i].second == tags_a[k] && b.writes[i].second == tags_b[k] &&
                   value == parameters[k];
        });
        if (forwarded == gpu_words.end()) {
            return {Forwarding::Transforms, {}};
        }
        check.map.push_back({*forwarded, static_cast<u32>(i), real.writes[i].first});
    }
    if (a.data_ram_writes != real.data_ram_writes || b.data_ram_writes != real.data_ram_writes) {
        return {Forwarding::DataRam, {}};
    }
    return check;
}

} // namespace VideoCommon::ListMacroTrace
