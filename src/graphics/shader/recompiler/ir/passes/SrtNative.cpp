#include "graphics/shader/recompiler/ir/passes/SrtNative.h"

#include "common/assert.h"
#include "common/config.h"
#include "graphics/shader/recompiler/ir/passes/SrtNativeAsm.h"
#include "graphics/shader/recompiler/ir/passes/SrtWalker.h"

#include <bit>
#include <cstddef>
#include <cstring>


namespace Libs::Graphics::ShaderRecompiler::IR {

namespace {

using namespace X64;

// SrtNativeFrame field offsets, as the generated code addresses them.
constexpr int32_t FrameMemo         = 0;
constexpr int32_t FrameGeneration   = 8;
constexpr int32_t FrameUserData     = 16;
constexpr int32_t FrameUserDataSize = 24;
constexpr int32_t FrameShaderBase   = 32;
constexpr int32_t FrameClean        = 48;
constexpr int32_t FrameFailed       = 56;
constexpr int32_t FrameFastRead     = 64;
constexpr int32_t FrameBufferViews  = 72;
constexpr int32_t FrameBulkReads    = 80;
static_assert(offsetof(SrtNativeFrame, memo) == FrameMemo);
static_assert(offsetof(SrtNativeFrame, generation) == FrameGeneration);
static_assert(offsetof(SrtNativeFrame, user_data) == FrameUserData);
static_assert(offsetof(SrtNativeFrame, user_data_size) == FrameUserDataSize);
static_assert(offsetof(SrtNativeFrame, shader_base) == FrameShaderBase);
static_assert(offsetof(SrtNativeFrame, clean) == FrameClean);
static_assert(offsetof(SrtNativeFrame, failed) == FrameFailed);
static_assert(offsetof(SrtNativeFrame, fast_read) == FrameFastRead);
static_assert(offsetof(SrtNativeFrame, buffer_views) == FrameBufferViews);
static_assert(offsetof(SrtNativeFrame, bulk_reads) == FrameBulkReads);
static_assert(sizeof(ResourcePlan::EvaluationContext::BufferView) == 24);
static_assert(offsetof(SrtReadSpan, base) == 0);
static_assert(offsetof(SrtReadSpan, data) == 8);
static_assert(offsetof(SrtReadSpan, last_dword_offset) == 16);
static_assert(sizeof(ResourcePlan::EvaluationContext::Entry) == 16);

constexpr uint64_t AddressMask = 0x0000ffffffffffffull;

// Integer argument registers and the shadow space of the host C ABI, for helper calls.
#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
constexpr Reg     Arg0   = RCX;
constexpr Reg     Arg1   = RDX;
constexpr Reg     Arg2   = R8;
constexpr Reg     Arg3   = R9;
constexpr int32_t Shadow = 32;
#else
constexpr Reg     Arg0   = RDI;
constexpr Reg     Arg1   = RSI;
constexpr Reg     Arg2   = RDX;
constexpr Reg     Arg3   = RCX;
constexpr int32_t Shadow = 0;
#endif

bool ImmediateBits(Value value, uint64_t& bits) {
	switch (value.GetType()) {
		case Type::U1: bits = value.U1(); return true;
		case Type::U8: bits = value.U8(); return true;
		case Type::U16: bits = value.U16(); return true;
		case Type::U32: bits = value.U32(); return true;
		case Type::U64: bits = value.U64(); return true;
		case Type::F32: bits = std::bit_cast<uint32_t>(value.F32Value()); return true;
		default: return false;
	}
}

bool IsRawRead(const ResourcePlan& program, const Inst& inst) {
	const auto op = inst.GetOpcode();
	if (op != ValueOpcode::LoadAddressU32 && op != ValueOpcode::ReadConstBuffer) {
		return false;
	}
	const auto index = inst.Flags<MemoryFlags>().index;
	if (index >= program.memory_info.size()) {
		return false;
	}
	const auto kind = program.memory_info[index].kind;
	return (op == ValueOpcode::LoadAddressU32 && kind == ResourceKind::ScalarAddress) ||
	       (op == ValueOpcode::ReadConstBuffer && kind == ResourceKind::ScalarBuffer);
}

} // namespace

// Helpers the generated code calls. They are members so that they can reach the walker.
struct SrtNativeHelpers {
	// 0 unsupported, 1 complete, 2 the same read failure the scalar path would have returned.
	// Resolve only immutable inputs or existing memos. At most the first cold word invokes
	// the original reader, in order; later words use only the span that reader authorizes.
	static uint32_t TryBulk(SrtNativeFrame* frame, const SrtBulkReadPlan* plan, uint32_t* flat) noexcept {
		bool all_cached = true;
		uint32_t first_cold = plan->count;
		for (uint32_t i = 0; i < plan->count; ++i) {
			const auto state = frame->memo[plan->words[i].memo_index].generation;
			if (state == frame->generation + 1) return 0;
			all_cached &= state == frame->generation;
			if (state != frame->generation && first_cold == plan->count) first_cold = i;
		}
		if (all_cached) {
			for (uint32_t i = 0; i < plan->count; ++i)
				flat[plan->words[i].flat_offset] = static_cast<uint32_t>(frame->memo[plan->words[i].memo_index].value);
			return 1;
		}
		const auto* authorized_span = frame->fast_read != nullptr ? frame->fast_read : frame->captured_bulk_read;
		if (frame->buffer_views == nullptr || authorized_span == nullptr) return 0;
		auto& view = frame->buffer_views[plan->view_index];
		if (view.generation != frame->generation) {
			uint64_t values[4] {};
			for (uint32_t i = 0; i < 4; ++i) {
				const auto& source = plan->sources[i];
				switch (source.kind) {
					case SrtBulkReadPlan::Source::Kind::Immediate: values[i] = source.value; break;
					case SrtBulkReadPlan::Source::Kind::UserData:
						if (source.index >= frame->user_data_size) return 0;
						values[i] = frame->user_data[source.index]; break;
					case SrtBulkReadPlan::Source::Kind::Memo:
					case SrtBulkReadPlan::Source::Kind::CleanMemo: {
						const auto* target = source.kind == SrtBulkReadPlan::Source::Kind::CleanMemo && frame->clean != nullptr
						    ? frame->clean : frame;
						if (target->memo[source.index].generation != target->generation) return 0;
						values[i] = target->memo[source.index].value; break;
					}
					default: return 0;
				}
			}
			const uint32_t high = static_cast<uint32_t>(values[1]);
			const uint64_t stride = (high >> 16) & 0x3fff;
			view.base = ((uint64_t(high) << 32) | static_cast<uint32_t>(values[0])) & AddressMask;
			view.size = uint64_t(static_cast<uint32_t>(values[2])) * (stride != 0 ? stride : 1);
			view.generation = frame->generation;
		}
		if (plan->maximum > view.size || view.size - plan->maximum < 4) return 0;
		const auto& span = *authorized_span;
		const uint64_t address = (view.base & ~uint64_t(3)) + plan->minimum;
		auto serves = [&] {
			const uint64_t offset = address - span.base;
			return span.data != nullptr && offset <= span.last_dword_offset &&
			    plan->maximum - plan->minimum <= span.last_dword_offset - offset;
		};
		if (!serves()) {
			for (uint32_t i = 0; i < first_cold; ++i)
				flat[plan->words[i].flat_offset] = static_cast<uint32_t>(frame->memo[plan->words[i].memo_index].value);
			const auto& word = plan->words[first_cold];
			auto& entry = frame->memo[word.memo_index];
			entry.generation = frame->generation + 1;
			uint64_t value = 0;
			if (!Read(frame, address + word.offset, view.base, &value)) {
				entry.generation = 0;
				if (*frame->failed == nullptr) *frame->failed = plan->instructions[first_cold];
				return 2;
			}
			entry.value = value; entry.generation = frame->generation;
			flat[word.flat_offset] = static_cast<uint32_t>(value);
			if (!serves()) return 0; // the scalar fallback sees the first word's memo, not a second read
		}
		const uint64_t offset = address - span.base;
		for (uint32_t i = 0; i < plan->count; ++i) {
			const auto& word = plan->words[i];
			auto& entry = frame->memo[word.memo_index];
			if (entry.generation != frame->generation) {
				uint32_t value;
				std::memcpy(&value, span.data + offset + word.offset, sizeof(value));
				if (frame->captured_reads != nullptr)
					frame->captured_reads->emplace_back(address + word.offset, sizeof(value));
				entry.value = value; entry.generation = frame->generation;
			}
			flat[word.flat_offset] = static_cast<uint32_t>(entry.value);
		}
		return 1;
	}
	// One instruction through the interpreter; its own memo entry is the routine's to fill.
	static bool Interpret(SrtNativeFrame* frame, const Inst* inst, uint64_t* result) {
		return frame->walker->EvaluateInst(*inst, *result);
	}
	// A value outside the compiled set (not in the plan's storage), memo and all.
	static bool EvaluateValue(SrtNativeFrame* frame, const Inst* inst, uint64_t* result) {
		return frame->walker->EvaluateWide(Value(const_cast<Inst*>(inst)), *result);
	}
	// SrtWalker::EvaluateRawRead's read, after the address computation.
	static bool Read(SrtNativeFrame* frame, uint64_t address, uint64_t base, uint64_t* result) {
		auto&    walker = *frame->walker;
		uint32_t word   = 0;
		if (walker.m_runtime.read_memory != nullptr) {
			if (!walker.m_runtime.read_memory(walker.m_runtime.userdata, address, {&word, 1})) {
				if (base == 0) {
					*result = 0;
					return true;
				}
				walker.m_read_failure         = "guest memory unreadable";
				walker.m_read_failure_address = address;
				walker.m_read_failure_offset  = 0;
				walker.m_read_failure_size    = 0;
				return false;
			}
		} else {
			std::memcpy(&word, reinterpret_cast<const void*>(address), sizeof(word));
		}
		*result = word;
		return true;
	}
};

namespace {

class Compiler {
public:
	explicit Compiler(const ResourcePlan& program, bool value_aliases, bool inline_user_data, bool canonical_views,
	                  bool eager_flat)
	    : m_program(program), m_value_aliases(value_aliases), m_inline_user_data(inline_user_data),
	      m_canonical_views(canonical_views), m_eager_flat(eager_flat) {}

