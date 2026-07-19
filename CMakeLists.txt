cmake_minimum_required(VERSION 3.8)
project(my_offboard_cpp)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# find dependencies
find_package(ament_cmake REQUIRED)
find_package(rclcpp REQUIRED)
find_package(px4_msgs REQUIRED)
find_package(rosidl_default_generators REQUIRED)
find_package(std_srvs REQUIRED)

# px4_msgs release branches currently share the same package.xml version, so tests
# validate the installed message schema rather than relying on a version constraint.
get_filename_component(PX4_MSGS_SHARE_DIR "${px4_msgs_DIR}/.." ABSOLUTE)

rosidl_generate_interfaces(${PROJECT_NAME}
  "srv/GotoGlobal.srv"
)

add_executable(my_offboard_node src/my_offboard_node.cpp)
add_executable(lr24_command_node src/lr24_command_node.cpp)
add_executable(global_goto_node src/global_goto_node.cpp)

ament_target_dependencies(my_offboard_node
  rclcpp
  px4_msgs
  std_srvs
)

ament_target_dependencies(lr24_command_node
  rclcpp
  std_srvs
)

ament_target_dependencies(global_goto_node
  rclcpp
  px4_msgs
  std_srvs
)

rosidl_get_typesupport_target(cpp_typesupport_target
  ${PROJECT_NAME} "rosidl_typesupport_cpp")
target_link_libraries(lr24_command_node "${cpp_typesupport_target}")
target_link_libraries(global_goto_node "${cpp_typesupport_target}")

install(TARGETS
  my_offboard_node
  lr24_command_node
  global_goto_node
  DESTINATION lib/${PROJECT_NAME}
)

install(DIRECTORY
  launch
  DESTINATION share/${PROJECT_NAME}
)

if(BUILD_TESTING)
  find_package(ament_lint_auto REQUIRED)
  find_package(ament_cmake_pytest REQUIRED)
  # the following line skips the linter which checks for copyrights
  # comment the line when a copyright and license is added to all source files
  set(ament_cmake_copyright_FOUND TRUE)
  # the following line skips cpplint (only works in a git repo)
  # comment the line when this package is in a git repo and when
  # a copyright and license is added to all source files
  set(ament_cmake_cpplint_FOUND TRUE)
  ament_lint_auto_find_test_dependencies()
  ament_add_pytest_test(test_send_lr24_command
    test/test_send_lr24_command.py
  )
  ament_add_pytest_test(test_px4_msgs_115_schema
    test/test_px4_msgs_115_schema.py
  )
  set_tests_properties(test_px4_msgs_115_schema PROPERTIES
    ENVIRONMENT "PX4_MSGS_DIR=${PX4_MSGS_SHARE_DIR}/msg"
  )
endif()

ament_export_dependencies(rosidl_default_runtime)
ament_package()
