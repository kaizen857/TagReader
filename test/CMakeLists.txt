add_executable(TagReaderCatch2SmokeTests
    catch2/smoke_test.cpp
)

target_link_libraries(TagReaderCatch2SmokeTests
    PRIVATE
        Catch2::Catch2WithMain
)

tagreader_enable_sanitizers(TagReaderCatch2SmokeTests)

target_include_directories(TagReaderCatch2SmokeTests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

include(CTest)
include(Catch)

catch_discover_tests(TagReaderCatch2SmokeTests)

add_executable(TagReaderSecuritySmoke
    security/security_smoke.cpp
)

tagreader_enable_sanitizers(TagReaderSecuritySmoke)

target_link_libraries(TagReaderSecuritySmoke
    PRIVATE
        TagReaderCore
)

find_program(Python3_EXECUTABLE NAMES python3 python REQUIRED)

set(TAGREADER_SECURITY_SAMPLE_DIR ${CMAKE_CURRENT_BINARY_DIR}/security/samples)
set(TAGREADER_SECURITY_COVER_DIR ${CMAKE_CURRENT_BINARY_DIR}/security/covers)

add_test(NAME TagReaderSecurityGenerateSamples
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/security/generate_samples.py --out-dir ${TAGREADER_SECURITY_SAMPLE_DIR}
)

set_tests_properties(TagReaderSecurityGenerateSamples PROPERTIES
    LABELS "security"
    FIXTURES_SETUP "tagreader_security_samples"
    RESOURCE_LOCK "tagreader_security_workspace"
)

add_test(NAME TagReaderSecuritySmoke
    COMMAND TagReaderSecuritySmoke ${TAGREADER_SECURITY_COVER_DIR} ${TAGREADER_SECURITY_SAMPLE_DIR}
)

set_tests_properties(TagReaderSecuritySmoke PROPERTIES
    LABELS "security"
    FIXTURES_REQUIRED "tagreader_security_samples"
    RESOURCE_LOCK "tagreader_security_workspace"
    SKIP_RETURN_CODE 77
)

add_executable(TagReaderFlacMalformedMetadataCatch2Tests
    regression/catch2_regression_support.cpp
    regression/flac_malformed_metadata_support.cpp
    regression/flac_malformed_metadata_catch2_tests.cpp
)

target_link_libraries(TagReaderFlacMalformedMetadataCatch2Tests
    PRIVATE
        Catch2::Catch2WithMain
        TagReaderCore
)

tagreader_enable_sanitizers(TagReaderFlacMalformedMetadataCatch2Tests)

catch_discover_tests(TagReaderFlacMalformedMetadataCatch2Tests)

add_executable(TagReaderDefaultCoverExportDirectoryCatch2Tests
    regression/catch2_regression_support.cpp
    regression/catch2_sample_support.cpp
    regression/default_cover_export_directory_support.cpp
    regression/default_cover_export_directory_catch2_tests.cpp
)

target_link_libraries(TagReaderDefaultCoverExportDirectoryCatch2Tests
    PRIVATE
        Catch2::Catch2WithMain
        TagReaderCore
)

tagreader_enable_sanitizers(TagReaderDefaultCoverExportDirectoryCatch2Tests)

catch_discover_tests(TagReaderDefaultCoverExportDirectoryCatch2Tests)

add_executable(TagReaderCoverProcessingContractCatch2Tests
    regression/catch2_regression_support.cpp
    regression/catch2_sample_support.cpp
    regression/cover_processing_contract_catch2_tests.cpp
    regression/cover_mode_processing_catch2_tests.cpp
    regression/cover_format_fixtures.cpp
    regression/cover_format_matrix_catch2_tests.cpp
)

target_link_libraries(TagReaderCoverProcessingContractCatch2Tests
    PRIVATE
        Catch2::Catch2WithMain
        TagReaderCore
)

target_include_directories(TagReaderCoverProcessingContractCatch2Tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

tagreader_enable_sanitizers(TagReaderCoverProcessingContractCatch2Tests)

catch_discover_tests(TagReaderCoverProcessingContractCatch2Tests)

add_executable(TagReaderSidecarCoverCatch2Tests
    regression/catch2_regression_support.cpp
    regression/catch2_sample_support.cpp
    regression/sidecar_cover_catch2_tests.cpp
)

target_link_libraries(TagReaderSidecarCoverCatch2Tests
    PRIVATE
        Catch2::Catch2WithMain
        TagReaderCore
)

target_include_directories(TagReaderSidecarCoverCatch2Tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

tagreader_enable_sanitizers(TagReaderSidecarCoverCatch2Tests)

catch_discover_tests(TagReaderSidecarCoverCatch2Tests)

add_executable(TagReaderFolderCoverCatch2Tests
    regression/catch2_regression_support.cpp
    regression/catch2_sample_support.cpp
    regression/folder_cover_catch2_tests.cpp
)

target_link_libraries(TagReaderFolderCoverCatch2Tests
    PRIVATE
        Catch2::Catch2WithMain
        TagReaderCore
)

target_include_directories(TagReaderFolderCoverCatch2Tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

tagreader_enable_sanitizers(TagReaderFolderCoverCatch2Tests)

catch_discover_tests(TagReaderFolderCoverCatch2Tests TEST_PREFIX "TagReaderFolderCover.")

add_executable(TagReaderCueCatch2Tests
    regression/catch2_regression_support.cpp
    regression/catch2_sample_support.cpp
    regression/cue_catch2_tests.cpp
    regression/cue_cover_options_catch2_tests.cpp
)

target_link_libraries(TagReaderCueCatch2Tests
    PRIVATE
        Catch2::Catch2WithMain
        TagReaderCore
)

target_include_directories(TagReaderCueCatch2Tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/formats/cue
)

tagreader_enable_sanitizers(TagReaderCueCatch2Tests)

catch_discover_tests(TagReaderCueCatch2Tests)

add_executable(TagReaderCuePathCatch2Tests
    regression/catch2_regression_support.cpp
    regression/cue_path_catch2_tests.cpp
)

target_link_libraries(TagReaderCuePathCatch2Tests
    PRIVATE
        Catch2::Catch2WithMain
        TagReaderCore
)

target_include_directories(TagReaderCuePathCatch2Tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/formats/cue
)

tagreader_enable_sanitizers(TagReaderCuePathCatch2Tests)

catch_discover_tests(TagReaderCuePathCatch2Tests)

add_executable(TagReaderCueMappingCatch2Tests
    regression/catch2_regression_support.cpp
    regression/catch2_sample_support.cpp
    regression/cue_mapping_catch2_tests.cpp
)

target_link_libraries(TagReaderCueMappingCatch2Tests
    PRIVATE
        Catch2::Catch2WithMain
        TagReaderCore
)

target_include_directories(TagReaderCueMappingCatch2Tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/formats/cue
)

tagreader_enable_sanitizers(TagReaderCueMappingCatch2Tests)

catch_discover_tests(TagReaderCueMappingCatch2Tests)

add_executable(TagReaderCueTimingCatch2Tests
    regression/catch2_regression_support.cpp
    regression/catch2_sample_support.cpp
    regression/cue_timing_catch2_tests.cpp
)

target_link_libraries(TagReaderCueTimingCatch2Tests
    PRIVATE
        Catch2::Catch2WithMain
        TagReaderCore
)

target_include_directories(TagReaderCueTimingCatch2Tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/formats/cue
)

tagreader_enable_sanitizers(TagReaderCueTimingCatch2Tests)

catch_discover_tests(TagReaderCueTimingCatch2Tests)

add_executable(TagReaderLyricsNormalizeComplexityCatch2Tests
    catch2/lyrics_normalize_complexity_catch2_tests.cpp
)

target_link_libraries(TagReaderLyricsNormalizeComplexityCatch2Tests
    PRIVATE
        Catch2::Catch2WithMain
        TagReaderCore
)

tagreader_enable_sanitizers(TagReaderLyricsNormalizeComplexityCatch2Tests)

target_include_directories(TagReaderLyricsNormalizeComplexityCatch2Tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

catch_discover_tests(TagReaderLyricsNormalizeComplexityCatch2Tests)

add_executable(TagReaderTrAudit001031Catch2Tests
    regression/tr_audit_001_031_catch2_tests.cpp
)

target_link_libraries(TagReaderTrAudit001031Catch2Tests
    PRIVATE
        Catch2::Catch2WithMain
        TagReaderCore
)

tagreader_enable_sanitizers(TagReaderTrAudit001031Catch2Tests)

target_include_directories(TagReaderTrAudit001031Catch2Tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

catch_discover_tests(TagReaderTrAudit001031Catch2Tests)

add_executable(TagReaderTrAudit032056Catch2Tests
    regression/tr_audit_032_056_catch2_tests.cpp
)

target_link_libraries(TagReaderTrAudit032056Catch2Tests
    PRIVATE
        Catch2::Catch2WithMain
        TagReaderCore
)

tagreader_enable_sanitizers(TagReaderTrAudit032056Catch2Tests)

target_include_directories(TagReaderTrAudit032056Catch2Tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

catch_discover_tests(TagReaderTrAudit032056Catch2Tests)

if(TARGET TagReaderFuzz)
    set(TAGREADER_FUZZ_CORPUS_DIR ${CMAKE_CURRENT_BINARY_DIR}/fuzz/corpus)

    add_test(NAME TagReaderFuzzGenerateCorpus
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/corpus/generate_corpus.py --out-dir ${TAGREADER_FUZZ_CORPUS_DIR}
    )

    set_tests_properties(TagReaderFuzzGenerateCorpus PROPERTIES
        LABELS "fuzz"
        FIXTURES_SETUP "tagreader_fuzz_corpus"
        RESOURCE_LOCK "tagreader_fuzz_workspace"
    )

    add_test(NAME TagReaderFuzzBoundedSmoke
        COMMAND TagReaderFuzz -runs=100 ${TAGREADER_FUZZ_CORPUS_DIR}
    )

    set_tests_properties(TagReaderFuzzBoundedSmoke PROPERTIES
        LABELS "fuzz"
        FIXTURES_REQUIRED "tagreader_fuzz_corpus"
        RESOURCE_LOCK "tagreader_fuzz_workspace"
    )
endif()
