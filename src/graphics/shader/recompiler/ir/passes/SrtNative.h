#ifndef EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_PASSES_SRTNATIVE_H_
#define EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_PASSES_SRTNATIVE_H_

// The native SRT walker: a resource plan's evaluation graph compiled into x86-64 machine code.
//
// SrtWalker (SrtWalker.cpp) interprets a plan's IR on every draw and dispatch: resolve the value,
// check the memo, dispatch on the opcode, recurse into the operands. That interpretation was most
// of the GPU thread's resource walk (DEBUGGING.md, 2026-09-30). SrtNativeCode emits one routine
// per plan instruction and walker mode that does the same work with the dispatch compiled away:
//
// - Each routine checks and fills the same memo entry the interpreter uses
//   (ResourcePlan::EvaluationContext, {value, generation}, the low generation bit marking a value
//   still being evaluated), so native and interpreted evaluation mix freely.
// - Operands are evaluated lazily, in the interpreter's order: a select evaluates only the arm its
//   predicate chooses, so no memory is read that the interpreter would not read.
// - Guest memory reads go through the walker's reader, like the interpreter's.
// - An instruction the compiler does not handle calls the interpreter for that one instruction.
//
// Two modes, matching MaterializeResources' two walkers:
// - Self: the walker has no clean evaluator. Select predicates and every SRT slot use it.
// - Split: the walker has a clean evaluator (itself in Self mode) and the plan's clean flat slots;
//   select predicates and clean slots are evaluated in the clean walker, as the interpreter does.
//
// Routine convention (private to the generated code): R12 = the current walker's frame,
// R13 = its memo base, R14 = its generation. A routine returns the value in RAX with CF clear, or
// CF set on failure. It may clobber RAX, RCX, RDX, R8-R11 and XMM0-XMM1, and preserves the rest.

#include "graphics/shader/recompiler/ir/ShaderIR.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace Libs::Graphics::ShaderRecompiler::IR {

class SrtWalker;

// A reader can publish bytes whose ownership and lifetime it has already checked for this
// evaluation. The span is invalidated whenever logging/strict capture needs every callback.
struct SrtReadSpan {
	uint64_t base = UINT64_MAX;
	const uint8_t* data = nullptr;
	uint64_t last_dword_offset = 0;
};

// One walker's state as the generated code sees it. The field offsets are part of the code.
struct SrtNativeFrame {
	ResourcePlan::EvaluationContext::Entry* memo           = nullptr; // 0
	uint64_t                                generation     = 0;       // 8
	const uint32_t*                         user_data      = nullptr; // 16
	uint64_t                                user_data_size = 0;       // 24
	uint64_t                                shader_base    = 0;       // 32
	SrtWalker*                              walker         = nullptr; // 40
	SrtNativeFrame*                         clean          = nullptr; // 48
	const Inst**                            failed         = nullptr; // 56
	const SrtReadSpan*                       fast_read      = nullptr; // 64
	ResourcePlan::EvaluationContext::BufferView* buffer_views = nullptr; // 72
	uint64_t bulk_reads = 0; // 80
	const SrtReadSpan* captured_bulk_read = nullptr;
	std::vector<std::pair<uint64_t, uint64_t>>* captured_reads = nullptr;
};

struct SrtBulkReadPlan {
	struct Word { uint32_t memo_index = 0, flat_offset = 0, offset = 0; };
	struct Source {
		enum class Kind : uint32_t { Invalid, Immediate, UserData, Memo, CleanMemo };
		Kind kind = Kind::Invalid;
		uint32_t index = 0;
		uint64_t value = 0;
	};
	uint32_t view_index = 0, count = 0;
	uint64_t minimum = 0, maximum = 0;
	std::array<Word, 8> words;
	std::array<Source, 4> sources;
	std::array<const Inst*, 8> instructions;
};

enum class SrtNativeMode : uint32_t { Self = 0, Split = 1 };

// A plan value resolved once at compile time, so the walker's hot entry points (flat SRT reads,
// descriptor dwords, control-flow conditions) need no Value::Resolve, memo index or routine
// lookup per call: those touched the scattered IR nodes and were most of the remaining cost.
struct SrtNativeValue {
	enum Kind : uint8_t { Fail = 0, Immediate, Routine, Interpret };
	Kind        kind      = Fail;
	uint32_t    index     = 0;       // Routine: the instruction's evaluation index.
	uint64_t    immediate = 0;       // Immediate: its bits, as EvaluateWide returns them.
	const Inst* inst      = nullptr; // Routine and Interpret: the instruction.
};

// Totals of the plans compiled so far (SrtWalker.cpp).
struct SrtNativeStats {
	uint32_t plans        = 0;
	uint32_t failed       = 0;
	uint64_t bytes        = 0;
	uint64_t instructions = 0;
	uint64_t interpreted  = 0;
};

