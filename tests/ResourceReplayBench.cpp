#include "graphics/host_gpu/renderer/pipeline/resourceReplay.h"
#include "graphics/host_gpu/renderer/pipeline/resourceWorker.h"
#include "graphics/shader/recompiler/ir/passes/SrtNative.h"
#include "common/liveSwitches.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <thread>
#include <atomic>
#include <fstream>

namespace {
using namespace Libs::Graphics;
namespace IR = ShaderRecompiler::IR;
struct Memory {
	using Key = std::pair<uint64_t, size_t>;
	struct Value { std::vector<uint32_t> words; bool ok; };
	std::map<Key, Value> data;
	struct Region { uint64_t base; std::vector<uint32_t> words; };
	std::vector<Region> regions;
	IR::SrtReadSpan fast;
	bool publish_fast = false;
	bool unexpected = false;
	bool Initialize(const ResourceReplay::ReadLog& log) {
		std::map<uint64_t, uint32_t> known;
		for (const auto& read: log.reads) {
			Value value {{}, read.ok};
			if (read.ok) value.words.assign(log.words.begin() + read.first,
			                                log.words.begin() + read.first + read.count);
			auto [it, inserted] = data.try_emplace(Key {read.address, read.count}, value);
			if (!inserted && (it->second.ok != value.ok || it->second.words != value.words))
				return false; // Memory changed during capture: no deterministic replay is possible.
			if (read.ok) for (uint32_t i = 0; i < read.count; ++i) {
				const auto word = log.words[read.first + i];
				auto [old, fresh] = known.try_emplace(read.address + uint64_t(i) * 4, word);
				if (!fresh && old->second != word) return false;
			}
		}
		for (const auto& [address, word]: known) {
			if (regions.empty() || regions.back().base + regions.back().words.size() * 4 != address)
				regions.push_back({address, {}});
			regions.back().words.push_back(word);
		}
		return true;
	}
	static bool Read(void* userdata, uint64_t address, std::span<uint32_t> values) {
		auto& memory = *static_cast<Memory*>(userdata);
		const auto found = memory.data.find({address, values.size()});
		if (found == memory.data.end()) { memory.unexpected = true; return false; }
		if (!found->second.ok) return false;
		std::memcpy(values.data(), found->second.words.data(), values.size_bytes());
		if (memory.publish_fast) {
		memory.fast = {};
		for (const auto& region: memory.regions) {
			if (address >= region.base && address - region.base < region.words.size() * 4) {
				memory.fast = {region.base, reinterpret_cast<const uint8_t*>(region.words.data()),
				    region.words.size() * 4 - 4};
				break;
			}
		}
		}
		return true;
	}
};
bool SameReadRanges(const IR::ResourceSnapshot& a, const IR::ResourceSnapshot& b) {
	auto left = a.specialization_reads, right = b.specialization_reads;
	std::ranges::sort(left); std::ranges::sort(right);
	left.erase(std::unique(left.begin(), left.end()), left.end());
	right.erase(std::unique(right.begin(), right.end()), right.end());
	return left == right;
}
bool Same(const IR::ResourceSnapshot& a, const IR::ResourceSnapshot& b,
          std::span<const uint8_t> gpu_slots = {}, bool eager = false) {
	bool same_flat = a.flattened_srt.size() == b.flattened_srt.size();
	if (same_flat) for (size_t i = 0; i < a.flattened_srt.size(); ++i)
		if ((gpu_slots.empty() || gpu_slots[i] != 0) && a.flattened_srt[i] != b.flattened_srt[i]) same_flat = false;
	const bool same_reads = eager ? std::ranges::all_of(b.specialization_reads, [&](const auto& range) {
		return std::ranges::find(a.specialization_reads, range) != a.specialization_reads.end();
	}) : SameReadRanges(a, b);
	return a.buffer_write_extents == b.buffer_write_extents && a.bindless_heaps == b.bindless_heaps &&
	       a.bindless_sampler_heaps == b.bindless_sampler_heaps && a.buffers == b.buffers &&
	       a.images == b.images && a.samplers == b.samplers && same_flat &&
	       a.user_data == b.user_data && same_reads &&
	       a.uniform_fill == b.uniform_fill;
}
void Difference(const IR::ResourceSnapshot& a, const IR::ResourceSnapshot& b,
                const IR::ResourceSpecialization& sa, const IR::ResourceSpecialization& sb,
                int mode, bool expected, bool actual) {
	std::fprintf(stderr, " mode %d expected_ok=%d actual_ok=%d differences:", mode, expected, actual);
#define DIFF(field) if (a.field != b.field) std::fprintf(stderr, " %s", #field)
	DIFF(buffer_write_extents); DIFF(bindless_heaps); DIFF(bindless_sampler_heaps);
	DIFF(buffers); DIFF(images); DIFF(samplers); DIFF(flattened_srt); DIFF(user_data);
	DIFF(specialization_reads); DIFF(uniform_fill);
#undef DIFF
	if (sa != sb) std::fprintf(stderr, " specialization");
	std::fprintf(stderr, "\n");
}
}
namespace Common {
int DbgExitHandler(const char* file, int line, std::string_view text) {
	std::fprintf(stderr, "EXIT %s:%d: %.*s\n", file, line, int(text.size()), text.data());
	std::abort();
}
int DbgExitHandler(const char* file, int line, fmt::text_style, std::string_view text) {
	return DbgExitHandler(file, line, text);
}
int DbgExitIfHandler(const char* expression, const char* file, int line) {
	std::fprintf(stderr, "EXIT_IF(%s) %s:%d\n", expression, file, line); std::abort();
}
void DbgExit(int) { std::abort(); }
}
int main(int argc, char** argv) {
	if (argc == 3 && std::string_view(argv[1]) == "--audit-plan") {
		std::ifstream input(argv[2], std::ios::binary);
		std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
		IR::ResourcePlan plan;
		if (!input || !ProgramDiskCache::DeserializePlan(bytes, plan)) return 2;
		std::printf("Plan %016llx stage=%u roots=%zu buffers=%zu images=%zu samplers=%zu\n",
		    static_cast<unsigned long long>(plan.shader_hash), static_cast<unsigned>(plan.stage),
		    plan.srt_reads.size(), plan.info.buffers.size(), plan.info.images.size(), plan.info.samplers.size());
		for (size_t i=0; i<plan.info.buffers.size(); ++i) {
			const auto& b=plan.info.buffers[i];
			std::printf("buffer=%zu source=%u read=%u write=%u atomic=%u formatted=%u scalar=%u gpu_records=%u\n",
			    i,b.source,b.read,b.written,b.atomic,b.formatted,b.scalar,b.gpu_records);
			if (b.source >= plan.descriptor_sources.size()) return 3;
			const auto& source=plan.descriptor_sources[b.source];
			for (uint32_t w=0; w<source.dword_count; ++w) {
				const auto value=source.dwords[w].Resolve();
				if (const auto* inst=value.TryInstruction()) {
					std::printf("  word=%u op=%s",w,std::string(IR::ValueOpcodeName(inst->GetOpcode())).c_str());
					if (inst->GetOpcode()==IR::ValueOpcode::ReadConst && inst->NumArgs()>1 && inst->Arg(1).IsImmediate())
						std::printf(" flat=%u",inst->Arg(1).U32());
					std::puts("");
				} else std::printf("  word=%u immediate\n",w);
			}
		}
		return 0;
	}
	if (argc < 2) { std::puts("resource_replay_bench <capture-directory> [iterations=500] [eager]"); return 2; }
	const int iterations = argc > 2 ? std::clamp(std::atoi(argv[2]), 1, 100000) : 500;
	const bool compare_eager = argc > 3 && std::string_view(argv[3]) == "eager";
	const bool parallel = argc > 3 && (std::string_view(argv[3]) == "parallel" || std::string_view(argv[3])=="buffer-group-parallel");
	const bool worker_mode = argc > 3 && std::string_view(argv[3]) == "worker";
	const bool direct_mode = argc > 3 && std::string_view(argv[3]) == "direct";
	const bool views_mode = argc > 3 && std::string_view(argv[3]) == "views";
	const bool combo_mode = argc > 3 && std::string_view(argv[3]) == "combo";
	const bool capture_span_mode = argc > 3 && std::string_view(argv[3]) == "capture-spans";
	const bool capture_bulk_mode = argc > 3 && std::string_view(argv[3]) == "capture-bulk";
	const bool buffer_group_mode=argc>3 && (std::string_view(argv[3])=="buffer-group" || std::string_view(argv[3])=="buffer-group-parallel");
	const bool canonical_mode = buffer_group_mode || (argc > 3 && std::string_view(argv[3]) == "canonical");
	const bool bulk_mode = argc > 3 && (std::string_view(argv[3]) == "bulk" || combo_mode || capture_span_mode || capture_bulk_mode || canonical_mode);
	const bool srt_alias_only = argc > 3 && (std::string_view(argv[3]) == "aliases-srt" || combo_mode || capture_span_mode || capture_bulk_mode || canonical_mode);
	const bool alias_mode = argc > 3 && (std::string_view(argv[3]) == "aliases" || srt_alias_only);
	const bool special_mode = direct_mode || views_mode || bulk_mode || alias_mode;
	auto& bulk = Common::LiveSwitches::Get("KYTY_SRT_BULK_READS", 0);
	auto& views = Common::LiveSwitches::Get("KYTY_SRT_BUFFER_VIEWS", 0);
	const bool describe = argc > 3 && std::string_view(argv[3]) == "describe";
	auto& direct = Common::LiveSwitches::Get("KYTY_SRT_DIRECT_READ", 0);
	std::unique_ptr<ResourceEvaluationWorker> evaluator;
	if (worker_mode) evaluator = std::make_unique<ResourceEvaluationWorker>();
	auto& batch = Common::LiveSwitches::Get("KYTY_SRT_NATIVE_BATCH", 0);
	auto& buffer_group=Common::LiveSwitches::Get("KYTY_SRT_BUFFER_GROUP",0);
	buffer_group.store(0);
	unsigned valid = 0, rejected = 0, failed = 0;
	double total[2] {};
	std::vector<std::filesystem::path> paths;
	for (const auto& item: std::filesystem::directory_iterator(argv[1]))
		if (item.path().extension() == ".srt-replay") paths.push_back(item.path());
	std::ranges::sort(paths);
	std::puts("capture,shader,reads,flat_words,baseline_us,batch_us,ratio,status,buffers,images,samplers,gpu_flat_words,stage,bulk_roots,buffer_views");
	for (const auto& path: paths) {
		ResourceReplay::Capture capture;
		Memory memory;
		IR::ResourcePlan reference, candidate;
		if (!ResourceReplay::Load(path, capture) || !memory.Initialize(capture.log) ||
		    !ProgramDiskCache::DeserializePlan(capture.plan, reference) ||
		    !ProgramDiskCache::DeserializePlan(capture.plan, candidate)) { rejected++; continue; }
		memory.publish_fast = direct_mode || bulk_mode;
		if (describe && candidate.srt_reads.size() >= 128) {
			std::map<std::string, unsigned> kinds;
			unsigned constant_offset = 0;
			std::vector<const IR::Inst*> handles, distinct_arguments;
			for (const auto& read: candidate.srt_reads) {
				const auto* inst = read.value.Resolve().TryInstruction();
				if (inst == nullptr) { ++kinds["immediate"]; continue; }
				++kinds[std::string(IR::ValueOpcodeName(inst->GetOpcode()))];
				if (inst->NumArgs() > 1 && inst->Arg(1).Resolve().IsImmediate()) ++constant_offset;
				if (inst->GetOpcode() == IR::ValueOpcode::ReadConstBuffer) {
					const auto* handle = inst->Arg(0).Resolve().TryInstruction();
					if (handle != nullptr && handle->NumArgs() == 4 && std::ranges::find(handles, handle) == handles.end()) {
						handles.push_back(handle);
						const bool same = std::ranges::any_of(distinct_arguments, [&](const auto* prior) {
							for (size_t i = 0; i < 4; ++i) if (prior->Arg(i).Resolve() != handle->Arg(i).Resolve()) return false;
							return true;
						});
						if (!same) distinct_arguments.push_back(handle);
					}
				}
			}
			std::fprintf(stderr, "Plan %016llx stage=%u roots=%zu nodes=%zu constant-offset=%u:",
			    static_cast<unsigned long long>(candidate.shader_hash), static_cast<unsigned>(candidate.stage),
			    candidate.srt_reads.size(), candidate.value_storage.size(), constant_offset);
			for (const auto& [kind, count]: kinds) std::fprintf(stderr, " %s=%u", kind.c_str(), count);
			std::fprintf(stderr, " handles=%zu different-arguments=%zu\n", handles.size(), distinct_arguments.size());
		}
		IR::SrtRuntime runtime {.user_data = capture.user_data, .shader_base = capture.shader_base,
		    .read_memory = Memory::Read, .userdata = &memory, .read_specialization_memory = Memory::Read,
		    .float_image_atomics = capture.float_image_atomics,
		    .workgroup_count = capture.workgroup_count, .workgroup_size = capture.workgroup_size,
		    .fast_read = direct_mode || bulk_mode ? &memory.fast : nullptr,
		    .fast_specialization_read = direct_mode || bulk_mode ? &memory.fast : nullptr};
		IR::ResourceSnapshot a, b;
		IR::ResourceSpecialization sa, sb;
		if (compare_eager) reference.gpu_flat_slots.clear();
		reference.native_code = IR::SrtNativeCode::Compile(reference, canonical_mode, !canonical_mode,buffer_group_mode);
		candidate.native_code = IR::SrtNativeCode::Compile(candidate, alias_mode, !srt_alias_only, canonical_mode);
		batch.store(special_mode ? 1 : 0, std::memory_order_relaxed);
		direct.store(0, std::memory_order_relaxed);
		views.store(0, std::memory_order_relaxed);
		bulk.store(0, std::memory_order_relaxed);
		bool expected = false;
		for (int warm = 0; warm < 4; ++warm) expected = IR::MaterializeResources(reference, runtime, a, sa);
		bool correct = expected && candidate.native_code != nullptr && reference.native_code != nullptr;
		for (int mode: {0, 1, 0, 1}) {
			buffer_group.store(buffer_group_mode ? mode : 0);
			runtime.capture_read_spans = canonical_mode || capture_bulk_mode || (capture_span_mode && mode != 0);
			runtime.capture_bulk_reads = canonical_mode || (capture_bulk_mode && mode != 0);
			batch.store(special_mode ? 1 : mode, std::memory_order_relaxed);
			direct.store(direct_mode ? mode : 0, std::memory_order_relaxed);
			views.store(views_mode ? mode : 0, std::memory_order_relaxed);
			bulk.store(bulk_mode ? (capture_span_mode || capture_bulk_mode || canonical_mode ? 1 : mode) : 0, std::memory_order_relaxed);
			const bool actual = IR::MaterializeResources(candidate, runtime, b, sb);
			const bool same = Same(a, b, candidate.gpu_flat_slots, compare_eager);
			if (actual != expected || !same || sa != sb)
				Difference(a, b, sa, sb, mode, expected, actual);
			correct &= actual && same && sa == sb;
		}
		if (memory.unexpected) { rejected++; continue; }
		if (!correct) { failed++; std::fprintf(stderr, "Replay mismatch: %s\n", path.string().c_str()); continue; }
		if (worker_mode) {
			if (!IR::PrepareResourceEvaluation(candidate) || capture.user_data.size() > 40) { failed++; continue; }
			auto& job = evaluator->job;
			IR::ResourceEvaluationSession session;
			job.plan = &candidate; job.session = &session;
			job.user_data_count = static_cast<uint32_t>(capture.user_data.size());
			std::ranges::copy(capture.user_data, job.user_data.begin());
			job.shader_base = capture.shader_base;
			job.float_image_atomics = capture.float_image_atomics;
			job.workgroup_count = capture.workgroup_count; job.workgroup_size = capture.workgroup_size;
			std::map<uint64_t, ResourceMemorySnapshot::Page> pages;
			for (const auto& read: capture.log.reads) {
				if (!read.ok) { correct = false; break; }
				for (uint32_t word = 0; word < read.count; ++word) {
					for (uint32_t byte = 0; byte < 4; ++byte) {
						const auto address = read.address + uint64_t(word) * 4 + byte;
						auto& page = pages[address & ~uint64_t(4095)];
						page.address = address & ~uint64_t(4095);
						page.bytes[address & 4095] = uint8_t(capture.log.words[read.first + word] >> (byte * 8));
					}
				}
			}
			if (!correct) { rejected++; continue; }
			job.memory.pages.clear();
			for (auto& [address, page]: pages) job.memory.pages.push_back(std::move(page));
			batch.store(1, std::memory_order_relaxed);
			for (int trial = 0; trial < 25; ++trial) {
				evaluator->Submit(); evaluator->Wait();
				if (!job.ok || !Same(a, job.resources, candidate.gpu_flat_slots) || job.specialization != sa) {
					correct = false; break;
				}
			}
			if (!correct) { failed++; std::fprintf(stderr, "Worker replay mismatch: %s\n", path.string().c_str()); continue; }
		}
		if (parallel) {
			if (!IR::PrepareResourceEvaluation(candidate)) { failed++; continue; }
			batch.store(1, std::memory_order_relaxed);
			std::atomic_bool mismatch {false};
			std::vector<std::jthread> workers;
			for (int worker = 0; worker < 4; ++worker) workers.emplace_back([&] {
				Memory local_memory = memory;
				IR::ResourceEvaluationSession session;
				auto local_runtime = runtime;
				local_runtime.userdata = &local_memory;
				local_runtime.evaluation_session = &session;
				IR::ResourceSnapshot actual;
				IR::ResourceSpecialization special;
				for (int i = 0; i < 25; ++i) {
					if (!IR::MaterializeResources(candidate, local_runtime, actual, special) ||
					    !Same(a, actual, candidate.gpu_flat_slots) || special != sa || local_memory.unexpected)
						mismatch.store(true, std::memory_order_relaxed);
				}
			});
			for (auto& worker: workers) worker.join();
			if (mismatch.load()) { failed++; std::fprintf(stderr, "Parallel replay mismatch: %s\n", path.string().c_str()); continue; }
		}
		double micros[2] {};
		std::array<unsigned,2> measurements {};
		const auto order=buffer_group_mode ? std::vector<int>{0,1,1,0} : std::vector<int>{0,1};
		for (int mode: order) {
			buffer_group.store(buffer_group_mode ? mode : 0);
			runtime.capture_read_spans = canonical_mode || capture_bulk_mode || (capture_span_mode && mode != 0);
			runtime.capture_bulk_reads = canonical_mode || (capture_bulk_mode && mode != 0);
			batch.store(special_mode ? 1 : mode, std::memory_order_relaxed);
			direct.store(direct_mode ? mode : 0, std::memory_order_relaxed);
			views.store(views_mode ? mode : 0, std::memory_order_relaxed);
			bulk.store(bulk_mode ? (capture_span_mode || capture_bulk_mode || canonical_mode ? 1 : mode) : 0, std::memory_order_relaxed);
			const auto& measured = !capture_span_mode && !capture_bulk_mode && (compare_eager || alias_mode) && mode == 0 ? reference : candidate;
			std::array<double, 3> times {};
			for (auto& time: times) {
				const auto begin = std::chrono::steady_clock::now();
				for (int i = 0; i < iterations; i++)
					if (!IR::MaterializeResources(measured, runtime, b, sb)) { correct = false; break; }
				time = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / iterations;
			}
			std::ranges::sort(times);
			micros[mode] += times[1]; ++measurements[mode];
		}
		for (unsigned mode=0;mode<2;++mode){micros[mode]/=measurements[mode];total[mode]+=micros[mode];}
		if (!correct || memory.unexpected) failed++;
		else valid++;
		std::printf("%s,%016llx,%zu,%zu,%.4f,%.4f,%.4f,%s,%zu,%zu,%zu,%zu,%u,%u,%u\n", path.filename().string().c_str(),
		    static_cast<unsigned long long>(candidate.shader_hash), capture.log.reads.size(), b.flattened_srt.size(),
		    micros[0], micros[1], micros[0] / micros[1], correct ? "ok" : "failed",
		    candidate.info.buffers.size(), candidate.info.images.size(), candidate.info.samplers.size(),
		    candidate.gpu_flat_slots.empty() ? candidate.srt_reads.size()
		        : static_cast<size_t>(std::count(candidate.gpu_flat_slots.begin(), candidate.gpu_flat_slots.end(), 1u)),
		    static_cast<unsigned>(candidate.stage), candidate.native_code->BulkReadRoots(),
		    candidate.native_code->BufferViewCount());
	}
	std::fprintf(stderr, "Replay: %u valid, %u rejected (unstable/incomplete capture), %u mismatches; "
	    "equal-weight baseline %.3f us, batch %.3f us, ratio %.3f. This is CPU replay, not game FPS.\n",
	    valid, rejected, failed, total[0], total[1], total[0] / std::max(total[1], 0.00001));
	if (parallel) std::fprintf(stderr, "Parallel replay: %u plans, four independent sessions, 25 evaluations per worker.\n", valid);
	if (worker_mode) std::fprintf(stderr, "Worker replay: %u plans, 25 submitted/joined snapshots per plan.\n", valid);
	if (direct_mode) std::fprintf(stderr, "Direct read replay: both modes use native batches; spans contain only captured known words.\n");
	if (views_mode) std::fprintf(stderr, "Buffer views replay: both modes use native batches, direct read spans disabled.\n");
	if (capture_span_mode) std::fprintf(stderr, "Capture spans: same alias code and bulk=1 in both modes; only recorded reader span reuse changes.\n");
	if (capture_bulk_mode) std::fprintf(stderr, "Capture bulk: same alias code, bulk=1 and recorded scalar spans in both modes; only captured bulk batches change.\n");
	if (canonical_mode && !buffer_group_mode) std::fprintf(stderr, "Canonical views: aliases and recorded bulk spans enabled in both modes; only proven argument identity changes.\n");
	if (buffer_group_mode) std::fprintf(stderr,"Buffer group: both modes use canonical alias batches and captured bulk spans; only readonly buffer C ABI grouping changes.\n");
	if (bulk_mode && !capture_span_mode && !capture_bulk_mode && !canonical_mode) std::fprintf(stderr, "Bulk read replay: both modes use native batches; enabled mode includes direct spans and buffer views.\n");
	if (alias_mode && !combo_mode && !capture_span_mode && !capture_bulk_mode && !canonical_mode) std::fprintf(stderr, "Value aliases replay: separate native code variants, batches enabled, views/direct/bulk disabled.\n");
	if (combo_mode) std::fprintf(stderr, "Combined replay: classic batches vs grouped reads and SRT aliases, separate native code variants.\n");
	return failed != 0 || valid == 0 ? 1 : 0;
}

#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
