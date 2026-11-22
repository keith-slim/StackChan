cmake_minimum_required(VERSION 3.16)

project(stackchan_firmware_tests LANGUAGES CXX)

enable_testing()

add_executable(motion_math_test
    motion_math_test.cpp
    ../main/stackchan/motion/motion_math.cpp
)

target_include_directories(motion_math_test PRIVATE
    ../main
)

target_compile_features(motion_math_test PRIVATE cxx_std_17)

add_test(NAME motion_math_test COMMAND $<TARGET_FILE:motion_math_test>)

add_executable(timed_servo_move_test timed_servo_move_test.cpp)
target_include_directories(timed_servo_move_test PRIVATE ../main)
target_compile_features(timed_servo_move_test PRIVATE cxx_std_17)
add_test(NAME timed_servo_move_test COMMAND $<TARGET_FILE:timed_servo_move_test>)

add_executable(event_timer_test event_timer_test.cpp)
target_include_directories(event_timer_test PRIVATE ../main)
target_compile_features(event_timer_test PRIVATE cxx_std_17)
add_test(NAME event_timer_test COMMAND $<TARGET_FILE:event_timer_test>)
