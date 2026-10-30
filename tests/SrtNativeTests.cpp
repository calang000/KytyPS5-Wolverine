// The native SRT walker (SrtNative.cpp) against the interpreter (SrtWalker.cpp): the same plan is
// built twice, one copy stays on the interpreter and the other is compiled, and every expression
// is evaluated by both for many inputs. Results, including failures, must be identical.

#include "graphics/shader/recompiler/ir/ShaderIR.h"
#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include "graphics/shader/recompiler/ir/passes/SrtNative.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"
#include "common/liveSwitches.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <vector>
#include <thread>
#include <atomic>

namespace {

using namespace Libs::Graphics::ShaderRecompiler::IR;

void Check(bool value, const char* text) {
	if (!value) {
		std::fprintf(stderr, "SrtNativeTests: failed: %s\n", text);
		std::abort();
	}
}

constexpr uint32_t UserDataCount = 12;
constexpr uint64_t MemoryBase    = 0x10000;
constexpr uint32_t MemoryDwords  = 64;

// Guest memory as the tests' readers see it: MemoryDwords dwords at MemoryBase, with a different
// pattern for the clean (specialization) reader so that routing mistakes show.
struct TestMemory {
	std::array<uint32_t, MemoryDwords> ordinary {};
	std::array<uint32_t, MemoryDwords> clean {};
	uint32_t                           reads = 0;
	SrtReadSpan fast_ordinary, fast_clean;
};

bool ReadFrom(const std::array<uint32_t, MemoryDwords>& words, uint64_t address,
              std::span<uint32_t> values) {
	if (address < MemoryBase || (address & 3u) != 0u ||
	    address + values.size() * 4u > MemoryBase + MemoryDwords * 4u) {
		return false;
	}
	std::memcpy(values.data(), &words[(address - MemoryBase) / 4u], values.size_bytes());
	return true;
}

bool ReadOrdinary(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& memory = *static_cast<TestMemory*>(userdata);
	memory.reads++;
	return ReadFrom(memory.ordinary, address, values);
}

bool ReadClean(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& memory = *static_cast<TestMemory*>(userdata);
	memory.reads++;
	return ReadFrom(memory.clean, address, values);
}

bool ReadOrdinarySpan(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& memory = *static_cast<TestMemory*>(userdata);
	const bool ok = ReadOrdinary(userdata, address, values);
	memory.fast_ordinary = ok ? SrtReadSpan {MemoryBase,
	    reinterpret_cast<const uint8_t*>(memory.ordinary.data()), sizeof(memory.ordinary) - 4} : SrtReadSpan {};
	return ok;
}
bool ReadCleanSpan(void* userdata, uint64_t address, std::span<uint32_t> values) {
	auto& memory = *static_cast<TestMemory*>(userdata);
	const bool ok = ReadClean(userdata, address, values);
	memory.fast_clean = ok ? SrtReadSpan {MemoryBase,
	    reinterpret_cast<const uint8_t*>(memory.clean.data()), sizeof(memory.clean) - 4} : SrtReadSpan {};
	return ok;
}

Block& AddValueBlock(Program& program) {
	auto  block  = std::make_unique<Block>();
	auto* result = block.get();
	program.blocks.push_back(result);
	program.block_info.push_back({.id = 0});
	program.block_storage.push_back(std::move(block));
	return *result;
}

// Every expression the native compiler handles, over user data s0..s11:
// s0..s3 arbitrary values, s4..s7 small counts (offsets, widths), s8/s9 an address, s10/s11 more.
ResourcePlan BuildPlan() {
	Program program;
	program.stage                      = Libs::Graphics::ShaderType::Compute;
	program.srt_plan_complete          = true;
	program.resource_tracking_complete = true;
	auto& b                            = AddValueBlock(program);

	std::vector<Value> exprs;
	auto               u = [&](uint32_t reg) {
        return Value(&b.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(reg))}));
	};
	auto op = [&](ValueOpcode opcode, std::initializer_list<Value> args) {
		return Value(&b.AppendNewInst(opcode, args));
	};
	auto add = [&](Value value) { exprs.push_back(value); };
	auto lo  = [&](Value wide) { return op(ValueOpcode::CompositeExtractU64, {wide, Value(0u)}); };
	auto hi  = [&](Value wide) { return op(ValueOpcode::CompositeExtractU64, {wide, Value(1u)}); };

	const auto s0 = u(0), s1 = u(1), s2 = u(2), s3 = u(3);
	const auto s4 = u(4), s5 = u(5), s6 = u(6), s7 = u(7);
	const auto s8 = u(8), s9 = u(9), s10 = u(10), s11 = u(11);
	const auto w0 = op(ValueOpcode::CompositeConstructU64, {s0, s1});
	const auto w1 = op(ValueOpcode::CompositeConstructU64, {s2, s3});

	add(s0), add(s11);
	add(op(ValueOpcode::IAdd32, {s0, s1}));
	add(op(ValueOpcode::ISub32, {s0, s1}));
	add(op(ValueOpcode::IMul32, {s0, s1}));
	add(op(ValueOpcode::UMulHi, {s0, s1}));
	add(op(ValueOpcode::UMin32, {s0, s1}));
	add(op(ValueOpcode::IAdd32, {s0, Value(0xfffffff0u)}));
	for (auto opcode: {ValueOpcode::IAdd64, ValueOpcode::ISub64, ValueOpcode::IMul64,
	                   ValueOpcode::BitwiseAnd64}) {
		const auto r = op(opcode, {w0, w1});
		add(lo(r)), add(hi(r));
	}
	for (auto opcode: {ValueOpcode::ShiftLeftLogical64, ValueOpcode::ShiftRightLogical64,
	                   ValueOpcode::ShiftRightArithmetic64}) {
		const auto r = op(opcode, {w0, s4});
		add(lo(r)), add(hi(r));
	}
	for (auto opcode: {ValueOpcode::BitwiseAnd32, ValueOpcode::BitwiseOr32, ValueOpcode::BitwiseXor32,
	                   ValueOpcode::ShiftLeftLogical32, ValueOpcode::ShiftRightLogical32,
	                   ValueOpcode::ShiftRightArithmetic32, ValueOpcode::IEqual32,
	                   ValueOpcode::INotEqual32, ValueOpcode::ULessThan32, ValueOpcode::UGreaterThan32,
	                   ValueOpcode::ULessThanEqual32, ValueOpcode::SGreaterThanEqual32}) {
		add(op(opcode, {s0, s1}));
		add(op(opcode, {s2, s4}));
	}
	add(op(ValueOpcode::BitwiseNot32, {s0}));
	for (auto opcode: {ValueOpcode::LogicalAnd, ValueOpcode::LogicalOr, ValueOpcode::LogicalXor}) {
		add(op(opcode, {op(ValueOpcode::ULessThan32, {s0, s1}), op(ValueOpcode::IEqual32, {s2, s3})}));
	}
	add(op(ValueOpcode::LogicalNot, {op(ValueOpcode::ULessThan32, {s0, s1})}));
	add(op(ValueOpcode::BitFieldUExtract, {s0, s4, s5}));
	add(op(ValueOpcode::BitFieldSExtract, {s0, s4, s5}));
	add(op(ValueOpcode::BitFieldUExtract, {s1, Value(8u), Value(8u)}));
	add(op(ValueOpcode::BitFieldInsert, {s0, s1, s6, s7}));
	add(op(ValueOpcode::BitFieldInsert, {s0, s1, Value(0u), Value(32u)}));
	// Floats: s0..s3 carry float bit patterns too.
	add(op(ValueOpcode::ConvertF32U32, {s0}));
	add(op(ValueOpcode::ConvertU32F32, {s2}));
	add(op(ValueOpcode::FPMul32, {s2, s3}));
	add(op(ValueOpcode::FPTrunc32, {s2}));
	add(op(ValueOpcode::FPRecipIFlag32, {s3}));
	add(op(ValueOpcode::FPIsNan32, {s2}));
	for (auto opcode: {ValueOpcode::FPOrdLessThanEqual32, ValueOpcode::FPOrdGreaterThanEqual32}) {
		add(op(opcode, {s2, s3}));
		auto& flushed = b.AppendNewInst(opcode, {s2, s3});
		flushed.SetFlags(FPCompareFlags {.flush_input_denorms = true});
		add(Value(&flushed));
	}
	add(op(ValueOpcode::BitCastF32U32, {op(ValueOpcode::BitCastU32F32, {s1})}));
	// Selects, including nested ones and a select between two memory reads.
	const auto less = op(ValueOpcode::ULessThan32, {s0, s1});
	add(op(ValueOpcode::SelectU32, {less, s2, s3}));
	add(op(ValueOpcode::SelectU32, {op(ValueOpcode::IEqual32, {s4, Value(3u)}),
	                                op(ValueOpcode::SelectU32, {less, s0, s10}), s11}));
	// Raw reads: a scalar address at s8/s9 plus s10 (and a negative immediate), and a constant
	// buffer with records s6 and stride from s9's high bits.
	{
		MemoryInfo address;
		address.kind          = ResourceKind::ScalarAddress;
		address.planning_only = true;
		program.memory_info.push_back(address);
		address.offset = static_cast<uint32_t>(-8);
		program.memory_info.push_back(address);
		MemoryInfo buffer;
		buffer.kind          = ResourceKind::ScalarBuffer;
		buffer.planning_only = true;
		buffer.offset        = 12;
		program.memory_info.push_back(buffer);

		auto& handle = b.AppendNewInst(ValueOpcode::GetAddressResource, {s8, s9});
		for (uint32_t index = 0; index < 2; index++) {
			auto& raw = b.AppendNewInst(ValueOpcode::LoadAddressU32,
			                            {Value(&handle), s10, Value(0u), Value(true)});
			raw.SetFlags(MemoryFlags {.index = index, .pc = 0x40});
			add(Value(&raw));
			add(op(ValueOpcode::SelectU32, {less, Value(&raw), s0}));
		}
		auto& buffer_handle = b.AppendNewInst(ValueOpcode::GetBufferResource, {s8, s9, s6, s1});
		auto& read          = b.AppendNewInst(ValueOpcode::ReadConstBuffer, {Value(&buffer_handle), s10});
		read.SetFlags(MemoryFlags {.index = 2, .pc = 0x44});
		add(Value(&read));
		for (uint32_t offset: {0u, 4u, 16u, 252u, 256u, 0xfffffffcu}) {
			auto& constant_read = b.AppendNewInst(ValueOpcode::ReadConstBuffer, {Value(&buffer_handle), Value(offset)});
			constant_read.SetFlags(MemoryFlags {.index = 2, .pc = 0x44});
			add(Value(&constant_read));
		}
		// A null base reads zero.
		auto& null_handle = b.AppendNewInst(ValueOpcode::GetAddressResource, {Value(0u), Value(0u)});
		auto& null_read   = b.AppendNewInst(ValueOpcode::LoadAddressU32,
		                                    {Value(&null_handle), s10, Value(0u), Value(true)});
		null_read.SetFlags(MemoryFlags {.index = 0, .pc = 0x48});
		add(Value(&null_read));
	}

	for (const auto value: exprs) {
		DescriptorSource source;
		source.dwords[0]   = value;
		source.dword_count = 1;
		program.descriptor_sources.push_back(source);
	}
	// Multiword roots exercise the fused native entry, including shared subexpressions,
	// failures in the middle of a descriptor and mixed immediate/register/memory values.
	for (size_t first = 0; first < exprs.size(); first += 8) {
		DescriptorSource source;
		source.dword_count = static_cast<uint32_t>(std::min(size_t(8), exprs.size() - first));
		for (uint32_t i = 0; i < source.dword_count; ++i) source.dwords[i] = exprs[first + i];
		program.descriptor_sources.push_back(source);
	}
	return ExtractResourcePlan(program);
}

