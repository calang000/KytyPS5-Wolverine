#ifndef KYTY_RESOURCE_REPLAY_H_
#define KYTY_RESOURCE_REPLAY_H_

// Local CPU replay captures. No guest address is dereferenced by the replay tool: its reader
// serves only the words recorded during one real materialization, in the original read order.
#include "graphics/host_gpu/renderer/pipeline/programDiskCache.h"
#include <fstream>
#include <limits>

namespace Libs::Graphics::ResourceReplay {
struct Read {
	uint64_t address = 0;
	uint32_t first = 0;
	uint32_t count = 0;
	bool ok = false;
};
struct ReadLog {
	std::vector<Read> reads;
	std::vector<uint32_t> words;
};
struct Capture {
	std::vector<uint8_t> plan;
	std::vector<uint32_t> user_data;
	uint64_t shader_base = 0;
	std::array<uint32_t, 3> workgroup_count {}, workgroup_size {};
	bool float_image_atomics = true;
	ReadLog log;
};

template <typename T> bool Put(std::ostream& out, const T& value) {
	static_assert(std::is_trivially_copyable_v<T>);
	return bool(out.write(reinterpret_cast<const char*>(&value), sizeof(value)));
}
template <typename T> bool Get(std::istream& in, T& value) {
	static_assert(std::is_trivially_copyable_v<T>);
	return bool(in.read(reinterpret_cast<char*>(&value), sizeof(value)));
}
template <typename T> bool PutVector(std::ostream& out, const std::vector<T>& values) {
	return Put(out, uint64_t(values.size())) &&
	       bool(out.write(reinterpret_cast<const char*>(values.data()), values.size() * sizeof(T)));
}
template <typename T> bool GetVector(std::istream& in, std::vector<T>& values) {
	uint64_t count = 0;
	if (!Get(in, count) || count > (64u << 20u) / sizeof(T)) return false;
	values.resize(static_cast<size_t>(count));
	return bool(in.read(reinterpret_cast<char*>(values.data()), values.size() * sizeof(T)));
}

inline bool Write(const std::filesystem::path& path, const Capture& capture) {
	std::ofstream out(path, std::ios::binary | std::ios::trunc);
	if (!Put(out, uint64_t(0x3159504c52545253ull)) || !PutVector(out, capture.plan) ||
	    !PutVector(out, capture.user_data) || !Put(out, capture.shader_base) ||
	    !Put(out, capture.workgroup_count) || !Put(out, capture.workgroup_size) ||
	    !Put(out, uint32_t(capture.float_image_atomics)) ||
	    !Put(out, uint64_t(capture.log.reads.size()))) return false;
	for (const auto& read: capture.log.reads) {
		if (!Put(out, read.address) || !Put(out, read.first) || !Put(out, read.count) ||
		    !Put(out, uint32_t(read.ok))) return false;
	}
	return PutVector(out, capture.log.words);
}
inline bool Load(const std::filesystem::path& path, Capture& capture) {
	std::ifstream in(path, std::ios::binary);
	uint64_t magic = 0, count = 0;
	uint32_t atomics = 0;
	if (!Get(in, magic) || magic != 0x3159504c52545253ull || !GetVector(in, capture.plan) ||
	    !GetVector(in, capture.user_data) || capture.user_data.size() > 256 ||
	    !Get(in, capture.shader_base) || !Get(in, capture.workgroup_count) ||
	    !Get(in, capture.workgroup_size) || !Get(in, atomics) || !Get(in, count) ||
	    count > (1u << 20u)) return false;
	capture.float_image_atomics = atomics != 0;
	capture.log.reads.resize(static_cast<size_t>(count));
	for (auto& read: capture.log.reads) {
		uint32_t ok = 0;
		if (!Get(in, read.address) || !Get(in, read.first) || !Get(in, read.count) ||
		    !Get(in, ok)) return false;
		read.ok = ok != 0;
	}
	if (!GetVector(in, capture.log.words)) return false;
	for (const auto& read: capture.log.reads) {
		if (read.ok && (read.first > capture.log.words.size() ||
		               read.count > capture.log.words.size() - read.first)) return false;
	}
	return true;
}
} // namespace Libs::Graphics::ResourceReplay
#endif