	bool Run(std::vector<uint32_t> (&routines)[2],
	         std::vector<uint32_t> (&descriptors)[2], std::vector<uint32_t> (&flat)[2],
	         std::array<uint32_t,2>& buffer_groups,
	         uint32_t& batch_entry,uint32_t& group_entry, uint32_t& instructions, uint32_t& interpreted) {
		// Every instruction gets its evaluation index now, so the count is final.
		for (const auto& inst: m_program.value_storage) {
			const auto index = inst.EvaluationIndex(m_program.evaluation_value_count);
			if (index >= m_insts.size()) {
				m_insts.resize(index + 1, nullptr);
			}
			m_insts[index] = &inst;
		}
		for (auto& labels: m_labels) {
			labels.resize(m_insts.size());
			for (auto& label: labels) {
				label = m_asm.NewLabel();
			}
		}
		for (const auto* inst: m_insts) {
			if (inst == nullptr) continue;
			if (const auto* handle = BufferViewHandle(*inst); handle != nullptr && !m_view_labels.contains(handle)) {
				const auto prior = std::ranges::find_if(m_view_handles, [&](const auto* other) {
					return SameViewArguments(*other, *handle);
				});
				if (prior != m_view_handles.end()) {
					m_view_labels.emplace(handle, m_view_labels.at(*prior));
					m_view_slots.emplace(handle, m_view_slots.at(*prior));
					continue;
				}
				m_view_labels.emplace(handle, std::array<Label, 2> {m_asm.NewLabel(), m_asm.NewLabel()});
				m_view_slots.emplace(handle, static_cast<uint32_t>(m_view_handles.size()));
				m_view_handles.push_back(handle);
			}
		}
		EmitEntry();
		batch_entry = static_cast<uint32_t>(m_asm.Size());
		EmitBatchEntry();
		group_entry=static_cast<uint32_t>(m_asm.Size());
		EmitBatchEntry(true);
		for (uint32_t mode = 0; mode < 2; mode++) {
			routines[mode].assign(m_insts.size(), UINT32_MAX);
			for (size_t index = 0; index < m_insts.size(); index++) {
				if (m_insts[index] == nullptr) continue;
				routines[mode][index] = static_cast<uint32_t>(m_asm.Size());
				EmitRoutine(static_cast<SrtNativeMode>(mode), *m_insts[index]);
			}
			EmitBatches(static_cast<SrtNativeMode>(mode), descriptors[mode], flat[mode],buffer_groups[mode]);
			for (const auto* handle: m_view_handles) EmitBufferView(static_cast<SrtNativeMode>(mode), *handle);
		}
		instructions = static_cast<uint32_t>(m_insts.size());
		interpreted  = m_interpreted / 2;
		return m_asm.Finish();
	}

	[[nodiscard]] const Assembler& Asm() const { return m_asm; }
	[[nodiscard]] uint32_t BufferViewCount() const { return static_cast<uint32_t>(m_view_handles.size()); }
	[[nodiscard]] uint32_t BulkReadRoots() const { return m_bulk_roots / 2; }
	std::vector<std::unique_ptr<SrtBulkReadPlan>> TakeBulkPlans() { return std::move(m_bulk_plans); }

private:
	// entry(frame, routine, result) -> bool, in the host C ABI.
	void EmitEntry() {
		auto& a = m_asm;
		a.Push(RBX), a.Push(RBP), a.Push(R12), a.Push(R13), a.Push(R14), a.Push(R15);
		a.MovRR(R15, Arg2);
		a.MovRR(R11, Arg1);
		a.MovRR(R12, Arg0);
		a.Load(R13, R12, FrameMemo);
		a.Load(R14, R12, FrameGeneration);
		a.CallReg(R11);
		const auto failed = a.NewLabel();
		const auto done   = a.NewLabel();
		a.Jcc(CondB, failed);
		a.Store(R15, 0, RAX);
		a.MovRI(RAX, 1);
		a.Jmp(done);
		a.Bind(failed);
		a.MovRI(RAX, 0);
		a.Bind(done);
		a.Pop(R15), a.Pop(R14), a.Pop(R13), a.Pop(R12), a.Pop(RBP), a.Pop(RBX);
		a.Ret();
	}

	void EmitBatchEntry(bool return_count=false) {
		auto& a = m_asm;
		a.Push(RBX), a.Push(RBP), a.Push(R12), a.Push(R13), a.Push(R14), a.Push(R15);
		a.MovRR(R15, Arg2);
		a.MovRR(R11, Arg1);
		a.MovRR(R12, Arg0);
		a.Load(R13, R12, FrameMemo);
		a.Load(R14, R12, FrameGeneration);
		if (return_count) a.MovRR(RBX,Arg3);
		a.CallReg(R11);
		if (return_count) {
			a.Pop(R15),a.Pop(R14),a.Pop(R13),a.Pop(R12),a.Pop(RBP),a.Pop(RBX);
			a.Ret();return;
		}
		const auto failed = a.NewLabel(), done = a.NewLabel();
		a.Jcc(CondB, failed);
		a.MovRI(RAX, 1), a.Jmp(done);
		a.Bind(failed), a.MovRI(RAX, 0);
		a.Bind(done);
		a.Pop(R15), a.Pop(R14), a.Pop(R13), a.Pop(R12), a.Pop(RBP), a.Pop(RBX);
		a.Ret();
	}