// Interesting 32-bit values: edges of integers and floats.
constexpr uint32_t Specials[] = {0u,          1u,          2u,          31u,         32u,
                                 33u,         0x7fffffffu, 0x80000000u, 0xffffffffu, 0x3f800000u,
                                 0xbf800000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 0x00400000u,
                                 0x80400000u, 0x4f800000u, 0x4f7fffffu, 0x40000000u, 0x00800000u};

void RunCompare(const ResourcePlan& reference, const ResourcePlan& native, bool split) {
	std::mt19937 random(split ? 7u : 3u);
	TestMemory   memory;
	for (uint32_t i = 0; i < MemoryDwords; i++) {
		memory.ordinary[i] = 0x1000u + i * 0x01010101u;
		memory.clean[i]    = 0x2000u + i * 0x00100001u;
	}
	uint32_t mismatches = 0;
	for (uint32_t trial = 0; trial < 4000; trial++) {
		std::array<uint32_t, UserDataCount> user {};
		for (uint32_t i = 0; i < 4; i++) {
			user[i] = (random() & 1u) != 0 ? Specials[random() % std::size(Specials)] : random();
		}
		for (uint32_t i = 4; i < 8; i++) {
			user[i] = random() % 40u;
		}
		// Addresses: mostly inside the test memory, sometimes outside or null.
		const auto pick = random() % 8u;
		const auto base = pick == 0 ? 0ull : pick == 1 ? 0x7000ull : MemoryBase + (random() % 16u) * 4u;
		user[8]         = static_cast<uint32_t>(base);
		// The high dword doubles as the constant buffer's stride (bits 16-29).
		user[9]  = static_cast<uint32_t>(base >> 32u) | ((random() % 3u) << 18u);
		user[10] = (random() % 48u) * ((random() & 3u) == 0 ? 3u : 4u);
		user[11] = random();
		SrtRuntime ordinary {.user_data = user, .read_memory = ReadOrdinary, .userdata = &memory,
		                     .read_specialization_memory = ReadClean};
		SrtRuntime clean    = CleanRuntime(ordinary);
		for (uint32_t source = 0; source < reference.descriptor_sources.size(); source++) {
			DescriptorValue expected {};
			DescriptorValue actual {};
			bool            expected_ok = false;
			bool            actual_ok   = false;
			if (split) {
				SrtWalker ref_clean(reference, clean);
				SrtWalker ref(reference, ordinary, {}, &ref_clean);
				expected_ok = ref.EvaluateDescriptor(source, expected);
				SrtWalker nat_clean(native, clean);
				SrtWalker nat(native, ordinary, {}, &nat_clean);
				actual_ok = nat.EvaluateDescriptor(source, actual);
			} else {
				SrtWalker ref(reference, ordinary);
				expected_ok = ref.EvaluateDescriptor(source, expected);
				SrtWalker nat(native, ordinary);
				actual_ok = nat.EvaluateDescriptor(source, actual);
			}
			if (expected_ok != actual_ok || (expected_ok && expected != actual)) {
				if (mismatches++ < 20) {
					std::fprintf(stderr,
					             "SrtNativeTests: %s source %u: interpreter %s 0x%08x, native %s "
					             "0x%08x (s0..s3 %08x %08x %08x %08x, s4..s7 %u %u %u %u, "
					             "s8 %08x s9 %08x s10 %u)\n",
					             split ? "split" : "self", source, expected_ok ? "ok" : "failed",
					             expected.dwords[0], actual_ok ? "ok" : "failed", actual.dwords[0],
					             user[0], user[1], user[2], user[3], user[4], user[5], user[6],
					             user[7], user[8], user[9], user[10]);
				}
			}
		}
	}
	Check(mismatches == 0, "native and interpreted results differ");
}

