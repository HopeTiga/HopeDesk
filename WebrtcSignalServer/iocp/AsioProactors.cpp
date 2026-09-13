#include "AsioProactors.h"
#include <iostream>
#include <vector>
#include <algorithm>
#include "../utils/Utils.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#include <fstream>
#include <set>
#include <string>
#endif

namespace hope {
	namespace iocp {

		namespace {

			struct PhysicalCore {
				size_t cpuIndex;
				bool hasSmt;
			};

#ifdef _WIN32
			std::vector<PhysicalCore> getPhysicalCores() {
				std::vector<PhysicalCore> cores;

				DWORD bufferLength = 0;
				::GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bufferLength);
				if (bufferLength == 0) {
					return cores;
				}

				std::vector<char> buffer(bufferLength);
				if (!::GetLogicalProcessorInformationEx(RelationProcessorCore,
					reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()), &bufferLength)) {
					return cores;
				}

				size_t offset = 0;
				while (offset + 8 <= bufferLength) {
					const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX* entry =
						reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buffer.data() + offset);
					if (entry->Size < 8 || offset + entry->Size > bufferLength) {
						break;
					}

					const PROCESSOR_RELATIONSHIP& processor = entry->Processor;

					size_t cpuIndex = static_cast<size_t>(-1);
					for (WORD group = 0; group < processor.GroupCount; group++) {
						const KAFFINITY mask = processor.GroupMask[group].Mask;
						for (size_t bit = 0; bit < sizeof(KAFFINITY) * 8; bit++) {
							if (((mask >> bit) & 1) == 0) {
								continue;
							}
							const size_t globalIndex =
								static_cast<size_t>(processor.GroupMask[group].Group) * 64 + bit;
							if (globalIndex < cpuIndex) {
								cpuIndex = globalIndex;
							}
						}
					}

					if (cpuIndex != static_cast<size_t>(-1)) {
						PhysicalCore core;
						core.cpuIndex = cpuIndex;
						core.hasSmt = (processor.Flags & LTP_PC_SMT) != 0;
						cores.push_back(core);
					}

					offset += entry->Size;
				}

				return cores;
			}
#else

			std::vector<PhysicalCore> getPhysicalCores() {
				std::vector<PhysicalCore> cores;

				cpu_set_t allowedSet;
				CPU_ZERO(&allowedSet);
				if (::sched_getaffinity(0, sizeof(allowedSet), &allowedSet) != 0) {
					return cores;
				}

				std::set<size_t> visitedCore;
				for (size_t cpu = 0; cpu < CPU_SETSIZE; cpu++) {
					if (!CPU_ISSET(cpu, &allowedSet)) {
						continue;
					}

					const std::string path = "/sys/devices/system/cpu/cpu" + std::to_string(cpu)
						+ "/topology/thread_siblings_list";
					std::ifstream file(path);
					std::string line;
					if (!file || !std::getline(file, line)) {
						cores.push_back({ cpu, false });
						continue;
					}

					/* 兄弟列表形如 "0,1" 或 "0-1" 或 "0-3,8-11"，取允许集合里编号最小的当代表 */
					size_t representative = static_cast<size_t>(-1);
					size_t siblingCount = 0;
					size_t cursor = 0;
					while (cursor < line.size()) {
						while (cursor < line.size() && (line[cursor] < '0' || line[cursor] > '9')) {
							cursor++;
						}
						if (cursor >= line.size()) {
							break;
						}

						size_t first = 0;
						while (cursor < line.size() && line[cursor] >= '0' && line[cursor] <= '9') {
							first = first * 10 + static_cast<size_t>(line[cursor] - '0');
							cursor++;
						}

						size_t last = first;
						if (cursor < line.size() && line[cursor] == '-') {
							cursor++;
							last = 0;
							while (cursor < line.size() && line[cursor] >= '0' && line[cursor] <= '9') {
								last = last * 10 + static_cast<size_t>(line[cursor] - '0');
								cursor++;
							}
						}

						for (size_t sibling = first; sibling <= last && sibling < CPU_SETSIZE; sibling++) {
							if (!CPU_ISSET(sibling, &allowedSet)) {
								continue;
							}
							siblingCount++;
							if (sibling < representative) {
								representative = sibling;
							}
						}
					}

					if (representative == static_cast<size_t>(-1)) {
						continue;
					}
					if (!visitedCore.insert(representative).second) {
						continue;   // 这个物理核已经由它的另一个兄弟代表过了
					}

					cores.push_back({ representative, siblingCount > 1 });
				}

				return cores;
			}