	template <typename Body> void EmitBatch(Body&& body) {
		const auto fail = m_asm.NewLabel();
		body(fail);
		m_asm.Clc(), m_asm.Ret();
		m_asm.Bind(fail), m_asm.Stc(), m_asm.Ret();
	}
	void EmitBatches(SrtNativeMode mode, std::vector<uint32_t>& descriptors,
	                 std::vector<uint32_t>& flat,uint32_t& buffer_group) {
		std::vector<Label> descriptor_labels;
		for (const auto& source: m_program.descriptor_sources) {
			descriptor_labels.push_back(m_asm.NewLabel());
			m_asm.Bind(descriptor_labels.back());
			descriptors.push_back(static_cast<uint32_t>(m_asm.Size()));
			EmitBatch([&](Label fail) {
				if (source.dword_count > 8u) { m_asm.Jmp(fail); return; }
				std::array<uint32_t, 8> indices;
				for (uint32_t i = 0; i < source.dword_count; ++i) indices[i] = DescriptorFlatSlot(source.dwords[i]);
				const auto emit_word = [&](uint32_t i) {
					EmitValue(mode, source.dwords[i], fail);
					m_asm.Store32(R15, static_cast<int32_t>(i * 4u), RAX);
				};
				for (uint32_t i = 0; i < source.dword_count;) {
					const auto count = m_canonical_views
					    ? GatherReadCount(std::span(indices).subspan(i, source.dword_count - i), true) : 0;
					if (count < 4) { emit_word(i++); continue; }
					const auto legacy = m_asm.NewLabel(), done = m_asm.NewLabel();
					EmitGatherReads(mode, std::span(indices).subspan(i, count), legacy, fail, true, i);
					m_asm.Jmp(done);
					m_asm.Bind(legacy);
					for (uint32_t j = 0; j < count; ++j) emit_word(i + j);
					m_asm.Bind(done);
					i += static_cast<uint32_t>(count);
				}
			});
		}
		// Reuse the existing descriptor routines, preserving read order and memo
		// behavior. Only the C ABI boundary is fused, not the evaluations themselves.
		const bool readonly_group=m_program.info.buffers.size()>=4 && m_program.info.buffers.size()<=64 &&
		    std::ranges::all_of(m_program.info.buffers,[&](const auto& buffer) {
			    return !buffer.written && buffer.source<descriptor_labels.size() &&
			        m_program.descriptor_sources[buffer.source].dword_count==4;
		    });
		buffer_group=UINT32_MAX;
		if (readonly_group) {
			static_assert(offsetof(DescriptorValue,dwords)==0 && offsetof(DescriptorValue,dword_count)==32 && sizeof(DescriptorValue)==36);
			buffer_group=static_cast<uint32_t>(m_asm.Size());
			std::vector<Label> failures;
			m_asm.Push(RBP);m_asm.MovRR(RBP,R15);m_asm.Push(RBX);
			for (const auto& buffer:m_program.info.buffers) {
				failures.push_back(m_asm.NewLabel());
				m_asm.MovRI(RAX,0);
				for (int32_t offset=0;offset<32;offset+=8) m_asm.Store(R15,offset,RAX);
				m_asm.MovRI(RAX,4);m_asm.Store32(R15,32,RAX);
				const auto inactive=m_asm.NewLabel();
				m_asm.MovRR(RAX,RBX);m_asm.AndI(RAX,1);m_asm.Jcc(CondE,inactive);
				m_asm.Call(descriptor_labels[buffer.source]);m_asm.Jcc(CondB,failures.back());
				m_asm.Bind(inactive);m_asm.ShrI(RBX,1);
				m_asm.AddI(R15,sizeof(DescriptorValue));
			}
			m_asm.MovRR(R15,RBP);m_asm.Pop(RBX);m_asm.Pop(RBP);m_asm.MovRI(RAX,m_program.info.buffers.size());m_asm.Ret();
			for (size_t i=0;i<failures.size();++i) {
				m_asm.Bind(failures[i]);m_asm.MovRR(R15,RBP);m_asm.Pop(RBX);m_asm.Pop(RBP);m_asm.MovRI(RAX,i);m_asm.Ret();
			}
		}
		const auto emit_read = [&](uint32_t index, Label fail) {
			if (index >= m_program.srt_reads.size()) { m_asm.Jmp(fail); return; }
			const auto& read = m_program.srt_reads[index];
			if (read.flat_offset >= m_program.srt_reads.size()) { m_asm.Jmp(fail); return; }
			if (!m_eager_flat && !m_program.gpu_flat_slots.empty() && read.flat_offset < m_program.gpu_flat_slots.size() &&
			    m_program.gpu_flat_slots[read.flat_offset] == 0u) {
				m_asm.MovRI(RAX, 0);
				m_asm.Store32(R15, static_cast<int32_t>(read.flat_offset * 4u), RAX);
				return;
			}
			if (read.flat_offset < m_program.clean_flat_slots.size() &&
			    m_program.clean_flat_slots[read.flat_offset] != 0u)
				EmitCleanValue(mode, read.value, fail);
			else EmitValue(mode, read.value, fail);
			m_asm.Store32(R15, static_cast<int32_t>(read.flat_offset * 4u), RAX);
		};
		const auto emit_list = [&](std::span<const uint32_t> indices, Label fail) {
			for (size_t first = 0; first < indices.size();) {
				const auto count = GatherReadCount(indices.subspan(first));
				if (count >= 4) {
					const auto group = indices.subspan(first, count);
					const auto legacy = m_asm.NewLabel(), done = m_asm.NewLabel();
					EmitGatherReads(mode, group, legacy, fail);
					m_asm.Jmp(done);
					m_asm.Bind(legacy);
					for (const auto index: group) emit_read(index, fail);
					m_asm.Bind(done);
					first += count;
				} else emit_read(indices[first++], fail);
			}
		};
		flat.push_back(static_cast<uint32_t>(m_asm.Size()));
		EmitBatch([&](Label fail) {
			if (m_program.control_flow.empty()) {
				std::vector<uint32_t> indices(m_program.srt_reads.size());
				for (uint32_t i = 0; i < indices.size(); ++i) indices[i] = i;
				emit_list(indices, fail);
			}
		});
		for (const auto& block: m_program.control_flow) {
			flat.push_back(static_cast<uint32_t>(m_asm.Size()));
			EmitBatch([&](Label fail) { emit_list(block.srt_reads, fail); });
		}
	}

	[[nodiscard]] bool Compiled(const Inst* inst) const {
		const auto index = inst->EvaluationIndex(m_program.evaluation_value_count);
		return index < m_insts.size() && m_insts[index] == inst;
	}

	Label RoutineLabel(SrtNativeMode mode, const Inst* inst) const {
		return m_labels[static_cast<uint32_t>(mode)]
		               [inst->EvaluationIndex(m_program.evaluation_value_count)];
	}

	// Calls a C helper: aligns the stack, keeps RBX (callee-saved) as the saved stack pointer.
	// `setup` loads the arguments; the result is the helper's bool in RCX (0/1) and its uint64_t
	// output in RAX. Reloads the memo base, which an interpreter call may have moved.
	template <typename Setup>
	void EmitHelperCall(const void* helper, Setup&& setup) {
		auto& a = m_asm;
		a.Push(RBX);
		a.MovRR(RBX, RSP);
		a.AndI(RSP, -16);
		a.SubI(RSP, Shadow + 16);
		setup();
		a.CallAbs(helper);
		a.MovzxB(RCX, RAX);
		a.Load(RAX, RSP, Shadow);
		a.MovRR(RSP, RBX);
		a.Pop(RBX);
		a.Load(R13, R12, FrameMemo);
	}

	// Value -> RAX; jumps to `fail` when it cannot be evaluated.
	void EmitValue(SrtNativeMode mode, Value value, Label fail) {
		auto& a = m_asm;
		value   = value.Resolve();
		if (value.IsImmediate()) {
			uint64_t bits = 0;
			if (ImmediateBits(value, bits)) {
				a.MovRI(RAX, bits);
			} else {
				a.Jmp(fail);
			}
			return;
		}
		const auto* inst = value.TryInstruction();
		if (m_value_aliases && inst != nullptr && Compiled(inst)) {
			if (m_inline_user_data && inst->GetOpcode() == ValueOpcode::GetUserData && inst->Arg(0).GetType() == Type::ScalarReg) {
				EmitBody(mode, *inst, fail);
				return;
			}
			if (inst->GetOpcode() == ValueOpcode::ReadConst) {
				const auto slot = inst->Arg(1).Resolve();
				if (slot.IsImmediate() && slot.GetType() == Type::U32 && slot.U32() < m_program.srt_reads.size()) {
					const auto root = m_program.srt_reads[slot.U32()].value.Resolve();
					const auto* target = root.TryInstruction();
					if (root.IsImmediate() || (target != nullptr && target->GetOpcode() != ValueOpcode::ReadConst)) {
						if (mode == SrtNativeMode::Split && slot.U32() < m_program.clean_flat_slots.size() &&
						    m_program.clean_flat_slots[slot.U32()] != 0)
							EmitCleanValue(mode, root, fail);
						else EmitValue(mode, root, fail);
						return;
					}
				}
			}
		}
		if (!Compiled(inst)) {
			EmitHelperCall(reinterpret_cast<const void*>(&SrtNativeHelpers::EvaluateValue), [&] {
				a.MovRR(Arg0, R12);
				a.MovRI(Arg1, reinterpret_cast<uint64_t>(inst));
				a.Lea(Arg2, RSP, Shadow);
			});
			a.Test(RCX, RCX);
			a.Jcc(CondE, fail);
			return;
		}
		a.Call(RoutineLabel(mode, inst));
		a.Jcc(CondB, fail);
	}

	// A value the interpreter evaluates in the clean walker when there is one: select predicates
	// and clean SRT slots. In Split mode that is the clean frame, which runs Self code.
	void EmitCleanValue(SrtNativeMode mode, Value value, Label fail) {
		if (mode == SrtNativeMode::Self) {
			EmitValue(mode, value, fail);
			return;
		}
		auto& a        = m_asm;
		const auto res = value.Resolve();
		if (res.IsImmediate()) {
			EmitValue(SrtNativeMode::Self, res, fail);
			return;
		}
		if (!Compiled(res.TryInstruction())) {
			// An uncompiled value goes through the clean walker's interpreter.
			a.Push(R12), a.Push(R13), a.Push(R14);
			a.Load(R12, R12, FrameClean);
			a.Load(R13, R12, FrameMemo);
			a.Load(R14, R12, FrameGeneration);
			const auto failed = a.NewLabel();
			const auto done   = a.NewLabel();
			EmitValue(SrtNativeMode::Self, res, failed);
			a.Pop(R14), a.Pop(R13), a.Pop(R12);
			a.Jmp(done);
			a.Bind(failed);
			a.Pop(R14), a.Pop(R13), a.Pop(R12);
			a.Jmp(fail);
			a.Bind(done);
			return;
		}
		a.Push(R12), a.Push(R13), a.Push(R14);
		a.Load(R12, R12, FrameClean);
		a.Load(R13, R12, FrameMemo);
		a.Load(R14, R12, FrameGeneration);
		a.Call(RoutineLabel(SrtNativeMode::Self, res.TryInstruction()));
		// POP leaves the flags alone, so the callee's CF survives.
		a.Pop(R14), a.Pop(R13), a.Pop(R12);
		a.Jcc(CondB, fail);
	}

	// Evaluates `count` operands in order into RAX, RCX, RDX, R8 (the interpreter's order, which
	// stops at the first failure).
	void EmitOperands(SrtNativeMode mode, const Inst& inst, size_t count, Label fail) {
		static constexpr Reg Targets[4] = {RAX, RCX, RDX, R8};
		auto&                a          = m_asm;
		uint64_t             immediates[4] {};
		bool                 immediate[4] {};
		for (size_t i = 0; i < count; i++) {
			const auto value = inst.Arg(i).Resolve();
			if (value.IsImmediate()) {
				if (!ImmediateBits(value, immediates[i])) {
					a.Jmp(fail);
					return;
				}
				immediate[i] = true;
				continue;
			}
			EmitValue(mode, value, fail);
			a.Push(RAX);
		}
		for (size_t i = count; i-- > 0;) {
			if (!immediate[i]) a.Pop(Targets[i]);
		}
		for (size_t i = 0; i < count; i++) {
			if (immediate[i]) a.MovRI(Targets[i], immediates[i]);
		}
	}