void TestNativeMatchesInterpreter(bool value_aliases = false) {
	Check(SrtNativeCode::Supported(), "native SRT code is not supported on this host");
	const auto reference = BuildPlan();
	// Keep the reference plan on the interpreter.
	reference.native_attempted = true;
	const auto native = BuildPlan();
	native.native_code = SrtNativeCode::Compile(native, value_aliases);
	Check(native.native_code != nullptr, "the plan did not compile");
	Check(native.native_code->Interpreted() == 0, "an expression went through the interpreter");
	for (int batch: {0, 1}) {
		Common::LiveSwitches::Get("KYTY_SRT_NATIVE_BATCH", 0).store(batch);
		RunCompare(reference, native, false);
		RunCompare(reference, native, true);
	}
	std::printf("SrtNativeTests: %zu expressions, %u instructions, %zu bytes of code\n",
	            reference.descriptor_sources.size(), native.native_code->Instructions(),
	            native.native_code->CodeSize());
}

ResourcePlan FlatBatchPlan(bool control_flow, uint32_t read_count = 3) {
	Program program;
	program.srt_plan_complete = program.resource_tracking_complete = true;
	auto& block = AddValueBlock(program);
	auto u = [&](uint32_t reg) {
		return Value(&block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(reg))}));
	};
	const auto base = u(0), high = u(1), condition = u(2);
	auto& handle = block.AppendNewInst(ValueOpcode::GetAddressResource, {base, high});
	MemoryInfo memory; memory.kind = ResourceKind::ScalarAddress; memory.planning_only = true;
	program.memory_info.push_back(memory);
	for (uint32_t i = 0; i < read_count; ++i) {
		auto& read = block.AppendNewInst(ValueOpcode::LoadAddressU32,
		    {Value(&handle), Value(i * 4u), Value(0u), Value(true)});
		read.SetFlags(MemoryFlags {.index = 0});
		program.srt_reads.push_back({Value(&read), i});
	}
	program.clean_flat_slots = {0, 1, 0};
	if (control_flow) {
		program.control_flow.resize(3);
		program.control_flow[0].condition = condition;
		program.control_flow[0].successors = {1, 2};
		program.control_flow[1].srt_reads = {0, 1};
		for (uint32_t i = 2; i < read_count; ++i) program.control_flow[2].srt_reads.push_back(i);
	}
	return ExtractResourcePlan(program);
}

void TestFlatBatches(bool value_aliases = false, bool eager_mask = false) {
	if (eager_mask) {
		Common::LiveSwitches::Get("KYTY_SRT_VALUE_ALIASES", 0).store(1);
		Common::LiveSwitches::Get("KYTY_SRT_CANONICAL_VIEWS", 0).store(1);
		Common::LiveSwitches::Get("KYTY_LAZY_SRT_REFRESH", 1).store(0);
	}
	for (bool flow: {false, true}) {
		auto reference = FlatBatchPlan(flow, eager_mask ? 8u : 3u);
		auto native = FlatBatchPlan(flow, eager_mask ? 8u : 3u);
		if (eager_mask) {
			// Most slots are deliberately masked out; eager must still evaluate
			// them, including the six-word ordinary bulk group after strict slot1.
			native.gpu_flat_slots.assign(8, 0);
			native.gpu_flat_slots[1] = 1;
		}
		reference.native_attempted = true;
		native.native_code = SrtNativeCode::Compile(native, value_aliases);
		Check(native.native_code != nullptr, "flat batch did not compile");
		TestMemory memory;
		for (uint32_t i = 0; i < MemoryDwords; ++i) {
			memory.ordinary[i] = 100 + i; memory.clean[i] = 1000 + i;
		}
		for (uint32_t trial = 0; trial < 1000; ++trial) {
			const auto base = trial % 7 == 0 ? MemoryBase + (MemoryDwords - 1u) * 4u
			                  : trial % 5 == 0 ? 0ull : MemoryBase;
			std::array<uint32_t, 3> user {static_cast<uint32_t>(base), 0, trial % 2};
			SrtRuntime runtime {.user_data = user, .read_memory = ReadOrdinary, .userdata = &memory,
			                    .read_specialization_memory = ReadClean};
			std::vector<uint32_t> expected, actual;
			SrtWalker clean(reference, CleanRuntime(runtime));
			SrtWalker walker(reference, runtime, reference.clean_flat_slots, &clean);
			const bool expected_ok = walker.RefreshFlatBuffer(expected);
			for (int batch: {0, 1}) {
				Common::LiveSwitches::Get("KYTY_SRT_NATIVE_BATCH", 0).store(batch);
				SrtWalker native_clean(native, CleanRuntime(runtime));
				SrtWalker candidate(native, runtime, native.clean_flat_slots, &native_clean);
				const bool ok = candidate.RefreshFlatBuffer(actual);
				Check(ok == expected_ok && (!ok || (expected == actual &&
				      reference.evaluation_session.active_sources == native.evaluation_session.active_sources)), "flat batch result differs");
			}
		}
		if (eager_mask) Check(native.native_eager_canonical_code != nullptr,
		    "eager native control fell back instead of preparing full flat batches");
	}
	if (eager_mask) {
		Common::LiveSwitches::Get("KYTY_SRT_VALUE_ALIASES", 0).store(0);
		Common::LiveSwitches::Get("KYTY_SRT_CANONICAL_VIEWS", 0).store(0);
		Common::LiveSwitches::Get("KYTY_LAZY_SRT_REFRESH", 1).store(1);
	}
}

