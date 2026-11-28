#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

import config;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
  if (!condition) {
    std::printf("FAIL: %s\n", message);
    ++failures;
  }
}

} // namespace

int main() {
  auto port = pants::config::lookup<int>("server.port", 8080, "listen port");
  check(port != nullptr, "lookup should create the variable");
  check(port->get() == 8080, "default value should be used");
  check(port->name() == "server.port", "name should be preserved");
  check(port->description() == "listen port", "description should be preserved");

  auto name = pants::config::lookup<std::string>("server.name", std::string{"unknown"});
  check(name != nullptr, "string variable should be created");

  int changes = 0;
  int previous = 0;
  int current = 0;
  const pants::listener_id id = port->add_listener([&](const int& old_value, const int& new_value) {
    ++changes;
    previous = old_value;
    current = new_value;
  });
  check(id != 0, "listener id should be valid");

  check(port->set(9090), "first set should change the value");
  check(changes == 1 && previous == 8080 && current == 9090, "listener should observe the change");
  check(!port->set(9090), "setting the same value should be ignored");
  check(changes == 1, "listener should not fire for the same value");

  port->remove_listener(id);
  check(port->set(7070), "second set should change the value");
  check(changes == 1, "removed listener should not fire");

  pants::config::load_string(R"(
[server]
port = 1234
name = "pants"
)");

  check(port->get() == 1234, "toml value should be applied");
  check(name->get() == "pants", "toml string value should be applied");

  check(pants::config::is_valid_name("server.port"), "dotted name should be valid");
  check(!pants::config::is_valid_name(".server"), "leading dot should be invalid");
  check(!pants::config::is_valid_name("server..port"), "empty segment should be invalid");
  check(!pants::config::is_valid_name("server?port"), "invalid character should be rejected");

  check(pants::config::size() == 2, "two variables should be registered");
  check(pants::config::lookup<std::string>("server.port", std::string{}) == nullptr,
        "lookup with a different type should fail");

  int visited = 0;
  pants::config::visit([&](const pants::config_var_base::shared& variable) {
    (void)variable;
    ++visited;
  });
  check(visited == 2, "visit should see every variable");

  check(!pants::config::lookup<int>("bad..name", 0), "invalid name lookup should fail");

  if (failures != 0) {
    std::printf("%d check(s) failed\n", failures);
    return 1;
  }
  std::puts("config ok");
  return 0;
}