	void EmitInterpret(const Inst& inst, Label fail) {
		auto& a = m_asm;
		m_interpreted++;
		EmitHelperCall(reinterpret_cast<const void*>(&SrtNativeHelpers::Interpret), [&] {
			a.MovRR(Arg0, R12);
			a.MovRI(Arg1, reinterpret_cast<uint64_t>(&inst));
			a.Lea(Arg2, RSP, Shadow);
		});
		a.Test(RCX, RCX);
		a.Jcc(CondE, fail);
	}

	void EmitCompare(SrtNativeMode mode, const Inst& inst, Cond cond, Label fail) {
		auto& a = m_asm;
		EmitOperands(mode, inst, 2, fail);
		a.Cmp32(RAX, RCX);
		a.SetCC(cond, RAX);
		a.MovzxB(RAX, RAX);
	}

	void EmitBool(Reg reg) {
		m_asm.Test(reg, reg);
		m_asm.SetCC(CondNE, reg);
		m_asm.MovzxB(reg, reg);
	}

	// SrtWalker::EvaluateRawRead.
	const Inst* BufferViewHandle(const Inst& inst) const {
		if (inst.GetOpcode() != ValueOpcode::ReadConstBuffer || !IsRawRead(m_program, inst)) return nullptr;
		const auto* handle = inst.Arg(0).Resolve().TryInstruction();
		const auto offset_value = inst.Arg(1).Resolve();
		uint64_t offset = 0;
		if (handle == nullptr || handle->NumArgs() != 4 || !Compiled(handle) ||
		    !offset_value.IsImmediate() || !ImmediateBits(offset_value, offset) ||
		    static_cast<int32_t>(m_program.memory_info[inst.Flags<MemoryFlags>().index].offset) < 0)
			return nullptr;
		return handle;
	}

	uint64_t ReadRelativeOffset(const Inst& inst) const {
		uint64_t offset = 0; (void)ImmediateBits(inst.Arg(1).Resolve(), offset);
		return (uint64_t(m_program.memory_info[inst.Flags<MemoryFlags>().index].offset) & ~uint64_t(3)) +
		    (uint64_t(static_cast<uint32_t>(offset)) & ~uint64_t(3));
	}
	SrtBulkReadPlan::Source BulkSource(Value value) const {
		SrtBulkReadPlan::Source source;
		bool clean = false;
		for (uint32_t depth = 0; depth < 8; ++depth) {
			value = value.Resolve();
			if (value.IsImmediate()) {
				if (ImmediateBits(value, source.value)) source.kind = SrtBulkReadPlan::Source::Kind::Immediate;
				return source;
			}
			const auto* inst = value.TryInstruction();
			if (inst == nullptr || !Compiled(inst)) return source;
			if (inst->GetOpcode() == ValueOpcode::ReadConst) {
				const auto slot = inst->Arg(1).Resolve();
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 || slot.U32() >= m_program.srt_reads.size()) return source;
				clean |= slot.U32() < m_program.clean_flat_slots.size() && m_program.clean_flat_slots[slot.U32()] != 0;
				value = m_program.srt_reads[slot.U32()].value;
				continue;
			}
			if (inst->GetOpcode() == ValueOpcode::GetUserData && inst->Arg(0).GetType() == Type::ScalarReg &&
			    RegIndex(inst->Arg(0).ScalarRegister()) >= m_program.user_data_base) {
				source.kind = SrtBulkReadPlan::Source::Kind::UserData;
				source.index = RegIndex(inst->Arg(0).ScalarRegister()) - m_program.user_data_base;
			} else {
				source.kind = clean ? SrtBulkReadPlan::Source::Kind::CleanMemo : SrtBulkReadPlan::Source::Kind::Memo;
				source.index = inst->EvaluationIndex(m_program.evaluation_value_count);
			}
			return source;
		}
		return source;
	}
	bool CleanRead(uint32_t index) const {
		const auto flat = m_program.srt_reads[index].flat_offset;
		return flat < m_program.clean_flat_slots.size() && m_program.clean_flat_slots[flat] != 0;
	}
	bool SameViewArguments(const Inst& a, const Inst& b) const {
		for (uint32_t i = 0; i < 4; ++i) {
			const auto left = a.Arg(i).Resolve(), right = b.Arg(i).Resolve();
			if (left == right) continue;
			if (!m_canonical_views || left.GetType() != right.GetType()) return false;
			const auto x = BulkSource(left), y = BulkSource(right);
			// This is identity of immutable inputs / memo slots, never equivalence of
			// two memory operations that merely happen to have the same address.
			if (x.kind == SrtBulkReadPlan::Source::Kind::Invalid || x.kind != y.kind ||
			    x.index != y.index || x.value != y.value) return false;
		}
		return true;
	}
	uint32_t DescriptorFlatSlot(Value value) const {
		const auto* inst = value.Resolve().TryInstruction();
		if (inst == nullptr || inst->GetOpcode() != ValueOpcode::ReadConst) return UINT32_MAX;
		const auto slot = inst->Arg(1).Resolve();
		if (!slot.IsImmediate() || slot.GetType() != Type::U32 || slot.U32() >= m_program.srt_reads.size() ||
		    m_program.srt_reads[slot.U32()].flat_offset != slot.U32()) return UINT32_MAX;
		return slot.U32();
	}
	const Inst* GroupRead(uint32_t index, bool descriptor = false) const {
		if (index >= m_program.srt_reads.size()) return nullptr;
		const auto& read = m_program.srt_reads[index];
		if (read.flat_offset >= m_program.srt_reads.size() ||
		    (!descriptor && !m_eager_flat && !m_program.gpu_flat_slots.empty() && read.flat_offset < m_program.gpu_flat_slots.size() &&
		     m_program.gpu_flat_slots[read.flat_offset] == 0)) return nullptr;
		const auto* inst = read.value.Resolve().TryInstruction();
		return inst != nullptr && BufferViewHandle(*inst) != nullptr ? inst : nullptr;
	}
	size_t GatherReadCount(std::span<const uint32_t> indices, bool descriptor = false) const {
		const auto* first = GroupRead(indices[0], descriptor);
		if (first == nullptr) return 0;
		const auto* handle = BufferViewHandle(*first);
		auto minimum = ReadRelativeOffset(*first), maximum = minimum;
		const bool clean = CleanRead(indices[0]);
		size_t count = 1;
		while (count < std::min(indices.size(), size_t(8))) {
			const auto* next = GroupRead(indices[count], descriptor);
			if (next == nullptr || CleanRead(indices[count]) != clean) break;
			const auto offset = ReadRelativeOffset(*next);
			const auto next_min = std::min(minimum, offset), next_max = std::max(maximum, offset);
			if (next_max - next_min > 4092) break;
			bool duplicate_node = false;
			for (size_t prior = 0; prior < count; ++prior) duplicate_node |= GroupRead(indices[prior], descriptor) == next;
			if (duplicate_node) break;
			const auto* other = BufferViewHandle(*next);
			if (!SameViewArguments(*handle, *other)) break;
			minimum = next_min; maximum = next_max;
			++count;
		}
		return count;
	}

	// Keep group metadata in data rather than duplicating a large copy/memo routine. The
	// original reader may establish the span; unsupported state retains its scalar fallback.
	void EmitGatherReads(SrtNativeMode mode, std::span<const uint32_t> indices, Label legacy, Label fail,
	                     bool descriptor = false, uint32_t output_start = 0) {
		auto& a = m_asm;
		m_bulk_roots += static_cast<uint32_t>(indices.size());
		auto plan = std::make_unique<SrtBulkReadPlan>();
		const auto* handle = BufferViewHandle(*GroupRead(indices[0], descriptor));
		plan->view_index = m_view_slots.at(handle);
		for (uint32_t i = 0; i < 4; ++i) plan->sources[i] = BulkSource(handle->Arg(i));
		plan->count = static_cast<uint32_t>(indices.size());
		plan->minimum = plan->maximum = ReadRelativeOffset(*GroupRead(indices[0], descriptor));
		for (const auto index: indices) {
			const auto offset = ReadRelativeOffset(*GroupRead(index, descriptor));
			plan->minimum = std::min(plan->minimum, offset);
			plan->maximum = std::max(plan->maximum, offset);
		}
		for (size_t i = 0; i < indices.size(); ++i) {
			const auto* inst = GroupRead(indices[i], descriptor);
			plan->words[i] = {inst->EvaluationIndex(m_program.evaluation_value_count),
			    descriptor ? output_start + static_cast<uint32_t>(i) : m_program.srt_reads[indices[i]].flat_offset,
			    static_cast<uint32_t>(ReadRelativeOffset(*inst) - plan->minimum)};
			plan->instructions[i] = inst;
		}
		const auto* metadata = plan.get();
		m_bulk_plans.push_back(std::move(plan));
		const bool switch_clean = mode == SrtNativeMode::Split && CleanRead(indices[0]);
		const auto unavailable = a.NewLabel(), failed = a.NewLabel(), done = a.NewLabel();
		if (switch_clean) {
			a.Push(R12); a.Push(R13); a.Push(R14);
			a.Load(R12, R12, FrameClean); a.Load(R13, R12, FrameMemo); a.Load(R14, R12, FrameGeneration);
		}
		a.Load(RAX, R12, FrameBulkReads); a.Test(RAX, RAX); a.Jcc(CondE, unavailable);
		a.Push(RBX); a.MovRR(RBX, RSP); a.AndI(RSP, -16); a.SubI(RSP, Shadow + 16);
		a.MovRR(Arg0, R12); a.MovRI(Arg1, reinterpret_cast<uint64_t>(metadata)); a.MovRR(Arg2, R15);
		a.CallAbs(reinterpret_cast<const void*>(&SrtNativeHelpers::TryBulk));
		a.MovzxB(RAX, RAX); a.MovRR(RSP, RBX); a.Pop(RBX); a.Load(R13, R12, FrameMemo);
		a.Test(RAX, RAX); a.Jcc(CondE, unavailable);
		a.CmpI(RAX, 1); a.Jcc(CondNE, failed);
		if (switch_clean) { a.Pop(R14); a.Pop(R13); a.Pop(R12); }
		a.Jmp(done);
		a.Bind(unavailable);
		if (switch_clean) { a.Pop(R14); a.Pop(R13); a.Pop(R12); }
		a.Jmp(legacy);
		a.Bind(failed);
		if (switch_clean) { a.Pop(R14); a.Pop(R13); a.Pop(R12); }
		a.Jmp(fail);
		a.Bind(done);
	}