void TestConcurrentEvaluationSessions() {
	auto shared = BuildPlan();
	Check(PrepareResourceEvaluation(shared), "shared plan was not prepared");
	Common::LiveSwitches::Get("KYTY_SRT_NATIVE_BATCH", 0).store(1);
	std::atomic_uint32_t mismatches {0};
	std::vector<std::jthread> workers;
	for (uint32_t id = 0; id < 4; ++id) workers.emplace_back([&, id] {
		auto reference = BuildPlan(); reference.native_attempted = true;
		ResourceEvaluationSession session;
		TestMemory memory;
		for (uint32_t i = 0; i < MemoryDwords; ++i) {
			memory.ordinary[i] = id * 10000 + i;
			memory.clean[i] = id * 20000 + i + 1000;
		}
		for (uint32_t trial = 0; trial < 300; ++trial) {
			std::array<uint32_t, UserDataCount> user {id * 1000 + trial, trial * 17, id * 19,
			    0x3f800000u, 1, 2, 3, 4, uint32_t(MemoryBase), 0, (trial % 16) * 4, id + 7};
			SrtRuntime runtime {.user_data = user, .read_memory = ReadOrdinary, .userdata = &memory,
			    .read_specialization_memory = ReadClean, .evaluation_session = &session};
			auto ref_runtime = runtime; ref_runtime.evaluation_session = nullptr;
			for (uint32_t source = 0; source < shared.descriptor_sources.size(); ++source) {
				SrtWalker ref_clean(reference, CleanRuntime(ref_runtime));
				SrtWalker ref(reference, ref_runtime, reference.clean_flat_slots, &ref_clean);
				SrtWalker actual_clean(shared, CleanRuntime(runtime));
				SrtWalker actual(shared, runtime, shared.clean_flat_slots, &actual_clean);
				DescriptorValue a, b;
				const bool expected = ref.EvaluateDescriptor(source, a), result = actual.EvaluateDescriptor(source, b);
				if (expected != result || (result && a != b)) mismatches.fetch_add(1);
			}
		}
	});
	for (auto& worker: workers) worker.join();
	Check(mismatches.load() == 0, "concurrent sessions mixed values from different inputs");
	std::puts("SrtNativeTests: four concurrent sessions matched independent reference inputs");
}

void TestDirectReadSpans() {
	Common::LiveSwitches::Get("KYTY_SRT_DIRECT_READ", 0).store(1);
	auto reference = BuildPlan(), native = BuildPlan();
	reference.native_attempted = true;
	Check(PrepareResourceEvaluation(native), "direct read plan was not prepared");
	TestMemory memory, expected_memory;
	for (uint32_t i = 0; i < MemoryDwords; ++i) {
		memory.ordinary[i] = expected_memory.ordinary[i] = i * 19;
		memory.clean[i] = expected_memory.clean[i] = i * 31 + 101;
	}
	for (uint32_t trial = 0; trial < 300; ++trial) {
		// Include offsets at and past the last dword: the direct span must route out-of-range
		// reads through the original callback and preserve clean/ordinary reader selection.
		std::array<uint32_t, UserDataCount> user {trial, trial * 7, 3, 0x3f800000u,
		    1, 2, 3, 4, uint32_t(MemoryBase), 0, (trial % 80) * 4, 9};
		memory.fast_ordinary = {}; memory.fast_clean = {};
		SrtRuntime runtime {.user_data = user, .read_memory = ReadOrdinarySpan,
		    .userdata = &memory, .read_specialization_memory = ReadCleanSpan,
		    .fast_read = &memory.fast_ordinary, .fast_specialization_read = &memory.fast_clean};
		SrtRuntime expected {.user_data = user, .read_memory = ReadOrdinary,
		    .userdata = &expected_memory, .read_specialization_memory = ReadClean};
		for (uint32_t source = 0; source < native.descriptor_sources.size(); ++source) {
			SrtWalker clean_a(reference, CleanRuntime(expected));
			SrtWalker a(reference, expected, reference.clean_flat_slots, &clean_a);
			SrtWalker clean_b(native, CleanRuntime(runtime));
			SrtWalker b(native, runtime, native.clean_flat_slots, &clean_b);
			DescriptorValue av, bv;
			const bool aok = a.EvaluateDescriptor(source, av), bok = b.EvaluateDescriptor(source, bv);
			Check(aok == bok && (!aok || av == bv), "direct span differs from original reader");
		}
	}
	Check(memory.reads < expected_memory.reads, "direct span never bypassed a callback");
	std::printf("SrtNativeTests: direct spans matched; callbacks %u vs %u\n", memory.reads, expected_memory.reads);
	Common::LiveSwitches::Get("KYTY_SRT_DIRECT_READ", 0).store(0);
}

