#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ftxui/component/component.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <unordered_set>
#include <vector>

using namespace ftxui;
using json = nlohmann::json;

class Conn {
private:
  std::string _buf;
  int _sock;

public:
  Conn(const std::string &path) {
    _sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (_sock == -1) {
      return;
    }
    sockaddr_un addr = {};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
    int cn = connect(_sock, reinterpret_cast<struct sockaddr *>(&addr),
                     sizeof(addr));
    if (cn == -1) {
      return;
    }
  }

  int sendCmd(const std::string &msg) {
    std::string line = msg + "\n";
    return write(_sock, line.c_str(), line.size());
  }

  std::string readEvent() {
    size_t pos = _buf.find('\n');
    if (pos != std::string::npos) {
      std::string line = _buf.substr(0, pos);
      _buf.erase(0, pos + 1);
      return line;
    }

    char chunk[4096];
    while (true) {
      ssize_t n = read(_sock, chunk, sizeof(chunk));
      if (n <= 0)
        return "";
      _buf.append(chunk, n);
      pos = _buf.find('\n');
      if (pos != std::string::npos) {
        std::string line = _buf.substr(0, pos);
        _buf.erase(0, pos + 1);
        return line;
      }
    }
  }

  ~Conn() { close(_sock); }
};

struct Local {
  std::string name;
  std::string type;
  std::string value;
};

struct StackEntry {
  std::string value;
  std::string type;
};

enum DbgScreen {
  source = 0,
  locals = 1,
  stack = 2,
  output = 3,
};

struct DebugState {
  std::string event;
  std::string file;
  int line = 0;
  std::string reason;
  std::vector<std::string> source;
  int cursor = 1;
  std::unordered_set<int> bps;
  char pending_key = 0;
  std::vector<Local> locals;
  std::vector<StackEntry> stack;
  DbgScreen screen = DbgScreen::source; // 0 = source, 1 = locals
  bool focus_cursor = false;
  std::vector<std::string> output;
};

Local parseLocal(json j) {
  Local local;
  local.name = j["Name"];
  local.type = j["Type"];
  local.value = j["Value"];
  return local;
}

StackEntry parseStack(json j) {
  StackEntry stack;
  stack.value = j["Value"];
  stack.type = j["Type"];
  return stack;
}

std::vector<std::string> split_lines(const std::string &content) {
  std::vector<std::string> lines;
  std::istringstream stream(content);
  std::string line;
  while (std::getline(stream, line)) {
    lines.push_back(line);
  }
  return lines;
}

Element renderSource(const DebugState &state) {
  Elements rows;
  for (int i = 0; i < static_cast<int>(state.source.size()); i++) {
    int lineno = i + 1;
    const bool has_bp = state.bps.contains(lineno);
    auto bp = text(has_bp ? "● " : "  ") | color(Color::Red);

    auto num = text(std::to_string(lineno)) | size(WIDTH, EQUAL, 4) |
               color(Color::GrayDark);
    auto code = text(state.source[i]);
    auto row = hbox({bp, num, text(" "), code});
    if (lineno == state.line) {
      row = row | inverted;
      if (!state.focus_cursor) {
        row |= focus;
      }
    }

    if (lineno == state.cursor) {
      row |= underlined;
      if (state.focus_cursor) {
        row |= focus;
      }
    }
    rows.push_back(row);
  }

  const auto status_text =
      state.file.empty()
          ? std::string("waiting...")
          : state.file + ":" + std::to_string(state.line) + "  " + state.reason;

  const auto key_hint = state.pending_key
                            ? std::string(1, state.pending_key) + "-"
                            : std::string("");

  return vbox({text("sydney-dbg") | bold, separator(),
               vbox(std::move(rows)) | frame | flex, separator(),
               hbox({text(status_text), text("     "), text(key_hint)})}) |
         border | flex;
}

Element renderLocals(const DebugState &state) {
  Elements locals;
  for (int i = 0; i < static_cast<int>(state.locals.size()); i++) {
    auto num = text(std::to_string(i)) | size(WIDTH, EQUAL, 4) |
               color(Color::GrayDark);
    Element name = text(state.locals[i].name);
    Element type = text(state.locals[i].type);
    Element value = text(state.locals[i].value);
    locals.push_back(hbox({name, type, value}));
  }

  return vbox(hbox(text("sydney-dbg") | bold, separator(),
                   text("locals") | bold),
              vbox(std::move(locals) | frame | flex, separator())) |
         border | flex;
}

Element renderStack(const DebugState &state) {
  Elements stack;
  for (int i = 0; i < static_cast<int>(state.stack.size()); i++) {
    auto num = text(std::to_string(i)) | size(WIDTH, EQUAL, 4) |
               color(Color::GrayDark);
    Element type = text(state.stack[i].type);
    Element value = text(state.stack[i].value);
    stack.push_back(hbox({type, value}));
  }

  return vbox(
             hbox(text("sydney-dbg") | bold, separator(), text("stack") | bold),
             vbox(std::move(stack) | frame | flex, separator())) |
         border | flex;
}

Element renderOutput(const DebugState &state) {
  Elements lines;
  for (auto line : state.output) {
    auto txt = text(line) | color(Color::GrayDark);
    lines.push_back(txt);
  }

  return vbox(hbox(text("sydney-dbg") | bold, separator(),
                   text("outputs") | bold),
              separator(), vbox(std::move(lines)) | frame | flex) |
         border | flex;
}