	// A constant offset has no read side effects. Resolve low/high/records/word3 once, in their
	// original order, and keep the view in this walker's generation (never across evaluations).
	void EmitBufferView(SrtNativeMode mode, const Inst& handle) {
		auto& a = m_asm;
		const auto slot = static_cast<int32_t>(m_view_slots.at(&handle) * 24u);
		const auto hit = a.NewLabel(), fail = a.NewLabel(), cycle = a.NewLabel();
		a.Bind(m_view_labels.at(&handle)[static_cast<uint32_t>(mode)]);
		a.Load(RCX, R12, FrameBufferViews);
		a.Load(RAX, RCX, slot + 16);
		a.Cmp(RAX, R14);
		a.Jcc(CondE, hit);
		a.Lea(RDX, R14, 1);
		a.Cmp(RAX, RDX);
		a.Jcc(CondE, cycle);
		a.Store(RCX, slot + 16, RDX);
		a.Push(RBP); a.MovRR(RBP, RSP);
		for (uint32_t i = 0; i < 4; ++i) {
			EmitValue(mode, handle.Arg(i), fail);
			if (i < 3) a.Push(RAX);
		}
		a.Load(RAX, RBP, -16); a.ShlI(RAX, 32);
		a.Load32(RCX, RBP, -8); a.Or(RAX, RCX);
		a.MovRI(RCX, AddressMask); a.And(RAX, RCX);
		a.Load32(RDX, RBP, -24);
		a.Load32(RCX, RBP, -16); a.ShrI(RCX, 16); a.AndI(RCX, 0x3fff);
		const auto sized = a.NewLabel();
		a.Test(RCX, RCX); a.Jcc(CondE, sized); a.Imul(RDX, RCX); a.Bind(sized);
		a.Load(RCX, R12, FrameBufferViews);
		a.Store(RCX, slot, RAX); a.Store(RCX, slot + 8, RDX); a.Store(RCX, slot + 16, R14);
		a.MovRR(RSP, RBP); a.Pop(RBP); a.Clc(); a.Ret();
		a.Bind(hit);
		a.Load(RAX, RCX, slot); a.Load(RDX, RCX, slot + 8); a.Clc(); a.Ret();
		a.Bind(fail);
		a.MovRR(RSP, RBP); a.Pop(RBP);
		a.Load(RCX, R12, FrameBufferViews); a.StoreImm32(RCX, slot + 16, 0);
		a.Bind(cycle); a.Stc(); a.Ret();
	}

	void EmitReadWithBufferView(SrtNativeMode mode, const Inst& inst, const Inst& handle, Label fail) {
		auto& a = m_asm;
		const auto legacy = a.NewLabel(), zero = a.NewLabel(), done = a.NewLabel();
		a.Load(RCX, R12, FrameBufferViews); a.Test(RCX, RCX); a.Jcc(CondE, legacy);
		a.Call(m_view_labels.at(&handle)[static_cast<uint32_t>(mode)]); a.Jcc(CondB, fail);
		uint64_t offset = 0;
		(void)ImmediateBits(inst.Arg(1).Resolve(), offset);
		const uint64_t relative = (uint64_t(m_program.memory_info[inst.Flags<MemoryFlags>().index].offset) & ~uint64_t(3)) +
		    (uint64_t(static_cast<uint32_t>(offset)) & ~uint64_t(3));
		a.MovRI(RCX, relative); a.Cmp(RCX, RDX); a.Jcc(CondA, zero);
		a.Sub(RDX, RCX); a.CmpI(RDX, 4); a.Jcc(CondB, zero);
		a.MovRR(R11, RAX); a.AndI(RAX, -4); a.Add(RAX, RCX);
		EmitRead(fail); a.Jmp(done);
		a.Bind(zero); a.MovRI(RAX, 0); a.Jmp(done);
		a.Bind(legacy); EmitRawRead(mode, inst, fail);
		a.Bind(done);
	}

	void EmitRawRead(SrtNativeMode mode, const Inst& inst, Label fail) {
		auto&      a     = m_asm;
		const auto flags = inst.Flags<MemoryFlags>();
		if (flags.index >= m_program.memory_info.size()) {
			a.Jmp(fail);
			return;
		}
		if (!inst.Arg(0).Resolve().TryInstruction()) {
			// The interpreter would stop the emulator here; leave that to it.
			EmitInterpret(inst, fail);
			return;
		}
		const auto& mem           = m_program.memory_info[flags.index];
		const auto& handle        = *inst.Arg(0).ResolveInstruction();
		const bool  const_buffer  = inst.GetOpcode() == ValueOpcode::ReadConstBuffer;
		const auto  immediate     = static_cast<int64_t>(static_cast<int32_t>(mem.offset));
		// Operands onto the stack in the interpreter's order: low, high, offset[, records, word3].
		// [RBP-8] low, [RBP-16] high, [RBP-24] offset, [RBP-32] records.
		if (handle.NumArgs() < 2u) {
			EmitInterpret(inst, fail);
			return;
		}
		EmitValue(mode, handle.Arg(0), fail);
		a.Push(RAX);
		EmitValue(mode, handle.Arg(1), fail);
		a.Push(RAX);
		EmitValue(mode, inst.Arg(1), fail);
		a.Push(RAX);
		if (const_buffer) {
			if (handle.NumArgs() != 4u) {
				a.Jmp(fail);
				return;
			}
			EmitValue(mode, handle.Arg(2), fail);
			a.Push(RAX);
			EmitValue(mode, handle.Arg(3), fail);
			if (immediate < 0) {
				a.Jmp(fail);
				return;
			}
		}
		// R11 = base = ((high << 32) | low32) & AddressMask.
		a.Load(R11, RBP, -16);
		a.ShlI(R11, 32);
		a.Load32(RCX, RBP, -8);
		a.Or(R11, RCX);
		a.MovRI(RCX, AddressMask);
		a.And(R11, RCX);
		const auto read = a.NewLabel();
		if (const_buffer) {
			const auto zero = a.NewLabel();
			// RCX = byte offset = (immediate & ~3) + (offset32 & ~3).
			a.Load32(RCX, RBP, -24);
			a.AndI32(RCX, ~3u);
			a.AddI(RCX, static_cast<int32_t>(immediate & ~int64_t {3}));
			// RDX = stride = (high32 >> 16) & 0x3fff; R8 = size.
			a.Load32(RDX, RBP, -16);
			a.ShrI(RDX, 16);
			a.AndI(RDX, 0x3fff);
			a.Load32(R8, RBP, -32);
			const auto sized = a.NewLabel();
			a.Test(RDX, RDX);
			a.Jcc(CondE, sized);
			a.Imul(R8, RDX);
			a.Bind(sized);
			// Past the end reads zero (PS5 ISA, scalar buffer addressing).
			a.Cmp(RCX, R8);
			a.Jcc(CondA, zero);
			a.MovRR(RDX, R8);
			a.Sub(RDX, RCX);
			a.CmpI(RDX, 4);
			a.Jcc(CondB, zero);
			a.MovRR(RAX, R11);
			a.AndI(RAX, -4);
			a.Add(RAX, RCX);
			a.Jmp(read);
			a.Bind(zero);
			a.MovRI(RAX, 0);
			const auto done = a.NewLabel();
			a.Jmp(done);
			a.Bind(read);
			EmitRead(fail);
			a.Bind(done);
			return;
		}
		// RCX = relative = (immediate & ~3) + (offset32 & ~3), signed.
		a.Load32(RCX, RBP, -24);
		a.AndI32(RCX, ~3u);
		a.MovRI(RDX, static_cast<uint64_t>(immediate & ~int64_t {3}));
		a.Add(RCX, RDX);
		// RAX = (base & ~3) + relative, within the 48-bit address space (AddSignedAddress).
		a.MovRR(RAX, R11);
		a.AndI(RAX, -4);
		const auto negative = a.NewLabel();
		a.Test(RCX, RCX);
		a.Jcc(CondL, negative);
		a.MovRI(RDX, AddressMask);
		a.Sub(RDX, RAX);
		a.Cmp(RCX, RDX);
		a.Jcc(CondA, fail);
		a.Add(RAX, RCX);
		a.Jmp(read);
		a.Bind(negative);
		a.Neg(RCX);
		a.Cmp(RCX, RAX);
		a.Jcc(CondA, fail);
		a.Sub(RAX, RCX);
		a.Bind(read);
		EmitRead(fail);
	}