ResourcePlan BulkReadPlan(bool flow, uint32_t clean_mode, bool gather = false, bool duplicate = false,
                         uint32_t user_base = 0, bool distinct_inputs = false, bool grouped_descriptors = false) {
	Program program;
	program.srt_plan_complete = program.resource_tracking_complete = true;
	program.user_data_base = user_base; program.user_data_count = 5;
	auto& block = AddValueBlock(program);
	auto user = [&](uint32_t reg) { return Value(&block.AppendNewInst(ValueOpcode::GetUserData,
	    {Value(static_cast<ScalarReg>(reg + user_base))})); };
	const auto low = user(0), high = user(1), records = user(2), word3 = user(3), condition = user(4);
	program.memory_info.push_back({.kind = ResourceKind::ScalarBuffer, .planning_only = true});
	for (uint32_t i = 0; i < 16; ++i) {
		// Deliberately distinct handles with identical arguments, like the game's extracted IR.
		auto& handle = distinct_inputs
		    ? block.AppendNewInst(ValueOpcode::GetBufferResource, {user(0), user(1), user(2), user(3)})
		    : block.AppendNewInst(ValueOpcode::GetBufferResource, {low, high, records, word3});
		constexpr uint32_t permutation[8] {0, 3, 1, 7, 2, 6, 4, 5};
		const auto offset = gather ? (i / 8 * 8 + permutation[i % 8]) * 4 : i * 4;
		auto& read = block.AppendNewInst(ValueOpcode::ReadConstBuffer, {Value(&handle), Value(offset)});
		read.SetFlags(MemoryFlags {.index = 0});
		program.srt_reads.push_back({Value(&read), i});
		program.clean_flat_slots.push_back(clean_mode == 1 || (clean_mode == 2 && i >= 8));
	}
	if (duplicate) program.srt_reads[5].value = program.srt_reads[0].value;
	auto& srt = block.AppendNewInst(ValueOpcode::GetSrtResource);
	const uint32_t width = grouped_descriptors ? 8 : 1;
	for (uint32_t i = 0; i < 16; i += width) {
		DescriptorSource source;
		for (uint32_t j = 0; j < width; ++j)
			source.dwords[j] = Value(&block.AppendNewInst(ValueOpcode::ReadConst, {Value(&srt), Value(i + j)}));
		source.dword_count = width;
		program.descriptor_sources.push_back(source);
	}
	if (flow) {
		program.control_flow.resize(3);
		program.control_flow[0].condition = condition;
		program.control_flow[0].successors = {1, 2};
		for (uint32_t i = 0; i < 16; ++i) program.control_flow[i < 8 ? 1 : 2].srt_reads.push_back(i);
	}
	return ExtractResourcePlan(program);
}

void TestDescriptorOnlyBatches() {
	Common::LiveSwitches::Get("KYTY_SRT_BULK_READS", 0).store(1);
	Common::LiveSwitches::Get("KYTY_SRT_NATIVE_BATCH", 0).store(1);
	for (uint32_t clean_mode = 0; clean_mode < 3; ++clean_mode) for (bool gather: {false, true})
	for (bool spans: {false, true}) {
		auto reference = BulkReadPlan(false, clean_mode, gather, false, 0, true, true);
		auto native = BulkReadPlan(false, clean_mode, gather, false, 0, true, true);
		reference.gpu_flat_slots.assign(16, 0); native.gpu_flat_slots.assign(16, 0);
		reference.native_attempted = true;
		native.native_code = SrtNativeCode::Compile(native, true, false, true);
		Check(PrepareResourceEvaluation(native) && native.native_code->BulkReadRoots() != 0,
		    "descriptor-only reads were not grouped");
		TestMemory memory;
		for (uint32_t i = 0; i < MemoryDwords; ++i) { memory.ordinary[i] = i + 100; memory.clean[i] = i + 10000; }
		for (uint32_t trial = 0; trial < 128; ++trial) {
			std::array<uint32_t, 5> inputs {uint32_t(trial % 11 == 0 ? 0 :
			    MemoryBase + (trial % 7 == 0 ? MemoryDwords * 4 - 4 : trial % 4 * 4)),
			    (trial % 3) << 16, trial % 70, 0, 1};
			memory.fast_ordinary = {}; memory.fast_clean = {};
			SrtRuntime runtime {.user_data = inputs, .read_memory = ReadOrdinarySpan, .userdata = &memory,
			    .read_specialization_memory = ReadCleanSpan, .fast_read = spans ? &memory.fast_ordinary : nullptr,
			    .fast_specialization_read = spans ? &memory.fast_clean : nullptr};
			auto original = runtime; original.read_memory = ReadOrdinary; original.read_specialization_memory = ReadClean;
			original.fast_read = original.fast_specialization_read = nullptr;
			SrtWalker ca(reference, CleanRuntime(original)), a(reference, original, reference.clean_flat_slots, &ca);
			SrtWalker cb(native, CleanRuntime(runtime)), b(native, runtime, native.clean_flat_slots, &cb);
			if (trial % 5 == 0) {
				uint32_t x = 0, y = 0;
				Check(a.Evaluate(reference.srt_reads[2].value, x) == b.Evaluate(native.srt_reads[2].value, y) && x == y,
				    "descriptor partial memo differs");
				memory.ordinary[2] ^= 0x1234;
			}
			std::vector<uint32_t> af, bf;
			Check(a.RefreshFlatBuffer(af) && b.RefreshFlatBuffer(bf) && af == bf &&
			    std::ranges::all_of(bf, [](uint32_t value) { return value == 0; }), "lazy descriptor slots were populated eagerly");
			for (uint32_t source = 0; source < 2; ++source) {
				DescriptorValue expected, actual;
				const bool aok = a.EvaluateDescriptor(source, expected), bok = b.EvaluateDescriptor(source, actual);
				Check(aok == bok && (!aok || actual == expected), "descriptor batch differs from interpreter");
				if (aok) {
					memory.ordinary[0] ^= 1; memory.clean[0] ^= 2; memory.reads = 0;
					DescriptorValue again;
					Check(b.EvaluateDescriptor(source, again) && again == actual && memory.reads == 0,
					    "descriptor batch re-read or changed memoized words");
				}
			}
		}
	}
	Common::LiveSwitches::Get("KYTY_SRT_BULK_READS", 0).store(0);
	std::puts("SrtNativeTests: descriptor-only batches preserved lazy slots, memos, failures, bounds and clean routing");
}

