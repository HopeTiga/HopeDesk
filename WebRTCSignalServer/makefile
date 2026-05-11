CXX := g++
CXXFLAGS := -std=c++20 -O3 -finline-functions -pipe
CXXFLAGS += -w
CXXFLAGS += -DTCMALLOC_AGGRESSIVE_DECOMMIT -DTCMALLOC_RELEASE_RATE=10.0
CXXFLAGS += -DBOOST_ASIO_HAS_IO_URING
CXXFLAGS += -Iinclude/openssl -Iinclude/absl -Iinclude/boost

BUILDDIR := release-x64

LDFLAGS := -Llib/openssl -Llib/absl -Llib/boost -Llib/tcmalloc
LDFLAGS += -Wl,-rpath,$ORIGIN

LDLIBS := -lssl -lcrypto
LDLIBS += -lboost_json -lboost_container -lboost_thread -lboost_chrono -lboost_atomic -lboost_system -lboost_date_time -lboost_regex -lboost_program_options -lboost_serialization -lboost_coroutine -lboost_context -lboost_log -lboost_log_setup -lboost_filesystem -lboost_iostreams -lboost_random -lboost_exception -lboost_sam -lboost_charconv
LDLIBS += -ltcmalloc
LDLIBS += -luring

ABSL_LIBS = -labsl_base -labsl_synchronization -labsl_spinlock_wait -labsl_strerror -labsl_throw_delegate -labsl_tracing_internal -labsl_log_severity -labsl_malloc_internal -labsl_poison -labsl_raw_logging_internal -labsl_scoped_set_env -labsl_hashtablez_sampler -labsl_raw_hash_set -labsl_civil_time -labsl_time -labsl_time_zone -labsl_city -labsl_hash -labsl_stacktrace -labsl_symbolize -labsl_utf8_for_code_point -labsl_debugging_internal -labsl_decode_rust_punycode -labsl_demangle_internal -labsl_demangle_rust -labsl_examine_stack -labsl_failure_signal_handler -labsl_leak_check -labsl_int128 -labsl_graphcycles_internal -labsl_kernel_timeout_internal -labsl_strings -labsl_strings_internal -labsl_cord -labsl_cord_internal -labsl_crc32c -labsl_crc_internal -labsl_crc_cpu_detect -labsl_crc_cord_state -labsl_cordz_info -labsl_cordz_functions -labsl_cordz_handle -labsl_cordz_sample_token -labsl_status -labsl_statusor -labsl_str_format_internal -labsl_log_globals -labsl_log_flags -labsl_log_internal_check_op -labsl_log_internal_conditions -labsl_log_internal_format -labsl_log_internal_globals -labsl_log_internal_log_sink_set -labsl_log_internal_message -labsl_log_internal_nullguard -labsl_log_internal_proto -labsl_log_internal_structured_proto -labsl_log_sink -labsl_log_entry -labsl_log_initialize -labsl_vlog_config_internal -labsl_periodic_sampler -labsl_exponential_biased -labsl_source_location -labsl_flags_commandlineflag -labsl_flags_commandlineflag_internal -labsl_flags_config -labsl_flags_internal -labsl_flags_marshalling -labsl_flags_parse -labsl_flags_private_handle_accessor -labsl_flags_program_name -labsl_flags_reflection -labsl_flags_usage -labsl_flags_usage_internal -labsl_hashtable_profiler -labsl_hardening -labsl_generic_printer_internal -labsl_profile_builder -labsl_random_distributions -labsl_random_internal_platform -labsl_random_internal_randen -labsl_random_internal_randen_hwaes -labsl_random_internal_randen_hwaes_impl -labsl_random_internal_randen_slow -labsl_random_internal_seed_material -labsl_random_internal_entropy_pool -labsl_random_seed_gen_exception -labsl_random_seed_sequences -labsl_random_internal_distribution_test_util

LDLIBS += -Wl,--start-group $(ABSL_LIBS) -Wl,--end-group
LDLIBS += -lpthread -ldl -lquadmath

TARGET := $(BUILDDIR)/WebRTCSignalServer

SRCS := main.cpp \
	WebRTCSignalServer.cpp \
	WebRTCSignalSocket.cpp \
	WebRTCSignalManager.cpp \
	WebRTCSignalData.cpp \
	WebRTCLogicSystem.cpp \
	WebRTCMysqlManager.cpp \
	WebRTCMysqlManagerPools.cpp \
	AsioProactors.cpp \
	HttpSocket.cpp \
	Utils.cpp

OBJS := $(patsubst %.cpp,$(BUILDDIR)/%.o,$(SRCS))

all: $(TARGET)

$(TARGET): $(OBJS) | $(BUILDDIR)
	cp lib/openssl/libcrypto.so.3 lib/openssl/libssl.so.3 lib/tcmalloc/libtcmalloc.so.4.5.16 $(BUILDDIR)/
	cd $(BUILDDIR) && ln -sf libcrypto.so.3 libcrypto.so && ln -sf libssl.so.3 libssl.so && ln -sf libtcmalloc.so.4.5.16 libtcmalloc.so.4 && ln -sf libtcmalloc.so.4 libtcmalloc.so
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

$(BUILDDIR)/%.o: %.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c -o $@ $<

$(BUILDDIR):
	mkdir -p $@

clean:
	rm -rf $(BUILDDIR)

.PHONY: all clean