	// RAX = address, R11 = base -> RAX = the dword read.
	void EmitRead(Label fail) {
		auto& a = m_asm;
		const auto slow = a.NewLabel(), done = a.NewLabel();
		// The reader publishes a checked span after its first ordinary call. Later reads of
		// that span need neither a host ABI transition nor another lookup in the page cache.
		a.Load(RCX, R12, FrameFastRead);
		a.Test(RCX, RCX);
		a.Jcc(CondE, slow);
		a.MovRR(RDX, RAX);
		a.Load(R8, RCX, 0);
		a.Sub(RDX, R8);
		a.Load(R8, RCX, 16);
		a.Cmp(RDX, R8);
		a.Jcc(CondA, slow);
		a.Load(RCX, RCX, 8);
		a.Add(RCX, RDX);
		a.Load32(RAX, RCX, 0);
		a.Jmp(done);
		a.Bind(slow);
		EmitHelperCall(reinterpret_cast<const void*>(&SrtNativeHelpers::Read), [&] {
			a.MovRR(Arg1, RAX);
			a.MovRR(Arg2, R11);
			a.MovRR(Arg0, R12);
			a.Lea(Arg3, RSP, Shadow);
		});
		a.Test(RCX, RCX);
		a.Jcc(CondE, fail);
		a.Bind(done);
	}

	// SrtWalker::EvaluateExtract.
	void EmitExtract(SrtNativeMode mode, const Inst& inst, Label fail) {
		auto&      a     = m_asm;
		const auto index = inst.Arg(1).Resolve();
		if (!index.IsImmediate() || index.GetType() != Type::U32 || index.U32() >= 2u) {
			a.Jmp(fail);
			return;
		}
		const auto component = index.U32();
		if (inst.GetOpcode() == ValueOpcode::CompositeExtractU64) {
			EmitValue(mode, inst.Arg(0), fail);
			if (component != 0u) a.ShrI(RAX, 32);
			a.MovRR32(RAX, RAX);
			return;
		}
		if (!inst.Arg(0).Resolve().TryInstruction()) {
			EmitInterpret(inst, fail);
			return;
		}
		const auto& source = *inst.Arg(0).ResolveInstruction();
		if (source.GetOpcode() == ValueOpcode::CompositeConstructU32x2) {
			EmitValue(mode, source.Arg(component), fail);
			return;
		}
		if (source.GetOpcode() == ValueOpcode::IAddCarry32) {
			EmitOperands(mode, source, 2, fail);
			a.MovRR32(RAX, RAX);
			a.MovRR32(RCX, RCX);
			a.Add(RAX, RCX);
			if (component == 0u) {
				a.MovRR32(RAX, RAX);
			} else {
				a.ShrI(RAX, 32);
			}
			return;
		}
		a.Jmp(fail);
	}

	// BitFieldUExtract / BitFieldSExtract: RAX = base, RCX = offset, RDX = width.
	void EmitBitFieldExtract(bool sign, Label fail) {
		auto&      a    = m_asm;
		const auto zero = a.NewLabel();
		const auto done = a.NewLabel();
		// offset > 32 || width > 32 - offset fails.
		a.MovRR32(RCX, RCX);
		a.MovRR32(RDX, RDX);
		a.CmpI(RCX, 32);
		a.Jcc(CondA, fail);
		a.MovRI(R8, 32);
		a.Sub(R8, RCX);
		a.Cmp(RDX, R8);
		a.Jcc(CondA, fail);
		a.Test(RDX, RDX);
		a.Jcc(CondE, zero);
		// RAX = (base32 >> offset) & ((1 << width) - 1); offset < 32 here since width > 0.
		a.MovRR32(RAX, RAX);
		a.ShrCl(RAX);
		a.MovRR(R8, RDX);
		a.MovRR(RCX, R8);
		a.MovRI(R9, 1);
		a.ShlCl(R9);
		a.SubI(R9, 1);
		a.And(RAX, R9);
		if (sign) {
			// Sign-extend from `width` bits, then keep 32.
			a.MovRI(RCX, 64);
			a.Sub(RCX, R8);
			a.ShlCl(RAX);
			a.SarCl(RAX);
			a.MovRR32(RAX, RAX);
		}
		a.Jmp(done);
		a.Bind(zero);
		a.MovRI(RAX, 0);
		a.Bind(done);
	}

	// BitFieldInsert: RAX = base, RCX = insert, RDX = offset, R8 = width.
	void EmitBitFieldInsert(Label fail) {
		auto&      a    = m_asm;
		const auto keep = a.NewLabel();
		const auto done = a.NewLabel();
		a.MovRR32(RDX, RDX);
		a.MovRR32(R8, R8);
		a.CmpI(RDX, 32);
		a.Jcc(CondA, fail);
		a.MovRI(R9, 32);
		a.Sub(R9, RDX);
		a.Cmp(R8, R9);
		a.Jcc(CondA, fail);
		a.Test(R8, R8);
		a.Jcc(CondE, keep);
		// R9 = mask = ((1 << width) - 1) << offset, in 32 bits.
		a.MovRR(R10, RCX);
		a.MovRR(RCX, R8);
		a.MovRI(R9, 1);
		a.ShlCl(R9);
		a.SubI(R9, 1);
		a.MovRR(RCX, RDX);
		a.ShlCl(R9);
		a.MovRR32(R9, R9);
		// RAX = (base & ~mask) | ((insert << offset) & mask).
		a.ShlCl(R10);
		a.And(R10, R9);
		a.Not(R9);
		a.And(RAX, R9);
		a.Or(RAX, R10);
		a.Bind(keep);
		a.MovRR32(RAX, RAX);
		a.Bind(done);
	}

	// Flushes a float operand's denormal input to a signed zero (FPCompareFlags).
	void EmitFlushDenorm(Reg reg) {
		auto&      a    = m_asm;
		const auto keep = a.NewLabel();
		a.MovRR32(R9, reg);
		a.AndI32(R9, 0x7fffffffu);
		a.CmpI(R9, 0x00800000);
		a.Jcc(CondAE, keep);
		a.AndI32(reg, 0x80000000u);
		a.Bind(keep);
	}