#endif

			bool bindCurrentThreadToCpu(size_t cpuIndex) {
#ifdef _WIN32
				if (cpuIndex >= sizeof(DWORD_PTR) * 8) {
					return false;
				}
				const DWORD_PTR mask = static_cast<DWORD_PTR>(1) << cpuIndex;
				return ::SetThreadAffinityMask(::GetCurrentThread(), mask) != 0;
#else
				if (cpuIndex >= CPU_SETSIZE) {
					return false;
				}
				cpu_set_t cpuSet;
				CPU_ZERO(&cpuSet);
				CPU_SET(cpuIndex, &cpuSet);
				return ::pthread_setaffinity_np(::pthread_self(), sizeof(cpuSet), &cpuSet) == 0;
#endif
			}

		}

		size_t AsioProactors::sIoSize = std::thread::hardware_concurrency();

		size_t AsioProactors::sLogicSize = std::thread::hardware_concurrency();

		bool AsioProactors::sEnableCpuAffinity = false;

		size_t AsioProactors::sCpuAffinityOffset = 0;

		void AsioProactors::init(size_t size, bool enableCpuAffinity, size_t cpuAffinityOffset) {
			sIoSize = size;
			sLogicSize = size;
			sEnableCpuAffinity = enableCpuAffinity;
			sCpuAffinityOffset = cpuAffinityOffset;
		}

		AsioProactors::AsioProactors(size_t size)
			: size(size)
			, ioContexts(size)
			, works(size)
			, threads(size)
			, ioPressures(size) {

			std::vector<PhysicalCore> cores;
			if (sEnableCpuAffinity) {
				cores = getPhysicalCores();

				std::stable_sort(cores.begin(), cores.end(),
					[](const PhysicalCore& left, const PhysicalCore& right) {
						return static_cast<int>(left.hasSmt) > static_cast<int>(right.hasSmt);
					});

				const size_t smtCoreCount = static_cast<size_t>(std::count_if(
					cores.begin(), cores.end(),
					[](const PhysicalCore& core) { return core.hasSmt; }));

				if (cores.empty()) {
					LOG_WARN("AsioProactors CPU affinity requested but no physical core found, threads stay unbound");
				}
				else {
					LOG_INFO("AsioProactors CPU affinity enabled: {} threads over {} physical cores ({} with smt), one logical cpu per core",
						static_cast<int>(size), static_cast<int>(cores.size()), static_cast<int>(smtCoreCount));

					if (size > cores.size()) {
						LOG_WARN("AsioProactors CPU affinity: {} threads over {} physical cores, cores are reused round-robin and some will carry two reactors",
							static_cast<int>(size), static_cast<int>(cores.size()));
					}
					else {
						size_t ecoreReactors = 0;
						for (size_t reactorIndex = 0; reactorIndex < size; reactorIndex++) {
							if (!cores[(sCpuAffinityOffset + reactorIndex) % cores.size()].hasSmt) {
								ecoreReactors++;
							}
						}
						if (smtCoreCount > 0 && ecoreReactors > 0) {
							LOG_WARN("AsioProactors CPU affinity: {} of {} reactors land on e-cores, which have much lower single-core throughput; set threadSize to {} to keep every reactor on a p-core",
								static_cast<int>(ecoreReactors), static_cast<int>(size), static_cast<int>(smtCoreCount));
						}
					}
				}
			}

			for (int i = 0; i < size; i++) {

				ioContexts[i] = std::make_unique<boost::asio::io_context>(1);

				std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work = std::make_unique<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(
					boost::asio::make_work_guard(*ioContexts[i].get())
				);

				works[i] = std::move(work);

				const bool bindCpuAffinity = sEnableCpuAffinity && !cores.empty();
				const size_t cpuIndex = bindCpuAffinity
					? cores[(sCpuAffinityOffset + static_cast<size_t>(i)) % cores.size()].cpuIndex
					: 0;

				threads[i] = std::thread([this, i, bindCpuAffinity, cpuIndex]() {
					if (bindCpuAffinity && !bindCurrentThreadToCpu(cpuIndex)) {
						LOG_WARN("AsioProactors failed to bind thread {} to cpu {}", i, static_cast<int>(cpuIndex));
					}
					ioContexts[i]->run();
					});
			}

		}

		AsioProactors::~AsioProactors() {
			stop();
		}

		void AsioProactors::releaseWork() {
			for (auto& work : works) {

				if (work) {
					work.reset();
				}
			}
		}

		void AsioProactors::stop() {

			releaseWork();

			for (auto& context : ioContexts) {
				context->stop();
			}

			for (auto& t : threads) {
				if (t.joinable()) {
					t.join();
				}
			}
		}

		std::pair<int, boost::asio::io_context&> AsioProactors::getIoCompletePorts() {
			size_t current = loadBalancing.fetch_add(1);
			size_t index = current % size;
			ioPressures[index]++;
			return { static_cast<int>(index), *ioContexts[index] };
		}

		boost::asio::io_context& AsioProactors::getIoCompletePort(size_t channelIndex)
		{
			return *ioContexts[channelIndex];
		}

	}
}
