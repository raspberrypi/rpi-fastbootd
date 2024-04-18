set(SOURCES
    AsyncIO.cpp
)

add_library(asyncio STATIC ${SOURCES})
target_include_directories(
    asyncio
    PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/include
)