	// The instruction's value -> RAX, as SrtWalker::EvaluateInst.
	void EmitBody(SrtNativeMode mode, const Inst& inst, Label fail) {
		auto& a = m_asm;
		switch (inst.GetOpcode()) {
			case ValueOpcode::GetUserData: {
				const auto reg = inst.Arg(0);
				if (reg.GetType() != Type::ScalarReg) {
					EmitInterpret(inst, fail);
					return;
				}
				const auto index = RegIndex(reg.ScalarRegister());
				if (index < m_program.user_data_base) {
					a.Jmp(fail);
					return;
				}
				const auto slot = index - m_program.user_data_base;
				a.Load(RCX, R12, FrameUserDataSize);
				a.CmpI(RCX, static_cast<int32_t>(slot));
				a.Jcc(CondBE, fail);
				a.Load(RAX, R12, FrameUserData);
				a.Load32(RAX, RAX, static_cast<int32_t>(slot * 4u));
				return;
			}
			case ValueOpcode::GetShaderBase: a.Load(RAX, R12, FrameShaderBase); return;
			case ValueOpcode::Phi: {
				const auto value = ResolveInvariantPhi(m_program, Value(const_cast<Inst*>(&inst)));
				if (value.IsEmpty()) {
					a.Jmp(fail);
					return;
				}
				EmitValue(mode, value, fail);
				return;
			}
			case ValueOpcode::BitCastU32F32:
			case ValueOpcode::BitCastF32U32:
			case ValueOpcode::ConditionRef: EmitValue(mode, inst.Arg(0), fail); return;
			case ValueOpcode::CompositeExtractU64:
			case ValueOpcode::CompositeExtractU32x2: EmitExtract(mode, inst, fail); return;
			case ValueOpcode::CompositeConstructU64:
				EmitOperands(mode, inst, 2, fail);
				a.MovRR32(RAX, RAX);
				a.ShlI(RCX, 32);
				a.Or(RAX, RCX);
				return;
			case ValueOpcode::ReadConst: {
				const auto slot = inst.Arg(1).Resolve();
				if (!slot.IsImmediate() || slot.GetType() != Type::U32 ||
				    slot.U32() >= m_program.srt_reads.size()) {
					a.Jmp(fail);
					return;
				}
				const auto index = slot.U32();
				const auto value = m_program.srt_reads[index].value;
				// Split mode: the walker's clean slots are the plan's (checked when binding).
				if (mode == SrtNativeMode::Split && index < m_program.clean_flat_slots.size() &&
				    m_program.clean_flat_slots[index] != 0u) {
					EmitCleanValue(mode, value, fail);
				} else {
					EmitValue(mode, value, fail);
				}
				return;
			}
			case ValueOpcode::LoadAddressU32:
			case ValueOpcode::ReadConstBuffer:
				if (IsRawRead(m_program, inst)) {
					if (const auto* handle = BufferViewHandle(inst)) EmitReadWithBufferView(mode, inst, *handle, fail);
					else EmitRawRead(mode, inst, fail);
				} else {
					a.Jmp(fail);
				}
				return;
			case ValueOpcode::IAdd32:
				EmitOperands(mode, inst, 2, fail), a.Add(RAX, RCX), a.MovRR32(RAX, RAX);
				return;
			case ValueOpcode::IAdd64: EmitOperands(mode, inst, 2, fail), a.Add(RAX, RCX); return;
			case ValueOpcode::ISub32:
				EmitOperands(mode, inst, 2, fail), a.Sub(RAX, RCX), a.MovRR32(RAX, RAX);
				return;
			case ValueOpcode::ISub64: EmitOperands(mode, inst, 2, fail), a.Sub(RAX, RCX); return;
			case ValueOpcode::IMul32:
				EmitOperands(mode, inst, 2, fail), a.Imul(RAX, RCX), a.MovRR32(RAX, RAX);
				return;
			case ValueOpcode::IMul64: EmitOperands(mode, inst, 2, fail), a.Imul(RAX, RCX); return;
			case ValueOpcode::UMulHi:
				EmitOperands(mode, inst, 2, fail);
				a.MovRR32(RAX, RAX), a.MovRR32(RCX, RCX), a.Imul(RAX, RCX), a.ShrI(RAX, 32);
				return;
			case ValueOpcode::UMin32: {
				EmitOperands(mode, inst, 2, fail);
				const auto done = a.NewLabel();
				a.MovRR32(RAX, RAX), a.MovRR32(RCX, RCX);
				a.Cmp(RAX, RCX);
				a.Jcc(CondBE, done);
				a.MovRR(RAX, RCX);
				a.Bind(done);
				return;
			}
			case ValueOpcode::ConvertF32U32:
				EmitValue(mode, inst.Arg(0), fail);
				a.MovRR32(RAX, RAX), a.Cvtsi2ss64(XMM0, RAX), a.MovdRX(RAX, XMM0);
				return;
			case ValueOpcode::ConvertU32F32:
				// Finite, not below zero (-0 converts to 0) and within uint32_t, else fail.
				EmitValue(mode, inst.Arg(0), fail);
				a.MovRR32(RAX, RAX);
				a.MovRR(RCX, RAX);
				a.AndI32(RCX, 0x7f800000u);
				a.CmpI(RCX, 0x7f800000);
				a.Jcc(CondE, fail);
				{
					const auto positive = a.NewLabel();
					a.MovRR(RCX, RAX);
					a.ShrI(RCX, 31);
					a.Test(RCX, RCX);
					a.Jcc(CondE, positive);
					a.MovRR(RCX, RAX);
					a.AndI32(RCX, 0x7fffffffu);
					a.Test(RCX, RCX);
					a.Jcc(CondNE, fail);
					a.Bind(positive);
				}
				a.MovdXR(XMM0, RAX);
				a.Cvttss2si64(RAX, XMM0);
				a.MovRI(RCX, UINT32_MAX);
				a.Cmp(RAX, RCX);
				a.Jcc(CondA, fail);
				return;
			case ValueOpcode::FPMul32:
				EmitOperands(mode, inst, 2, fail);
				a.MovdXR(XMM0, RAX), a.MovdXR(XMM1, RCX), a.Mulss(XMM0, XMM1), a.MovdRX(RAX, XMM0);
				return;
			case ValueOpcode::FPTrunc32: {
				EmitValue(mode, inst.Arg(0), fail);
				// std::trunc preserves the bit pattern for large integral floats and NaNs;
				// ROUNDSS alone would quiet a signaling NaN and change a descriptor word.
				const auto done = a.NewLabel();
				a.MovRR32(RCX, RAX), a.AndI32(RCX, 0x7fffffffu), a.CmpI(RCX, 0x4b000000);
				a.Jcc(CondAE, done);
				a.MovdXR(XMM0, RAX), a.Roundss(XMM0, XMM0, 0x0b), a.MovdRX(RAX, XMM0);
				a.Bind(done);
				return;
			}
			case ValueOpcode::FPRecipIFlag32:
				// Exact reciprocals of normal powers of two only.
				EmitValue(mode, inst.Arg(0), fail);
				a.MovRR32(RAX, RAX);
				a.MovRR(RCX, RAX);
				a.AndI32(RCX, 0x807fffffu);
				a.Test(RCX, RCX);
				a.Jcc(CondNE, fail);
				a.ShrI(RAX, 23);
				a.AndI(RAX, 255);
				a.Test(RAX, RAX);
				a.Jcc(CondE, fail);
				a.CmpI(RAX, 254);
				a.Jcc(CondAE, fail);
				a.MovRI(RCX, 254);
				a.Sub(RCX, RAX);
				a.ShlI(RCX, 23);
				a.MovRR(RAX, RCX);
				return;
			case ValueOpcode::FPIsNan32:
				EmitValue(mode, inst.Arg(0), fail);
				a.AndI32(RAX, 0x7fffffffu);
				a.CmpI(RAX, 0x7f800000);
				a.SetCC(CondA, RAX);
				a.MovzxB(RAX, RAX);
				return;
			case ValueOpcode::FPOrdLessThanEqual32:
			case ValueOpcode::FPOrdGreaterThanEqual32:
				EmitOperands(mode, inst, 2, fail);
				if (inst.Flags<FPCompareFlags>().flush_input_denorms) {
					EmitFlushDenorm(RAX);
					EmitFlushDenorm(RCX);
				}
				a.MovdXR(XMM0, RAX), a.MovdXR(XMM1, RCX);
				// Ordered: an unordered compare sets CF, so AE is false for NaN.
				if (inst.GetOpcode() == ValueOpcode::FPOrdLessThanEqual32) {
					a.Ucomiss(XMM1, XMM0);
				} else {
					a.Ucomiss(XMM0, XMM1);
				}
				a.SetCC(CondAE, RAX);
				a.MovzxB(RAX, RAX);
				return;
			case ValueOpcode::BitwiseAnd32:
				EmitOperands(mode, inst, 2, fail), a.And(RAX, RCX), a.MovRR32(RAX, RAX);
				return;
			case ValueOpcode::BitwiseAnd64: EmitOperands(mode, inst, 2, fail), a.And(RAX, RCX); return;
			case ValueOpcode::BitwiseOr32:
				EmitOperands(mode, inst, 2, fail), a.Or(RAX, RCX), a.MovRR32(RAX, RAX);
				return;
			case ValueOpcode::BitwiseXor32:
				EmitOperands(mode, inst, 2, fail), a.Xor(RAX, RCX), a.MovRR32(RAX, RAX);
				return;
			case ValueOpcode::BitwiseNot32:
				EmitValue(mode, inst.Arg(0), fail), a.Not(RAX), a.MovRR32(RAX, RAX);
				return;
			case ValueOpcode::ShiftLeftLogical32:
				EmitOperands(mode, inst, 2, fail), a.ShlCl32(RAX);
				return;
			case ValueOpcode::ShiftLeftLogical64: EmitOperands(mode, inst, 2, fail), a.ShlCl(RAX); return;
			case ValueOpcode::ShiftRightLogical32:
				EmitOperands(mode, inst, 2, fail), a.ShrCl32(RAX);
				return;
			case ValueOpcode::ShiftRightLogical64: EmitOperands(mode, inst, 2, fail), a.ShrCl(RAX); return;
			case ValueOpcode::ShiftRightArithmetic32:
				EmitOperands(mode, inst, 2, fail), a.SarCl32(RAX);
				return;
			case ValueOpcode::ShiftRightArithmetic64:
				EmitOperands(mode, inst, 2, fail), a.SarCl(RAX);
				return;
			case ValueOpcode::BitFieldUExtract:
				EmitOperands(mode, inst, 3, fail), EmitBitFieldExtract(false, fail);
				return;
			case ValueOpcode::BitFieldSExtract:
				EmitOperands(mode, inst, 3, fail), EmitBitFieldExtract(true, fail);
				return;
			case ValueOpcode::BitFieldInsert:
				EmitOperands(mode, inst, 4, fail), EmitBitFieldInsert(fail);
				return;
			case ValueOpcode::SelectU32:
			case ValueOpcode::SelectU1:
			case ValueOpcode::SelectF32: {
				const auto other = a.NewLabel();
				const auto done  = a.NewLabel();
				EmitCleanValue(mode, inst.Arg(0), fail);
				a.Test(RAX, RAX);
				a.Jcc(CondE, other);
				EmitValue(mode, inst.Arg(1), fail);
				a.Jmp(done);
				a.Bind(other);
				EmitValue(mode, inst.Arg(2), fail);
				a.Bind(done);
				return;
			}
			case ValueOpcode::IEqual32: EmitCompare(mode, inst, CondE, fail); return;
			case ValueOpcode::INotEqual32: EmitCompare(mode, inst, CondNE, fail); return;
			case ValueOpcode::ULessThan32: EmitCompare(mode, inst, CondB, fail); return;
			case ValueOpcode::UGreaterThan32: EmitCompare(mode, inst, CondA, fail); return;
			case ValueOpcode::ULessThanEqual32: EmitCompare(mode, inst, CondBE, fail); return;
			case ValueOpcode::SGreaterThanEqual32: EmitCompare(mode, inst, CondGE, fail); return;
			case ValueOpcode::LogicalAnd:
				EmitOperands(mode, inst, 2, fail), EmitBool(RAX), EmitBool(RCX), a.And(RAX, RCX);
				return;
			case ValueOpcode::LogicalOr:
				EmitOperands(mode, inst, 2, fail), EmitBool(RAX), EmitBool(RCX), a.Or(RAX, RCX);
				return;
			case ValueOpcode::LogicalXor:
				EmitOperands(mode, inst, 2, fail), EmitBool(RAX), EmitBool(RCX), a.Xor(RAX, RCX);
				return;
			case ValueOpcode::LogicalNot:
				EmitValue(mode, inst.Arg(0), fail);
				a.Test(RAX, RAX), a.SetCC(CondE, RAX), a.MovzxB(RAX, RAX);
				return;
			case ValueOpcode::ReadFirstLane:
			case ValueOpcode::LoadBufferU32: EmitInterpret(inst, fail); return;
			default: a.Jmp(fail); return;
		}
	}