void TestBulkReadBatches() {
	Common::LiveSwitches::Get("KYTY_SRT_BULK_READS", 0).store(1);
	Common::LiveSwitches::Get("KYTY_SRT_NATIVE_BATCH", 0).store(1);
	for (bool canonical: {false, true})
	for (bool flow: {false, true}) for (uint32_t clean_mode = 0; clean_mode < 3; ++clean_mode)
	for (bool gather: {false, true}) for (bool duplicate: {false, true}) {
		auto reference = BulkReadPlan(flow, clean_mode, gather, duplicate, 0, true);
		auto native = BulkReadPlan(flow, clean_mode, gather, duplicate, 0, true);
		reference.native_attempted = true;
		native.native_code = SrtNativeCode::Compile(native, true, false, canonical);
		Check(PrepareResourceEvaluation(native), "bulk plan was not prepared");
		Check((duplicate || native.native_code->BulkReadRoots() == (canonical ? 16 : 0)) &&
		    (canonical ? native.native_code->BufferViewCount() == 1 : native.native_code->BufferViewCount() >= 15),
		    "bulk plan failed to group roots or canonicalize handles");
		TestMemory memory;
		for (uint32_t i = 0; i < MemoryDwords; ++i) {
			memory.ordinary[i] = i * 31 + 100; memory.clean[i] = i * 19 + 10000;
		}
		for (uint32_t trial = 0; trial < 200; ++trial) {
			std::array<uint32_t, 5> inputs {uint32_t(trial % 11 == 0 ? 0 : MemoryBase + (trial % 4) * 64),
			    (trial % 3) << 16, trial % 90, trial, trial % 2};
			memory.fast_ordinary = {}; memory.fast_clean = {};
			if (trial % 3 == 0) {
				memory.fast_ordinary = {MemoryBase, reinterpret_cast<const uint8_t*>(memory.ordinary.data()), sizeof(memory.ordinary) - 4};
				memory.fast_clean = {MemoryBase, reinterpret_cast<const uint8_t*>(memory.clean.data()), sizeof(memory.clean) - 4};
			}
			SrtRuntime runtime {.user_data = inputs, .read_memory = ReadOrdinarySpan, .userdata = &memory,
			    .read_specialization_memory = ReadCleanSpan, .fast_read = &memory.fast_ordinary,
			    .fast_specialization_read = &memory.fast_clean};
			auto original = runtime; original.read_memory = ReadOrdinary; original.read_specialization_memory = ReadClean;
			original.fast_read = original.fast_specialization_read = nullptr;
			SrtWalker clean_a(reference, CleanRuntime(original)), a(reference, original, reference.clean_flat_slots, &clean_a);
			SrtWalker clean_b(native, CleanRuntime(runtime)), b(native, runtime, native.clean_flat_slots, &clean_b);
			if (trial % 5 == 0) {
				uint32_t av = 0, bv = 0;
				Check(a.Evaluate(reference.srt_reads[2].value, av) == b.Evaluate(native.srt_reads[2].value, bv) && av == bv,
				    "bulk pre-evaluation differs");
				memory.ordinary[gather ? 1 : 2] ^= 0x1234;
			}
			std::vector<uint32_t> av, bv;
			const bool aok = a.RefreshFlatBuffer(av), bok = b.RefreshFlatBuffer(bv);
			Check(aok == bok && (!aok || av == bv), "bulk batch differs in values, bounds, memo or clean routing");
			if (aok) {
				memory.ordinary[0] ^= 0x1987; memory.clean[0] ^= 0x2987; memory.reads = 0;
				std::vector<uint32_t> av2, bv2;
				Check(a.RefreshFlatBuffer(av2) && b.RefreshFlatBuffer(bv2) && av2 == av && bv2 == bv && memory.reads == 0,
				    "fully memoized group re-read memory or changed cached values");
			}
		}
		reference.capture_specialization_reads = native.capture_specialization_reads = true;
		std::array<uint32_t, 5> inputs {uint32_t(MemoryBase), 0, 256, 0, 1};
		memory.fast_ordinary = {MemoryBase, reinterpret_cast<const uint8_t*>(memory.ordinary.data()), sizeof(memory.ordinary) - 4};
		memory.fast_clean = {MemoryBase, reinterpret_cast<const uint8_t*>(memory.clean.data()), sizeof(memory.clean) - 4};
		SrtRuntime runtime {.user_data = inputs, .read_memory = ReadOrdinarySpan, .userdata = &memory,
		    .read_specialization_memory = ReadCleanSpan, .fast_read = &memory.fast_ordinary,
		    .fast_specialization_read = &memory.fast_clean};
		auto original = runtime; original.read_memory = ReadOrdinary; original.read_specialization_memory = ReadClean;
		original.fast_read = original.fast_specialization_read = nullptr;
		ResourceSnapshot expected, actual;
		ResourceSpecialization sa, sb;
		Check(MaterializeResources(reference, original, expected, sa), "bulk capture reference failed");
		memory.reads = 0;
		Check(MaterializeResources(native, runtime, actual, sb) && actual.flattened_srt == expected.flattened_srt &&
		    actual.specialization_reads == expected.specialization_reads && memory.reads != 0 && sa == sb,
		    "bulk read bypassed required capture callbacks");
	}
	Common::LiveSwitches::Get("KYTY_SRT_BULK_READS", 0).store(0);
	std::puts("SrtNativeTests: grouped/gathered batches matched interpreter across bounds, memos, duplicate nodes, clean readers and branches");
}

void TestBulkReaderOrderAndFailures() {
	Common::LiveSwitches::Get("KYTY_SRT_BULK_READS", 0).store(1);
	Common::LiveSwitches::Get("KYTY_SRT_NATIVE_BATCH", 0).store(1);
	struct Reader {
		std::array<uint32_t, MemoryDwords> words {};
		std::vector<uint64_t> addresses;
		SrtReadSpan span;
		bool short_span = false, reject = false;
		static bool Read(void* data, uint64_t address, std::span<uint32_t> values) {
			auto& self = *static_cast<Reader*>(data);
			self.addresses.push_back(address);
			if (self.reject || !ReadFrom(self.words, address, values)) return false;
			self.span = self.short_span
			    ? SrtReadSpan {address, reinterpret_cast<const uint8_t*>(self.words.data()) + address - MemoryBase, 0}
			    : SrtReadSpan {MemoryBase, reinterpret_cast<const uint8_t*>(self.words.data()), sizeof(self.words) - 4};
			return true;
		}
	};
	for (bool gather: {false, true}) for (bool short_span: {false, true}) for (bool capture: {false, true})
	for (uint32_t user_base: {0u, 8u}) {
		auto plan = BulkReadPlan(false, 0, gather, false, user_base);
		plan.capture_specialization_reads = capture;
		Check(PrepareResourceEvaluation(plan), "reader order plan not prepared");
		Reader reader; reader.short_span = short_span;
		for (uint32_t i = 0; i < MemoryDwords; ++i) reader.words[i] = 700 + i;
		std::array<uint32_t, 5> inputs {uint32_t(MemoryBase), 0, 256, 0, 0};
		SrtRuntime runtime {.user_data = inputs, .read_memory = Reader::Read, .userdata = &reader,
		    .read_specialization_memory = Reader::Read, .fast_read = &reader.span, .fast_specialization_read = &reader.span};
		ResourceSnapshot snapshot; ResourceSpecialization specialization;
		Check(MaterializeResources(plan, runtime, snapshot, specialization), "bulk reader order materialization failed");
		for (uint32_t i = 0; i < 16; ++i) {
			const auto offset = plan.srt_reads[i].value.ResolveInstruction()->Arg(1).U32();
			Check(snapshot.flattened_srt[i] == reader.words[offset / 4], "bulk reader order value differs");
			if (short_span || capture) Check(reader.addresses[i] == MemoryBase + offset, "bulk changed reader order");
		}
		Check(reader.addresses.size() == (short_span || capture ? 16u : 1u), "bulk duplicated or omitted callbacks");
		Check(!capture || snapshot.specialization_reads.size() == 16, "bulk omitted capture reads");
	}
	for (bool null_base: {false, true}) {
		auto plan = BulkReadPlan(false, 0);
		Check(PrepareResourceEvaluation(plan), "failing reader plan not prepared");
		Reader reader; reader.reject = true;
		std::array<uint32_t, 5> inputs {null_base ? 0u : uint32_t(MemoryBase), 0, 256, 0, 0};
		SrtRuntime runtime {.user_data = inputs, .read_memory = Reader::Read, .userdata = &reader,
		    .read_specialization_memory = Reader::Read, .fast_read = &reader.span, .fast_specialization_read = &reader.span};
		ResourceSnapshot snapshot; ResourceSpecialization specialization;
		const bool ok = MaterializeResources(plan, runtime, snapshot, specialization);
		Check(ok == null_base && reader.addresses.size() == (null_base ? 16u : 1u), "bulk retried a failed read");
		if (null_base) Check(std::ranges::all_of(snapshot.flattened_srt, [](uint32_t word) { return word == 0; }), "bulk null-base result differs");
	}
	Common::LiveSwitches::Get("KYTY_SRT_BULK_READS", 0).store(0);
	std::puts("SrtNativeTests: bulk reader order, partial spans, capture and failed/null reads passed");
}

