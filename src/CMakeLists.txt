add_library(mde_common INTERFACE)
target_include_directories(mde_common INTERFACE ${PROJECT_SOURCE_DIR}/include)
target_compile_features(mde_common INTERFACE cxx_std_20)

add_library(mde_itch INTERFACE)
target_link_libraries(mde_itch INTERFACE mde_common)

add_library(mde_book STATIC book/reference_market.cpp)
target_link_libraries(mde_book PUBLIC mde_itch PRIVATE mde_warnings)

add_library(mde_synth STATIC synth/generator.cpp)
target_link_libraries(mde_synth PUBLIC mde_common PRIVATE mde_book mde_itch mde_warnings)

add_library(mde_moldudp64 STATIC moldudp64/packet.cpp moldudp64/replay.cpp)
target_link_libraries(mde_moldudp64 PUBLIC mde_common PRIVATE mde_warnings)

add_library(mde_net STATIC net/udp.cpp net/session.cpp)
target_link_libraries(mde_net PUBLIC mde_moldudp64 PRIVATE mde_warnings)