	void EmitRoutine(SrtNativeMode mode, const Inst& inst) {
		auto&      a       = m_asm;
		const auto index   = inst.EvaluationIndex(m_program.evaluation_value_count);
		const auto value   = static_cast<int32_t>(index * 16u);
		const auto state   = value + 8;
		const auto compute = a.NewLabel();
		const auto cycle   = a.NewLabel();
		const auto fail    = a.NewLabel();
		a.Bind(RoutineLabel(mode, &inst));
		// Memo hit: this generation's value. CF is clear after an equal compare.
		a.Load(RAX, R13, state);
		a.Cmp(RAX, R14);
		a.Jcc(CondNE, compute);
		a.Load(RAX, R13, value);
		a.Ret();
		a.Bind(compute);
		// generation | 1 marks a value being evaluated: a cycle fails.
		a.Lea(RCX, R14, 1);
		a.Cmp(RAX, RCX);
		a.Jcc(CondE, cycle);
		a.Store(R13, state, RCX);
		a.Push(RBP);
		a.MovRR(RBP, RSP);
		EmitBody(mode, inst, fail);
		a.MovRR(RSP, RBP);
		a.Pop(RBP);
		a.Load(R13, R12, FrameMemo);
		a.Store(R13, value, RAX);
		a.Store(R13, state, R14);
		a.Clc();
		a.Ret();
		// Failure: clear the memo entry and remember the first failing instruction.
		a.Bind(fail);
		a.MovRR(RSP, RBP);
		a.Pop(RBP);
		a.Load(R13, R12, FrameMemo);
		a.StoreImm32(R13, state, 0);
		const auto recorded = a.NewLabel();
		a.Load(RAX, R12, FrameFailed);
		a.CmpMemI(RAX, 0, 0);
		a.Jcc(CondNE, recorded);
		a.MovRI(RCX, reinterpret_cast<uint64_t>(&inst));
		a.Store(RAX, 0, RCX);
		a.Bind(recorded);
		a.Stc();
		a.Ret();
		a.Bind(cycle);
		a.Stc();
		a.Ret();
	}

	const ResourcePlan&      m_program;
	bool m_value_aliases = false;
	bool m_inline_user_data = true;
	bool m_canonical_views = false;
	bool m_eager_flat = false;
	Assembler                m_asm;
	std::vector<const Inst*> m_insts;
	std::vector<Label>       m_labels[2];
	std::unordered_map<const Inst*, std::array<Label, 2>> m_view_labels;
	std::vector<const Inst*> m_view_handles;
	std::unordered_map<const Inst*, uint32_t> m_view_slots;
	std::vector<std::unique_ptr<SrtBulkReadPlan>> m_bulk_plans;
	uint32_t m_bulk_roots = 0;
	uint32_t                 m_interpreted = 0;
};

} // namespace

// SrtNativeMemory.cpp
void* SrtNativeAllocateCode(const void* code, size_t size);
void  SrtNativeFreeCode(void* memory, size_t size);

SrtNativeCode::~SrtNativeCode() {
	SrtNativeFreeCode(m_memory, m_size);
}

bool SrtNativeCode::Supported() {
#if defined(__x86_64__) || defined(_M_X64)
	return true;
#else
	return false;
#endif
}

std::unique_ptr<SrtNativeCode> SrtNativeCode::Compile(const ResourcePlan& program, bool value_aliases,
                                                  bool inline_user_data, bool canonical_views,
                                                  bool eager_flat) {
	if (!Supported()) {
		return nullptr;
	}
	std::unique_ptr<SrtNativeCode> code(new SrtNativeCode());
	Compiler                       compiler(program, value_aliases, inline_user_data, canonical_views, eager_flat);
	uint32_t batch_entry = 0,group_entry=0;
	if (!compiler.Run(code->m_routines, code->m_descriptor_batches, code->m_flat_batches,code->m_readonly_buffer_batches,
	                  batch_entry,group_entry, code->m_instructions, code->m_interpreted)) {
		return nullptr;
	}
	const auto& bytes = compiler.Asm().Code();
	code->m_buffer_views = compiler.BufferViewCount();
	code->m_readonly_buffer_count=static_cast<uint32_t>(program.info.buffers.size());
	code->m_bulk_roots = compiler.BulkReadRoots();
	code->m_bulk_plans = compiler.TakeBulkPlans();
	code->m_size      = bytes.size();
	auto* memory = static_cast<uint8_t*>(SrtNativeAllocateCode(bytes.data(), code->m_size));
	if (memory == nullptr) {
		return nullptr;
	}
	code->m_memory = memory;
	code->m_group_entry=reinterpret_cast<SrtNativeCode::GroupEntry>(memory+group_entry);
	// The entry thunk is emitted first.
	code->m_entry = reinterpret_cast<Entry>(memory);
	code->m_batch_entry = reinterpret_cast<BatchEntry>(memory + batch_entry);
	const auto resolve = [&](Value value) {
		SrtNativeValue out;
		value = value.Resolve();
		if (value.IsImmediate()) {
			out.kind = ImmediateBits(value, out.immediate) ? SrtNativeValue::Immediate
			                                                : SrtNativeValue::Fail;
			return out;
		}
		const auto* inst  = value.TryInstruction();
		const auto  index = inst->EvaluationIndex(program.evaluation_value_count);
		if (code->Has(SrtNativeMode::Self, index) && code->Has(SrtNativeMode::Split, index)) {
			out.kind  = SrtNativeValue::Routine;
			out.index = index;
			out.inst  = inst;
		} else {
			out.kind = SrtNativeValue::Interpret;
			out.inst = inst;
		}
		return out;
	};
	for (const auto& read: program.srt_reads) {
		code->m_flat_reads.push_back(resolve(read.value));
	}
	for (const auto& block: program.control_flow) {
		code->m_conditions.push_back(block.condition.IsEmpty() ? SrtNativeValue {}
		                                                       : resolve(block.condition));
	}
	code->m_descriptor_dwords.resize(program.descriptor_sources.size() * 8u);
	for (size_t source = 0; source < program.descriptor_sources.size(); source++) {
		const auto& descriptor = program.descriptor_sources[source];
		for (uint32_t dword = 0; dword < descriptor.dword_count && dword < 8u; dword++) {
			code->m_descriptor_dwords[source * 8u + dword] = resolve(descriptor.dwords[dword]);
		}
	}
	return code;
}

bool SrtNativeCode::Has(SrtNativeMode mode, uint32_t index) const {
	const auto& routines = m_routines[static_cast<uint32_t>(mode)];
	return index < routines.size() && routines[index] != UINT32_MAX;
}

bool SrtNativeCode::Evaluate(SrtNativeFrame& frame, SrtNativeMode mode, uint32_t index,
                             uint64_t& result) const {
	const auto* routine = m_memory + m_routines[static_cast<uint32_t>(mode)][index];
	return m_entry(&frame, routine, &result);
}

bool SrtNativeCode::EvaluateDescriptor(SrtNativeFrame& frame, SrtNativeMode mode,
                                      uint32_t source, uint32_t* output) const {
	const auto& offsets = m_descriptor_batches[static_cast<uint32_t>(mode)];
	return source < offsets.size() && m_batch_entry(&frame, m_memory + offsets[source], output);
}
bool SrtNativeCode::RefreshBlock(SrtNativeFrame& frame, SrtNativeMode mode,
                                uint32_t block, uint32_t* flat) const {
	const auto& offsets = m_flat_batches[static_cast<uint32_t>(mode)];
	return block < offsets.size() && m_batch_entry(&frame, m_memory + offsets[block], flat);
}

bool SrtNativeCode::HasReadonlyBufferGroup(size_t count) const {
	return count==m_readonly_buffer_count && m_readonly_buffer_batches[0]!=UINT32_MAX &&
	    m_readonly_buffer_batches[1]!=UINT32_MAX;
}
size_t SrtNativeCode::EvaluateReadonlyBufferGroup(SrtNativeFrame& frame,SrtNativeMode mode,
	                                               DescriptorValue* output,uint64_t active_mask) const {
	const auto offset=m_readonly_buffer_batches[static_cast<uint32_t>(mode)];
	EXIT_IF(offset==UINT32_MAX);
	return m_group_entry(&frame,m_memory+offset,output,active_mask);
}

} // namespace Libs::Graphics::ShaderRecompiler::IR