// True, with the totals, once after each doubling of the number of plans compiled.
bool TakeSrtNativeReport(SrtNativeStats& stats);

class SrtNativeCode {
public:
	~SrtNativeCode();
	SrtNativeCode(const SrtNativeCode&)            = delete;
	SrtNativeCode& operator=(const SrtNativeCode&) = delete;

	// Compiles every instruction of the plan in both modes. Assigns the plan's evaluation indices,
	// so evaluation_value_count is final afterwards. Returns null when the host cannot run it.
	static std::unique_ptr<SrtNativeCode> Compile(const ResourcePlan& program, bool value_aliases = false,
	                                            bool inline_user_data = true, bool canonical_views = false,
	                                            bool eager_flat = false);

	// Whether this build can generate and run native code at all.
	static bool Supported();
	[[nodiscard]] uint32_t BufferViewCount() const { return m_buffer_views; }
	[[nodiscard]] uint32_t BulkReadRoots() const { return m_bulk_roots; }

	// Whether the instruction with this evaluation index has a routine in this mode.
	[[nodiscard]] bool Has(SrtNativeMode mode, uint32_t index) const;
	// Evaluates the instruction with this evaluation index (Has() must be true): the same result,
	// memo updates and failure as SrtWalker::EvaluateWide on that instruction.
	bool Evaluate(SrtNativeFrame& frame, SrtNativeMode mode, uint32_t index,
	              uint64_t& result) const;
	// Fused roots: enter the native calling convention once per descriptor or visited SRT block.
	// Roots retain their original order and use the same lazy routines and memo as scalar entry.
	bool EvaluateDescriptor(SrtNativeFrame& frame, SrtNativeMode mode, uint32_t source,
	                        uint32_t* output) const;
	[[nodiscard]] bool HasReadonlyBufferGroup(size_t count) const;
	size_t EvaluateReadonlyBufferGroup(SrtNativeFrame& frame, SrtNativeMode mode,
	                                 DescriptorValue* output,uint64_t active_mask) const;
	bool RefreshBlock(SrtNativeFrame& frame, SrtNativeMode mode, uint32_t block,
	                  uint32_t* flat) const;

	// The plan's srt_reads, control_flow conditions and descriptor dwords (8 per source).
	[[nodiscard]] const std::vector<SrtNativeValue>& FlatReads() const { return m_flat_reads; }
	[[nodiscard]] const std::vector<SrtNativeValue>& Conditions() const { return m_conditions; }
	[[nodiscard]] const SrtNativeValue* DescriptorDwords(uint32_t source) const {
		return &m_descriptor_dwords[static_cast<size_t>(source) * 8u];
	}

	[[nodiscard]] size_t CodeSize() const { return m_size; }
	[[nodiscard]] uint32_t Instructions() const { return m_instructions; }
	[[nodiscard]] uint32_t Interpreted() const { return m_interpreted; }

private:
	SrtNativeCode() = default;

	using Entry = bool (*)(SrtNativeFrame* frame, const void* routine, uint64_t* result);
	using BatchEntry = bool (*)(SrtNativeFrame* frame, const void* routine, uint32_t* output);
	using GroupEntry = size_t (*)(SrtNativeFrame* frame, const void* routine, DescriptorValue* output,uint64_t active_mask);

	uint8_t*              m_memory = nullptr;
	size_t                m_size   = 0;
	Entry                 m_entry  = nullptr;
	BatchEntry            m_batch_entry = nullptr;
	GroupEntry            m_group_entry = nullptr;
	// Routine offsets by evaluation index, per mode; UINT32_MAX when the index has no routine.
	std::vector<uint32_t> m_routines[2];
	std::vector<uint32_t> m_descriptor_batches[2];
	std::array<uint32_t,2> m_readonly_buffer_batches {UINT32_MAX,UINT32_MAX};
	uint32_t m_readonly_buffer_count = 0;
	// Slot 0: all reads when there is no control flow; slot 1+n: block n.
	std::vector<uint32_t> m_flat_batches[2];
	std::vector<SrtNativeValue> m_flat_reads;
	std::vector<SrtNativeValue> m_conditions;
	std::vector<SrtNativeValue> m_descriptor_dwords;
	uint32_t              m_instructions = 0;
	uint32_t              m_interpreted  = 0;
	uint32_t              m_buffer_views = 0;
	uint32_t              m_bulk_roots = 0;
	std::vector<std::unique_ptr<SrtBulkReadPlan>> m_bulk_plans;
};

} // namespace Libs::Graphics::ShaderRecompiler::IR

#endif // EMULATOR_SRC_GRAPHICS_SHADER_RECOMPILER_IR_PASSES_SRTNATIVE_H_
