add_library(imageeditor_mcp STATIC
    src/ToolRegistry.cpp
    src/Protocol.cpp
    src/Bridge.cpp
    src/Integration.cpp
    src/LocalServer.cpp)
target_include_directories(imageeditor_mcp PUBLIC include)
target_link_libraries(imageeditor_mcp PUBLIC Qt6::Core Qt6::Network)
target_compile_definitions(imageeditor_mcp PRIVATE VULKANA_MCP_VERSION="${VULKANA_VERSION}")
imageeditor_enable_warnings(imageeditor_mcp)

add_executable(vulkana-mcp src/main.cpp)
target_link_libraries(vulkana-mcp PRIVATE imageeditor_mcp)
target_compile_definitions(vulkana-mcp PRIVATE VULKANA_MCP_VERSION="${VULKANA_VERSION}")
imageeditor_enable_warnings(vulkana-mcp)
