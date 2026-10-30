#ifndef KYTY_RESOURCE_WORKER_H_
#define KYTY_RESOURCE_WORKER_H_

#include "graphics/shader/recompiler/ir/passes/ResourceMaterialization.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace Libs::Graphics {
// Filled by the GPU thread using clean backing copies. The evaluator never touches guest
// mappings, buffer/texture caches, or pointers into mappings that can be unmapped.
struct ResourceMemorySnapshot {
	static constexpr uint64_t PageSize = 4096;
	struct Page {
		uint64_t address;
		std::array<uint8_t, PageSize> bytes;
	};
	std::vector<Page> pages; // sorted by address
	bool missed = false;
	static bool Read(void* userdata, uint64_t address, std::span<uint32_t> words) {
		auto& self = *static_cast<ResourceMemorySnapshot*>(userdata);
		auto* destination = reinterpret_cast<uint8_t*>(words.data());
		size_t remaining = words.size_bytes();
		if (remaining == 0 || address > UINT64_MAX - remaining) {
			self.missed = true;
			return false;
		}
		while (remaining != 0) {
			const uint64_t base = address & ~(PageSize - 1);
			const auto found = std::lower_bound(self.pages.begin(), self.pages.end(), base,
			    [](const Page& page, uint64_t value) { return page.address < value; });
			if (found == self.pages.end() || found->address != base) {
				self.missed = true;
				return false;
			}
			const size_t offset = address - base;
			const size_t count = std::min(remaining, size_t(PageSize) - offset);
			std::memcpy(destination, found->bytes.data() + offset, count);
			destination += count; address += count; remaining -= count;
		}
		return true;
	}
};

// One reusable job: submit on the owner, overlap another stage, join before resolving the
// permutation. Each plan has a separate worker session; output belongs to this job until joined.
class ResourceEvaluationWorker {
public:
	struct Job {
		const ShaderRecompiler::IR::ResourcePlan* plan = nullptr;
		ShaderRecompiler::IR::ResourceEvaluationSession* session = nullptr;
		std::array<uint32_t, 40> user_data {};
		uint32_t user_data_count = 0;
		uint64_t shader_base = 0;
		std::array<uint32_t, 3> workgroup_count {}, workgroup_size {};
		bool float_image_atomics = true;
		ResourceMemorySnapshot memory;
		ShaderRecompiler::IR::ResourceSnapshot resources;
		ShaderRecompiler::IR::ResourceSpecialization specialization;
		bool ok = false;
		double evaluation_us = 0;
	};
	ResourceEvaluationWorker() : m_thread([this] { Run(); }) {}
	~ResourceEvaluationWorker() {
		Wait();
		m_state.store(3, std::memory_order_release);
		m_state.notify_one();
		m_thread.join();
	}
	ResourceEvaluationWorker(const ResourceEvaluationWorker&) = delete;
	ResourceEvaluationWorker& operator=(const ResourceEvaluationWorker&) = delete;
	Job job;
	void Submit() {
		job.ok = false;
		job.memory.missed = false;
		m_state.store(1, std::memory_order_release);
		m_state.notify_one();
	}
	void Wait() {
		while (m_state.load(std::memory_order_acquire) == 1) m_state.wait(1);
	}
private:
	void Run() {
		for (;;) {
			auto state = m_state.load(std::memory_order_acquire);
			if (state == 3) return;
			if (state != 1) { m_state.wait(state); continue; }
			const auto begin = std::chrono::steady_clock::now();
			ShaderRecompiler::IR::SrtRuntime runtime {
			    .user_data = std::span(job.user_data).first(job.user_data_count),
			    .shader_base = job.shader_base,
			    .read_memory = ResourceMemorySnapshot::Read, .userdata = &job.memory,
			    .read_specialization_memory = ResourceMemorySnapshot::Read,
			    .float_image_atomics = job.float_image_atomics,
			    .workgroup_count = job.workgroup_count, .workgroup_size = job.workgroup_size,
			    .evaluation_session = job.session};
			job.ok = ShaderRecompiler::IR::MaterializeResources(*job.plan, runtime,
			    job.resources, job.specialization) && !job.memory.missed;
			job.evaluation_us = std::chrono::duration<double, std::micro>(
			    std::chrono::steady_clock::now() - begin).count();
			m_state.store(2, std::memory_order_release);
			m_state.notify_one();
		}
	}
	std::atomic<uint32_t> m_state {0}; // idle/running/done/stopping
	std::thread m_thread;
};
} // namespace Libs::Graphics
#endif