void TestLiveValueAliases(bool inline_user_data = true) {
	auto& aliases = Common::LiveSwitches::Get("KYTY_SRT_VALUE_ALIASES", 0);
	auto& bulk = Common::LiveSwitches::Get("KYTY_SRT_BULK_READS", 0);
	for (uint32_t clean_mode = 0; clean_mode < 3; ++clean_mode) for (int bulk_mode: {0, 1}) {
		bulk.store(bulk_mode);
		auto reference = BulkReadPlan(false, clean_mode), native = BulkReadPlan(false, clean_mode);
		reference.native_attempted = true;
		native.native_code = SrtNativeCode::Compile(native);
		native.native_alias_code = SrtNativeCode::Compile(native, true, inline_user_data);
		native.native_alias_attempted = true;
		Check(native.native_code != nullptr && native.native_alias_code != nullptr, "live alias variants not compiled");
		TestMemory memory;
		for (uint32_t i = 0; i < MemoryDwords; ++i) { memory.ordinary[i] = i + 17; memory.clean[i] = i + 1071; }
		for (uint32_t trial = 0; trial < 200; ++trial) {
			aliases.store(trial % 2);
			std::array<uint32_t, 5> inputs {uint32_t(MemoryBase + trial % 4 * 64), (trial % 3) << 16, trial % 90, trial, 0};
			memory.fast_ordinary = {}; memory.fast_clean = {};
			SrtRuntime runtime {.user_data = inputs, .read_memory = ReadOrdinarySpan, .userdata = &memory,
			    .read_specialization_memory = ReadCleanSpan,
			    .fast_read = &memory.fast_ordinary, .fast_specialization_read = &memory.fast_clean};
			SrtWalker clean_a(reference, CleanRuntime(runtime)), a(reference, runtime, reference.clean_flat_slots, &clean_a);
			SrtWalker clean_b(native, CleanRuntime(runtime)), b(native, runtime, native.clean_flat_slots, &clean_b);
			std::vector<uint32_t> av, bv;
			const bool aok = a.RefreshFlatBuffer(av), bok = b.RefreshFlatBuffer(bv);
			Check(aok == bok && (!aok || av == bv), "live alias flat values differ");
			for (uint32_t source = 0; source < reference.descriptor_sources.size(); ++source) {
				DescriptorValue ad, bd;
				const bool ar = a.EvaluateDescriptor(source, ad), br = b.EvaluateDescriptor(source, bd);
				Check(ar == br && (!ar || ad == bd), "live alias descriptor differs");
			}
		}
	}
	aliases.store(0);
	bulk.store(0);
	std::puts("SrtNativeTests: live alias variants matched interpreter in flat slots and descriptor roots");
}

void TestSpanNeedsStrictReader() {
	Common::LiveSwitches::Get("KYTY_SRT_DIRECT_READ", 0).store(1);
	for (int bulk: {0, 1}) {
		Common::LiveSwitches::Get("KYTY_SRT_BULK_READS", 0).store(bulk);
		auto plan = BulkReadPlan(false, 1);
		Check(PrepareResourceEvaluation(plan), "missing strict reader plan not prepared");
		TestMemory memory;
		SrtReadSpan span {MemoryBase, reinterpret_cast<const uint8_t*>(memory.clean.data()), sizeof(memory.clean) - 4};
		std::array<uint32_t, 5> inputs {uint32_t(MemoryBase), 0, 256, 0, 0};
		SrtRuntime runtime {.user_data = inputs, .read_memory = ReadOrdinary, .userdata = &memory,
		    .fast_specialization_read = &span};
		SrtWalker clean(plan, CleanRuntime(runtime));
		DescriptorValue value;
		Check(!clean.EvaluateDescriptor(0, value) && memory.reads == 0, "span bypassed a missing strict reader");
	}
	Common::LiveSwitches::Get("KYTY_SRT_DIRECT_READ", 0).store(0);
	Common::LiveSwitches::Get("KYTY_SRT_BULK_READS", 0).store(0);
}

