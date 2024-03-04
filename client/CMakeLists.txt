# CMakeList.txt: проект CMake для updater; включите исходный код и определения,
# укажите здесь логику для конкретного проекта.
#
cmake_minimum_required (VERSION 3.8)

project ("client")
find_package(ixwebsocket CONFIG REQUIRED)
find_package(ZLIB REQUIRED)
find_package(spdlog CONFIG REQUIRED)
   
# Добавьте источник в исполняемый файл этого проекта.
add_executable (client "client.cpp" "client.h" "json.hpp")
target_link_libraries(${PROJECT_NAME} PRIVATE ixwebsocket::ixwebsocket )
target_link_libraries(${PROJECT_NAME} PRIVATE ZLIB::ZLIB)
target_link_libraries(${PROJECT_NAME} PRIVATE spdlog::spdlog)
set_property(TARGET ${PROJECT_NAME} PROPERTY CXX_STANDARD 17)
# TODO: Добавьте тесты и целевые объекты, если это необходимо.
