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

add_executable(speech_effects_test speech_effects_test.cpp)
target_include_directories(speech_effects_test PRIVATE ../main)
target_compile_features(speech_effects_test PRIVATE cxx_std_17)
add_test(NAME speech_effects_test COMMAND $<TARGET_FILE:speech_effects_test>)

add_executable(server_endpoint_test server_endpoint_test.cpp)
target_include_directories(server_endpoint_test PRIVATE ../main)
target_compile_features(server_endpoint_test PRIVATE cxx_std_17)
add_test(NAME server_endpoint_test COMMAND $<TARGET_FILE:server_endpoint_test>)

add_executable(server_boot_policy_test server_boot_policy_test.cpp)
target_include_directories(server_boot_policy_test PRIVATE ../main)
target_compile_features(server_boot_policy_test PRIVATE cxx_std_17)
add_test(NAME server_boot_policy_test COMMAND $<TARGET_FILE:server_boot_policy_test>)

add_executable(servo_reply_parser_test servo_reply_parser_test.cpp)
target_include_directories(servo_reply_parser_test PRIVATE ../main/hal/drivers/FTServo_Arduino/src)
target_compile_features(servo_reply_parser_test PRIVATE cxx_std_17)
add_test(NAME servo_reply_parser_test COMMAND $<TARGET_FILE:servo_reply_parser_test>)

add_executable(tracking_step_test tracking_step_test.cpp)
target_include_directories(tracking_step_test PRIVATE ../main/stackchan/motion)
target_compile_features(tracking_step_test PRIVATE cxx_std_17)
add_test(NAME tracking_step_test COMMAND $<TARGET_FILE:tracking_step_test>)

add_executable(perception_mailbox_test perception_mailbox_test.cpp)
target_include_directories(perception_mailbox_test PRIVATE ../main/stackchan/port_stage1)
target_compile_features(perception_mailbox_test PRIVATE cxx_std_17)
add_test(NAME perception_mailbox_test COMMAND $<TARGET_FILE:perception_mailbox_test>)

add_executable(face_filter_guard_test face_filter_guard_test.cpp)
target_include_directories(face_filter_guard_test PRIVATE ../main/stackchan/port_stage1)
target_compile_features(face_filter_guard_test PRIVATE cxx_std_17)
add_test(NAME face_filter_guard_test COMMAND $<TARGET_FILE:face_filter_guard_test>)


add_executable(charge_protection_test charge_protection_test.cpp)
target_include_directories(charge_protection_test PRIVATE ../main)
target_compile_features(charge_protection_test PRIVATE cxx_std_17)
add_test(NAME charge_protection_test COMMAND $<TARGET_FILE:charge_protection_test>)

add_executable(low_battery_test low_battery_test.cpp)
target_include_directories(low_battery_test PRIVATE ../main)
target_compile_features(low_battery_test PRIVATE cxx_std_17)
add_test(NAME low_battery_test COMMAND $<TARGET_FILE:low_battery_test>)

add_executable(random_led_test random_led_test.cpp)
target_include_directories(random_led_test PRIVATE ../main)
target_compile_features(random_led_test PRIVATE cxx_std_20)
add_test(NAME random_led_test COMMAND $<TARGET_FILE:random_led_test>)
