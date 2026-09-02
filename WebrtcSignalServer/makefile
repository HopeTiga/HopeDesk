CXX := clang++
CXXFLAGS := -std=c++20
CXXFLAGS += -O3 -DNDEBUG
CXXFLAGS += -march=native -mtune=native
CXXFLAGS += -fomit-frame-pointer
CXXFLAGS += -finline-functions -finline-hint-functions
CXXFLAGS += -fwhole-program-vtables -fstrict-vtable-pointers
CXXFLAGS += -ffunction-sections -fdata-sections
CXXFLAGS += -pipe
CXXFLAGS += -w
# mimalloc 全局替换 malloc/free:防止编译器按 glibc builtin 语义内联/改写分配调用
CXXFLAGS += -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free
CXXFLAGS += -DBOOST_ASIO_HAS_IO_URING
CXXFLAGS += -DBOOST_SAM_HEADER_ONLY
# spdlog header-only + 内置 fmt(与 Windows 侧 props 一致)
CXXFLAGS += -DSPDLOG_HEADER_ONLY
CXXFLAGS += -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE
CXXFLAGS += -Iinclude/mimalloc -Iinclude/spdlog -Iinclude/openssl -Iinclude/abseil-cpp -Iinclude/boost -Iinclude/coroRpc

# === ThinLTO + 协程优化核心 ===
CXXFLAGS += -flto=thin

BUILDDIR := release-x64

LDFLAGS := -Llib/mimalloc -Llib/openssl -Llib/abseil-cpp -Llib/boost
LDFLAGS += -Wl,-rpath,'$$ORIGIN'
LDFLAGS += -Wl,-rpath,'$$ORIGIN/../lib/mimalloc' -Wl,-rpath,'$$ORIGIN/../lib/openssl' -Wl,-rpath,'$$ORIGIN/../lib/abseil-cpp' -Wl,-rpath,'$$ORIGIN/../lib/boost'
LDFLAGS += -fuse-ld=lld
LDFLAGS += -flto=thin
LDFLAGS += -Wl,--gc-sections
LDFLAGS += -Wl,-O3 -Wl,--icf=all

LDLIBS := -lmimalloc
LDLIBS += -lssl -lcrypto
LDLIBS += -lboost_json -lboost_container -lboost_thread -lboost_chrono -lboost_atomic -lboost_system -lboost_date_time -lboost_regex -lboost_program_options -lboost_serialization -lboost_coroutine -lboost_context -lboost_log -lboost_log_setup -lboost_filesystem -lboost_iostreams -lboost_random -lboost_exception -lboost_charconv -lboost_url
LDLIBS += -luring

ABSL_LIBS = -labsl_base -labsl_synchronization -labsl_spinlock_wait -labsl_strerror -labsl_throw_delegate -labsl_tracing_internal -labsl_log_severity -labsl_malloc_internal -labsl_poison -labsl_raw_logging_internal -labsl_scoped_set_env -labsl_hashtablez_sampler -labsl_raw_hash_set -labsl_civil_time -labsl_time -labsl_time_zone -labsl_city -labsl_hash -labsl_stacktrace -labsl_symbolize -labsl_utf8_for_code_point -labsl_debugging_internal -labsl_decode_rust_punycode -labsl_demangle_internal -labsl_demangle_rust -labsl_examine_stack -labsl_failure_signal_handler -labsl_leak_check -labsl_int128 -labsl_graphcycles_internal -labsl_kernel_timeout_internal -labsl_strings -labsl_strings_internal -labsl_cord -labsl_cord_internal -labsl_crc32c -labsl_crc_internal -labsl_crc_cpu_detect -labsl_crc_cord_state -labsl_cordz_info -labsl_cordz_functions -labsl_cordz_handle -labsl_cordz_sample_token -labsl_status -labsl_statusor -labsl_str_format_internal -labsl_log_globals -labsl_log_flags -labsl_log_internal_check_op -labsl_log_internal_conditions -labsl_log_internal_format -labsl_log_internal_globals -labsl_log_internal_log_sink_set -labsl_log_internal_message -labsl_log_internal_nullguard -labsl_log_internal_proto -labsl_log_internal_structured_proto -labsl_log_sink -labsl_log_entry -labsl_log_initialize -labsl_vlog_config_internal -labsl_periodic_sampler -labsl_exponential_biased -labsl_source_location -labsl_flags_commandlineflag -labsl_flags_commandlineflag_internal -labsl_flags_config -labsl_flags_internal -labsl_flags_marshalling -labsl_flags_parse -labsl_flags_private_handle_accessor -labsl_flags_program_name -labsl_flags_reflection -labsl_flags_usage -labsl_flags_usage_internal -labsl_hashtable_profiler -labsl_hardening -labsl_generic_printer_internal -labsl_profile_builder -labsl_random_distributions -labsl_random_internal_platform -labsl_random_internal_randen -labsl_random_internal_randen_hwaes -labsl_random_internal_randen_hwaes_impl -labsl_random_internal_randen_slow -labsl_random_internal_seed_material -labsl_random_internal_entropy_pool -labsl_random_seed_gen_exception -labsl_random_seed_sequences -labsl_random_internal_distribution_test_util

LDLIBS += -Wl,--start-group $(ABSL_LIBS) -Wl,--end-group
LDLIBS += -lpthread -ldl -lquadmath

TARGET := $(BUILDDIR)/WebrtcSignalServer

SRCS := main.cpp \
	ssl/Ssl.cpp \
	signal/WebrtcSignalServer.cpp \
	signal/WebrtcSignalSocket.cpp \
	signal/WebrtcSignalManager.cpp \
	signal/WebrtcSignalPacket.cpp \
	signal/WebrtcLogicSystem.cpp \
	signal/HttpSocket.cpp \
	signal/HttpClient.cpp \
	signal/HttpFilters.cpp \
	rpc/Rpc.cpp \
	rpc/CoroRpc.cpp \
	rpc/CoroRpcHandleImpl.cpp \
	mysql/WebrtcMysqlManagerPools.cpp \
	iocp/AsioProactors.cpp \
	utils/Utils.cpp

OBJS := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(SRCS))

# 默认目标
all: $(TARGET)

# 链接目标
$(TARGET): $(OBJS)
	mkdir -p $(BUILDDIR)
	cp lib/openssl/libcrypto.so.3 lib/openssl/libssl.so.3 $(BUILDDIR)/
	cp -P lib/mimalloc/libmimalloc.so* $(BUILDDIR)/
	cd $(BUILDDIR) && ln -sf libcrypto.so.3 libcrypto.so && ln -sf libssl.so.3 libssl.so
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

# 编译规则：自动创建子目录
$(BUILDDIR)/%.o: %.cpp
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

run: $(TARGET)
	$(BUILDDIR)/WebrtcSignalServer

# 清理
clean:
	rm -rf $(BUILDDIR)

.PHONY: all run clean