int main(const int argc, char *argv[]) {
  if (argc < 2) {
    std::cerr << "usage sydney-dbg <program-path>\n";
    return 1;
  }

  const std::string path(argv[1]);
  int pipefd[2];
  pipe(pipefd);

  pid_t pid = fork();
  if (pid == -1) {
    std::cerr << "unable to spawn child process\n";
    return 1;
  } else if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    close(pipefd[1]);
    int res = execlp("sydney", "sydney", "debug", path.c_str());
    std::cerr << "failed to start sydney";
    return 1;
  }

  close(pipefd[1]);

  std::string sock_path = "/tmp/sydney-debug-" + std::to_string(pid) + ".sock";

  for (int i = 0; i < 100; i++) {
    if (access(sock_path.c_str(), F_OK) == 0) {
      break;
    }
    usleep(50000);
  }

  Conn conn(sock_path);

  std::mutex mtx;
  DebugState state;

  auto screen = ScreenInteractive::Fullscreen();

  bool source_loaded = false;

  std::thread interceptor([&] {
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
      std::string chunk(buf, n);
      auto lines = split_lines(chunk);
      std::lock_guard lock(mtx);
      for (auto &l : lines) {
        l.erase(std::remove(l.begin(), l.end(), '\r'), l.end());
        if (!l.empty()) {
          state.output.push_back(std::move(l));
        }
      }
      screen.PostEvent(Event::Custom);
    }
    close(pipefd[0]);
  });

  std::thread reader([&] {
    while (true) {
      auto line = conn.readEvent();
      if (line.empty())
        break;
      auto j = json::parse(line);
      std::string event = j["event"];

      if (event == "stopped") {
        if (!source_loaded) {
          auto cmd = json({{"cmd", "get_source"}, {"file", j["file"]}}).dump();
          conn.sendCmd(cmd);
        }
        std::lock_guard<std::mutex> lock(mtx);
        state.event = event;
        state.file = j["file"];
        state.line = j["line"];
        state.cursor = state.line;
        state.reason = j["reason"];
        state.focus_cursor = true;
        screen.PostEvent(Event::Custom);
      } else if (event == "response" && j["type"] == "source") {
        std::lock_guard<std::mutex> lock(mtx);
        state.source = split_lines(j["content"]);
        source_loaded = true;
        screen.PostEvent(Event::Custom);
      } else if (event == "response" && j["type"] == "locals") {
        std::vector<Local> locals;
        for (auto jj : j["data"]) {
          locals.push_back(parseLocal(std::move(jj)));
        }
        std::lock_guard<std::mutex> lock(mtx);
        state.locals = locals;
        screen.PostEvent(Event::Custom);
      } else if (event == "response" && j["type"] == "stack") {
        std::vector<StackEntry> stack;
        for (auto jj : j["data"]) {
          stack.push_back(parseStack(std::move(jj)));
        }
        state.stack = stack;
        screen.PostEvent(Event::Custom);
      }
    }
  });

  const auto component = Renderer([&] {
    std::lock_guard lock(mtx);

    if (state.screen == source) {
      return renderSource(state);
    }

    if (state.screen == locals) {
      return renderLocals(state);
    }

    if (state.screen == output) {
      return renderOutput(state);
    }

    return renderStack(state);
  });

  const auto with_keys = CatchEvent(component, [&](const Event &event) {
    if (event.is_character()) {
      std::lock_guard lock(mtx);
      const char ch = event.character()[0];

      // handle second key of a 2-char command
      if (state.pending_key == 's') {
        state.pending_key = 0;
        if (ch == 'o') {
          conn.sendCmd(json({{"cmd", "step_over"}}).dump());
        } else if (ch == 'i') {
          conn.sendCmd(json({{"cmd", "step_in"}}).dump());
        } else if (ch == 'u') {
          conn.sendCmd(json({{"cmd", "step_out"}}).dump());
        } else if (ch == 't' && state.screen != stack) {
          conn.sendCmd(json({{"cmd", "get_stack"}}).dump());
          state.stack.clear();
          state.screen = stack;
        }
        return true;
      }

      // start a 2-char command
      if (ch == 's') {
        state.pending_key = 's';
        return true;
      }

      // single key commands
      if (ch == 'n') {
        conn.sendCmd(json({{"cmd", "step_line"}}).dump());
        return true;
      }
      if (ch == 'c') {
        conn.sendCmd(json({{"cmd", "continue"}}).dump());
        return true;
      }
      if (ch == 'q') {
        screen.Exit();
        return true;
      }
      if (ch == 'b') {
        if (state.bps.contains(state.cursor)) {
          state.bps.erase(state.cursor);
          conn.sendCmd(json({{"cmd", "remove_breakpoint"},
                             {"file", state.file},
                             {"line", state.cursor}})
                           .dump());
        } else {
          state.bps.insert(state.cursor);
          conn.sendCmd(json({{"cmd", "set_breakpoint"},
                             {"file", state.file},
                             {"line", state.cursor}})
                           .dump());
        }
        return true;
      }
      if (ch == 'l' && state.screen != locals) {
        conn.sendCmd(json({{"cmd", "get_locals"}}).dump());
        state.locals.clear();
        state.screen = locals;
        return true;
      }

      if (ch == 'd' && state.screen != source) {
        state.locals.clear();
        state.screen = source;
        return true;
      }

      if (ch == 'o' && state.screen != output) {
        state.screen = output;
        return true;
      }

      return true;
    }

    if (event == Event::ArrowDown) {
      std::lock_guard lock(mtx);
      if (state.cursor < static_cast<int>(state.source.size())) {
        state.cursor++;
      }
      return true;
    }

    if (event == Event::ArrowUp) {
      std::lock_guard lock(mtx);
      if (state.cursor > 1) {
        state.cursor--;
      }
      return true;
    }

    return false;
  });

  screen.Loop(with_keys);
  return 0;
}