ResourcePlan NestedBulkReadPlan(bool clean_prefix, bool clean_data, bool independent_prefix = false) {
	Program program;
	program.srt_plan_complete = program.resource_tracking_complete = true;
	program.user_data_count = 1;
	auto& block = AddValueBlock(program);
	auto& user = block.AppendNewInst(ValueOpcode::GetUserData, {Value(static_cast<ScalarReg>(0))});
	auto& address = block.AppendNewInst(ValueOpcode::GetAddressResource, {Value(&user), Value(0u)});
	program.memory_info.push_back({.kind = ResourceKind::ScalarAddress, .planning_only = true});
	program.memory_info.push_back({.kind = ResourceKind::ScalarBuffer, .planning_only = true});
	const uint32_t prefix_count = independent_prefix ? 8 : 4;
	for (uint32_t i = 0; i < prefix_count; ++i) {
		auto& read = block.AppendNewInst(ValueOpcode::LoadAddressU32,
		    {Value(&address), Value(i % 4 * 4), Value(0u), Value(true)});
		read.SetFlags(MemoryFlags {.index = 0});
		program.srt_reads.push_back({Value(&read), i});
		program.clean_flat_slots.push_back(clean_prefix);
	}
	auto& srt = block.AppendNewInst(ValueOpcode::GetSrtResource);
	for (uint32_t i = 0; i < 16; ++i) {
		std::array<Value, 4> words;
		for (uint32_t lane = 0; lane < 4; ++lane)
			words[lane] = Value(&block.AppendNewInst(ValueOpcode::ReadConst,
			    {Value(&srt), Value(lane + (independent_prefix && i >= 8 ? 4 : 0))}));
		auto& buffer = block.AppendNewInst(ValueOpcode::GetBufferResource, {words[0], words[1], words[2], words[3]});
		auto& read = block.AppendNewInst(ValueOpcode::ReadConstBuffer, {Value(&buffer), Value(i * 4)});
		read.SetFlags(MemoryFlags {.index = 1});
		program.srt_reads.push_back({Value(&read), prefix_count + i});
		program.clean_flat_slots.push_back(clean_data);
	}
	return ExtractResourcePlan(program);
}

void TestNestedBulkReaderProvenance() {
	auto& aliases = Common::LiveSwitches::Get("KYTY_SRT_VALUE_ALIASES", 0);
	auto& canonical = Common::LiveSwitches::Get("KYTY_SRT_CANONICAL_VIEWS", 0);
	Common::LiveSwitches::Get("KYTY_SRT_BULK_READS", 0).store(1);
	for (bool independent: {false, true})
	for (bool clean_prefix: {false, true}) for (bool clean_data: {false, true}) for (int alias: {0, 1, 2}) {
		aliases.store(alias != 0); canonical.store(alias == 2);
		auto reference = NestedBulkReadPlan(clean_prefix, clean_data, independent);
		auto native = NestedBulkReadPlan(clean_prefix, clean_data, independent);
		reference.native_attempted = true;
		Check(PrepareResourceEvaluation(native), "nested bulk plan not prepared");
		native.native_alias_code = SrtNativeCode::Compile(native, true, false); native.native_alias_attempted = true;
		native.native_canonical_code = SrtNativeCode::Compile(native, true, false, true);
		native.native_canonical_attempted = true;
		Check(native.native_canonical_code != nullptr &&
		    native.native_canonical_code->BufferViewCount() == (independent ? 2 : 1),
		    "canonical view merged independent memory operations or missed identical memo sources");
		{ SrtWalker warm(native, {}); } // different generations for the clean and ordinary frames
		TestMemory memory;
		for (uint32_t i = 0; i < MemoryDwords; ++i) { memory.ordinary[i] = 100 + i; memory.clean[i] = 10000 + i; }
		memory.ordinary[1] = memory.clean[1] = 0;
		memory.ordinary[2] = memory.clean[2] = 256;
		memory.ordinary[3] = memory.clean[3] = 0;
		for (uint32_t trial = 0; trial < 100; ++trial) {
			memory.ordinary[0] = uint32_t(MemoryBase + 64 + trial % 2 * 16);
			memory.clean[0] = uint32_t(MemoryBase + 128 + trial % 2 * 16);
			memory.fast_ordinary = {}; memory.fast_clean = {};
			std::array<uint32_t, 1> inputs {uint32_t(MemoryBase)};
			SrtRuntime runtime {.user_data = inputs, .read_memory = ReadOrdinarySpan, .userdata = &memory,
			    .read_specialization_memory = ReadCleanSpan, .fast_read = &memory.fast_ordinary,
			    .fast_specialization_read = &memory.fast_clean};
			SrtWalker ca(reference, CleanRuntime(runtime)), a(reference, runtime, reference.clean_flat_slots, &ca);
			SrtWalker cb(native, CleanRuntime(runtime)), b(native, runtime, native.clean_flat_slots, &cb);
			// Keep a valid but different ordinary pointer in the memo before strict prefix refresh.
			uint32_t ap = 0, bp = 0;
			Check(a.Evaluate(reference.srt_reads[0].value, ap) && b.Evaluate(native.srt_reads[0].value, bp) && ap == bp,
			    "nested prefix pre-evaluation failed");
			std::vector<uint32_t> av, bv;
			Check(a.RefreshFlatBuffer(av) && b.RefreshFlatBuffer(bv) && av == bv,
			    "nested bulk view used the wrong reader's pointer or memo generation");
		}
	}
	aliases.store(0); canonical.store(0); Common::LiveSwitches::Get("KYTY_SRT_BULK_READS", 0).store(0);
	std::puts("SrtNativeTests: nested views preserved strict/ordinary pointer provenance with both code variants");
}

} // namespace

namespace Common {

int DbgExitHandler(const char* file, int line, std::string_view text) {
	std::fprintf(stderr, "SrtNativeTests: EXIT at %s:%d: %.*s\n", file, line,
	             static_cast<int>(text.size()), text.data());
	std::abort();
}

int DbgExitHandler(const char* file, int line, fmt::text_style, std::string_view text) {
	return DbgExitHandler(file, line, text);
}

int DbgExitIfHandler(const char* expression, const char* file, int line) {
	std::fprintf(stderr, "SrtNativeTests: EXIT_IF(%s) at %s:%d\n", expression, file, line);
	return 1;
}

void DbgExit(int) {
	std::abort();
}

} // namespace Common

int main() {
	TestNativeMatchesInterpreter();
	TestFlatBatches();
	TestConcurrentEvaluationSessions();
	TestDirectReadSpans();
	TestBulkReadBatches();
	TestDescriptorOnlyBatches();
	TestBulkReaderOrderAndFailures();
	TestLiveValueAliases();
	TestLiveValueAliases(false);
	TestSpanNeedsStrictReader();
	TestNestedBulkReaderProvenance();
	TestNativeMatchesInterpreter(true);
	TestFlatBatches(true);
	TestFlatBatches(true, true);
	Common::LiveSwitches::Get("KYTY_SRT_BUFFER_VIEWS", 0).store(1);
	TestNativeMatchesInterpreter();
	TestFlatBatches();
	TestConcurrentEvaluationSessions();
	TestDirectReadSpans();
	std::puts("SrtNativeTests: all cases passed");
	return 0;
}

#include "graphics/shader/recompiler/ir/Block.cpp"
#include "graphics/shader/recompiler/ir/Program.cpp"
#include "graphics/shader/recompiler/ir/Type.cpp"
#include "graphics/shader/recompiler/ir/Value.cpp"
#include "graphics/shader/recompiler/ir/opcodes/ValueOpcodes.cpp"
